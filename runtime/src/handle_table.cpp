#include "rcomp/runtime/handle_table.h"
#include <new>

namespace rcomp::rt {

const char* handle_kind_name(HandleKind k) {
    switch (k) {
    case HandleKind::None: return "None";
    case HandleKind::File: return "File";
    case HandleKind::Event: return "Event";
    case HandleKind::Mutex: return "Mutex";
    case HandleKind::Thread: return "Thread";
    case HandleKind::Semaphore: return "Semaphore";
    case HandleKind::Notification: return "Notification";
    case HandleKind::Enumerator: return "Enumerator";
    case HandleKind::Timer: return "Timer";
    case HandleKind::Task: return "Task";
    case HandleKind::Session: return "Session";
    case HandleKind::IoCompletion: return "IoCompletion";
    case HandleKind::Test: return "Test";
    }
    return "?";
}

HandleTable::HandleTable(uint32_t max_slots)
    : max_slots_(max_slots == 0 || max_slots > kMaxSlots ? kMaxSlots : max_slots) {}

bool HandleTable::decode(uint32_t handle, uint32_t* slot, uint8_t* gen) const {
    if ((handle & 0xF0000000u) != kHandleTag || (handle & 3u) != 0) return false;
    *slot = (handle >> 2) & (kMaxSlots - 1);
    *gen = (uint8_t)((handle >> 20) & 0xFF);
    return true;
}

Status HandleTable::allocate_slot_locked(uint32_t* slot) {
    if (!slot) return Status::InvalidArgument;
    if (!free_.empty()) {
        *slot = free_.back();
        free_.pop_back();
        return Status::Ok;
    }
    if (slots_.size() >= max_slots_) return Status::TableFull;
    *slot = (uint32_t)slots_.size();
#if defined(__cpp_exceptions)
    bool added = false;
    try {
        // Closing a published handle must not need host allocation. Keep
        // reuse capacity in step with slots_' geometric capacity before
        // publishing a new slot; free_ has at most one entry per slot.
        slots_.emplace_back();
        added = true;
        if (free_.capacity() < slots_.capacity()) free_.reserve(slots_.capacity());
    } catch (const std::bad_alloc&) {
        if (added) slots_.pop_back();
        return Status::OutOfMemory;
    }
#else
    slots_.emplace_back();
    if (free_.capacity() < slots_.capacity()) free_.reserve(slots_.capacity());
#endif
    return Status::Ok;
}

Status HandleTable::insert(std::shared_ptr<HandleObject> obj, uint32_t* out) {
    if (!obj || !out || obj->kind() == HandleKind::None) return Status::InvalidArgument;
    uint32_t slot = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const Status allocated = allocate_slot_locked(&slot);
        if (allocated != Status::Ok) return allocated;
        Slot& s = slots_[slot];
        s.obj = obj;
        ++live_;
        *out = kHandleTag | ((uint32_t)s.generation << 20) | (slot << 2);
    }
    obj->handle_opened();
    return Status::Ok;
}

Status HandleTable::lookup_any(uint32_t handle, std::shared_ptr<HandleObject>* out) const {
    uint32_t slot;
    uint8_t gen;
    if (!out || !decode(handle, &slot, &gen)) return Status::InvalidHandle;
    std::lock_guard<std::mutex> lock(mu_);
    if (slot >= slots_.size()) return Status::InvalidHandle;
    const Slot& s = slots_[slot];
    if (!s.obj || s.generation != gen) return Status::InvalidHandle;
    *out = s.obj;
    return Status::Ok;
}

Status HandleTable::lookup(uint32_t handle, HandleKind expected,
                           std::shared_ptr<HandleObject>* out) const {
    std::shared_ptr<HandleObject> object;
    Status status = lookup_any(handle, &object);
    if (status != Status::Ok) return status;
    if (object->kind() != expected) return Status::WrongHandleKind;
    *out = std::move(object);
    return Status::Ok;
}

Status HandleTable::close(uint32_t handle) {
    uint32_t slot;
    uint8_t gen;
    if (!decode(handle, &slot, &gen)) return Status::InvalidHandle;
    std::shared_ptr<HandleObject> dying;  // destroyed outside the lock
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (slot >= slots_.size()) return Status::InvalidHandle;
        Slot& s = slots_[slot];
        if (!s.obj || s.generation != gen) return Status::InvalidHandle;
        // insert() provisioned reuse capacity before this slot became
        // visible, so a valid close never allocates host metadata.
        if (s.generation != UINT8_MAX) free_.push_back(slot);
        dying = std::move(s.obj);
        // An exhausted slot is retired, never wrapped back to a stale handle.
        if (s.generation != UINT8_MAX) ++s.generation;
        --live_;
    }
    dying->handle_closed();
    return Status::Ok;
}

Status HandleTable::duplicate(uint32_t source, uint32_t* out) {
    if (!out) return Status::InvalidArgument;
    uint32_t source_slot;
    uint8_t source_gen;
    if (!decode(source, &source_slot, &source_gen)) return Status::InvalidHandle;
    std::shared_ptr<HandleObject> object;
    uint32_t target_slot = 0, target_handle = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (source_slot >= slots_.size()) return Status::InvalidHandle;
        const Slot& source_entry = slots_[source_slot];
        if (!source_entry.obj || source_entry.generation != source_gen) return Status::InvalidHandle;
        object = source_entry.obj;
        const Status allocated = allocate_slot_locked(&target_slot);
        if (allocated != Status::Ok) return allocated;
        Slot& target = slots_[target_slot];
        target.obj = object;
        ++live_;
        target_handle = kHandleTag | ((uint32_t)target.generation << 20) | (target_slot << 2);
    }
    object->handle_opened();
    *out = target_handle;
    return Status::Ok;
}

uint32_t HandleTable::live_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return live_;
}

}  // namespace rcomp::rt
