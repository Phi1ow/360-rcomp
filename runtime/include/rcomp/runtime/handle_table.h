// Typed guest handle table (owner: Agent 3, runtime/).
//
// Guest-visible handles are 32-bit values:
//
//   0xF0000000 | (generation & 0xFF) << 20 | slot << 2      slot < 2^18
//
// so they are never 0, never 0xFFFFFFFF (INVALID_HANDLE_VALUE), always
// 4-byte aligned, and a stale handle (closed, slot reused) is detected by its
// generation. A slot is retired when its eight-bit generation is exhausted;
// after max_slots*256 lifetimes the table is full even with no live objects.
// The encoding is local to one Runtime, not an inter-runtime identity.
// Lookups are kind-checked: a handle of another kind returns
// Status::WrongHandleKind, a closed/unknown one Status::InvalidHandle. Nothing
// ever "succeeds" on a bad handle.
//
// Objects are reference counted (shared_ptr): close() removes the handle but a
// thread that already looked the object up keeps it alive until it is done.
// Thread-safe.
#pragma once

#include <stdint.h>

#include <memory>
#include <mutex>
#include <vector>

#include "rcomp/runtime/status.h"

namespace rcomp::rt {

enum class HandleKind : uint8_t {
    None = 0,
    File = 1,
    Event = 2,
    Mutex = 3,
    Thread = 4,
    Semaphore = 5,
    Notification = 6,
    Enumerator = 8,
    Timer = 9,  // NtCreateTimer (src/hle_xboxkrnl_threads.cpp)
    Task = 10,     // XamTaskSchedule (src/hle_xam_misc.cpp)
    Session = 11,  // XamSessionCreateHandle (src/hle_xam_misc.cpp)
    IoCompletion = 12,  // NtCreateIoCompletion (src/hle_xboxkrnl_threads.cpp)
    Test = 0x7F,  // reserved for runtime/tests
};

const char* handle_kind_name(HandleKind k);

class HandleObject {
public:
    virtual ~HandleObject() = default;
    virtual HandleKind kind() const = 0;
    // Optional guest object identity. Most runtime handles do not expose a
    // guest Body pointer; thread handles override this once their object
    // identity has been created.
    virtual uint32_t guest_object_body() const { return 0; }
    // Lifecycle notifications are host bookkeeping only. They must not fail
    // or allocate: insert/duplicate publishes the handle before opened(), and
    // close invalidates it before closed().
    virtual void handle_opened() {}
    virtual void handle_closed() {}
    // Non-consuming manual readiness, called under the dispatcher mutex.
    // Overrides must not block, allocate or acquire another lock.
    virtual bool dispatcher_ready() const { return false; }
};

class HandleTable {
public:
    static constexpr uint32_t kHandleTag = 0xF0000000u;
    static constexpr uint32_t kMaxSlots = 1u << 18;

    explicit HandleTable(uint32_t max_slots = 4096);

    // Inserts `obj`; *out receives the guest handle.
    Status insert(std::shared_ptr<HandleObject> obj, uint32_t* out);
    // Kind-checked lookup.
    Status lookup(uint32_t handle, HandleKind expected, std::shared_ptr<HandleObject>* out) const;
    // Generic lookup used by object-manager operations that preserve the
    // object's actual type (for example NtDuplicateObject).
    Status lookup_any(uint32_t handle, std::shared_ptr<HandleObject>* out) const;
    template <class T>
    Status lookup_as(uint32_t handle, std::shared_ptr<T>* out) const {
        std::shared_ptr<HandleObject> o;
        Status s = lookup(handle, T::kKind, &o);
        if (s == Status::Ok) *out = std::static_pointer_cast<T>(o);
        return s;
    }
    // Removes the handle without allocating host metadata. Closing twice
    // returns InvalidHandle.
    Status close(uint32_t handle);
    // Creates a distinct handle to the same object. `source` must be an
    // ordinary live table handle; pseudo-handles are intentionally excluded.
    // Failure leaves both source and *out unchanged.
    Status duplicate(uint32_t source, uint32_t* out);
    uint32_t live_count() const;

private:
    struct Slot {
        std::shared_ptr<HandleObject> obj;
        uint8_t generation = 0;
    };
    bool decode(uint32_t handle, uint32_t* slot, uint8_t* gen) const;
    Status allocate_slot_locked(uint32_t* slot);

    mutable std::mutex mu_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    uint32_t max_slots_;
    uint32_t live_ = 0;
};

}  // namespace rcomp::rt
