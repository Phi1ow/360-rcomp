#include "rcomp/guest_memory.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "os_vm.h"

namespace rcomp {

const char* mem_status_name(MemStatus s) {
    switch (s) {
    case MemStatus::Ok: return "Ok";
    case MemStatus::InvalidArgument: return "InvalidArgument";
    case MemStatus::OutOfMemory: return "OutOfMemory";
    case MemStatus::NotReserved: return "NotReserved";
    case MemStatus::Conflict: return "Conflict";
    case MemStatus::PlatformError: return "PlatformError";
    }
    return "?";
}

namespace {
os::Prot to_os(Protect p) {
    switch (p) {
    case Protect::Read: return os::kRead;
    case Protect::ReadWrite: return os::kReadWrite;
    default: return os::kNone;
    }
}
bool range_ok(uint64_t addr, uint64_t size) {
    return size != 0 && (addr % kGuestPageSize) == 0 && (size % kGuestPageSize) == 0 &&
           addr < kGuestSpaceSize && size <= kGuestSpaceSize - addr;
}
bool valid_protect(Protect prot) {
    return prot == Protect::None || prot == Protect::Read || prot == Protect::ReadWrite;
}
void set_access(uint64_t* readable, uint64_t* writable, uint64_t page, Protect prot) {
    const uint64_t bit = 1ull << (page % 64);
    if (prot == Protect::Read || prot == Protect::ReadWrite) readable[page / 64] |= bit;
    else readable[page / 64] &= ~bit;
    if (prot == Protect::ReadWrite) writable[page / 64] |= bit;
    else writable[page / 64] &= ~bit;
}
}  // namespace

GuestMemory::~GuestMemory() { release(); }

MemStatus GuestMemory::reserve() {
    if (base_) return MemStatus::InvalidArgument;
    if (os::vm_page_size() > kGuestPageSize) return MemStatus::PlatformError;
    reserve_size_ = kGuestSpaceSize + kGuestGuardSize;
    base_ = (uint8_t*)os::vm_reserve(reserve_size_, kBaseAlignment);
    if (!base_) {
        fprintf(stderr, "rcomp: guest reserve of 0x%llx bytes failed (errno %d)\n",
                (unsigned long long)reserve_size_, errno);
        reserve_size_ = 0;
        return MemStatus::OutOfMemory;
    }
    memset(committed_bits_, 0, sizeof committed_bits_);
    memset(readable_bits_, 0, sizeof readable_bits_);
    memset(writable_bits_, 0, sizeof writable_bits_);
    committed_bytes_ = peak_committed_bytes_ = 0;
    runtime_range_count_ = 0;
    return MemStatus::Ok;
}

void GuestMemory::release() {
    if (!base_) return;
    os::vm_release(base_, reserve_size_);
    base_ = nullptr;
    reserve_size_ = 0;
    memset(committed_bits_, 0, sizeof committed_bits_);
    memset(readable_bits_, 0, sizeof readable_bits_);
    memset(writable_bits_, 0, sizeof writable_bits_);
    committed_bytes_ = 0;
    runtime_range_count_ = 0;
}

bool GuestMemory::overlaps_runtime_range(uint64_t addr, uint64_t size) const {
    for (int i = 0; i < runtime_range_count_; ++i) {
        const auto& r = runtime_ranges_[i];
        if (addr < r.addr + r.size && r.addr < addr + size) return true;
    }
    return false;
}

bool GuestMemory::in_alias_window(uint64_t addr, uint64_t size) const {
    for (uint64_t alias : kPhysicalAliasBases)
        if (addr < alias + kPhysicalWindowSize && alias < addr + size) return true;
    return false;
}

// Views the part of [addr, addr+size) that lies in the physical window through
// the alias windows. Alias pages are marked committed for accessibility checks
// but never counted in committed_bytes_ (they own no backing).
MemStatus GuestMemory::alias_range(uint64_t addr, uint64_t size, Protect prot) {
    if (!aliasing_supported_) return MemStatus::Ok;
    const uint64_t lo = addr > kPhysicalWindowBase ? addr : kPhysicalWindowBase;
    const uint64_t hi_end = kPhysicalWindowBase + kPhysicalWindowSize;
    const uint64_t hi = addr + size < hi_end ? addr + size : hi_end;
    if (lo >= hi) return MemStatus::Ok;
    for (uint64_t alias : kPhysicalAliasBases) {
        const uint64_t dst = alias + (lo - kPhysicalWindowBase);
        if (!os::vm_alias(base_ + dst, base_ + lo, hi - lo, to_os(prot))) {
            if (errno == ENOSYS) {
                aliasing_supported_ = false;
                return MemStatus::Ok;
            }
            fprintf(stderr, "rcomp: physical alias 0x%llx failed (errno %d)\n", (unsigned long long)dst, errno);
            return MemStatus::PlatformError;
        }
        for (uint64_t p = dst / kGuestPageSize; p < (dst + (hi - lo)) / kGuestPageSize; ++p) {
            committed_bits_[p / 64] |= 1ull << (p % 64);
            set_access(readable_bits_, writable_bits_, p, prot);
        }
    }
    return MemStatus::Ok;
}

void GuestMemory::unalias_range(uint64_t addr, uint64_t size) {
    const uint64_t lo = addr > kPhysicalWindowBase ? addr : kPhysicalWindowBase;
    const uint64_t hi_end = kPhysicalWindowBase + kPhysicalWindowSize;
    const uint64_t hi = addr + size < hi_end ? addr + size : hi_end;
    if (lo >= hi) return;
    for (uint64_t alias : kPhysicalAliasBases) {
        const uint64_t dst = alias + (lo - kPhysicalWindowBase);
        bool any = false;
        for (uint64_t p = dst / kGuestPageSize; p < (dst + (hi - lo)) / kGuestPageSize; ++p) {
            const uint64_t bit = 1ull << (p % 64);
            if (committed_bits_[p / 64] & bit) any = true;
            committed_bits_[p / 64] &= ~bit;
            set_access(readable_bits_, writable_bits_, p, Protect::None);
        }
        if (any) os::vm_unalias(base_ + dst, hi - lo);
    }
}

MemStatus GuestMemory::protect_alias_range(uint64_t addr, uint64_t size, Protect prot) {
    if (!aliasing_supported_) return MemStatus::Ok;
    const uint64_t lo = addr > kPhysicalWindowBase ? addr : kPhysicalWindowBase;
    const uint64_t hi_end = kPhysicalWindowBase + kPhysicalWindowSize;
    const uint64_t hi = addr + size < hi_end ? addr + size : hi_end;
    if (lo >= hi) return MemStatus::Ok;
    for (uint64_t alias : kPhysicalAliasBases) {
        const uint64_t dst = alias + (lo - kPhysicalWindowBase);
        if (!os::vm_protect(base_ + dst, hi - lo, to_os(prot))) return MemStatus::PlatformError;
        for (uint64_t p = dst / kGuestPageSize; p < (dst + (hi - lo)) / kGuestPageSize; ++p)
            set_access(readable_bits_, writable_bits_, p, prot);
    }
    return MemStatus::Ok;
}

MemStatus GuestMemory::commit_impl(uint64_t addr, uint64_t size, Protect prot) {
    if (!os::vm_commit(base_ + addr, size, to_os(prot))) {
        return errno == ENOMEM ? MemStatus::OutOfMemory : MemStatus::PlatformError;
    }
    for (uint64_t p = addr / kGuestPageSize; p < (addr + size) / kGuestPageSize; ++p) {
        uint64_t bit = 1ull << (p % 64);
        if (!(committed_bits_[p / 64] & bit)) {
            committed_bits_[p / 64] |= bit;
            committed_bytes_ += kGuestPageSize;
        }
        set_access(readable_bits_, writable_bits_, p, prot);
    }
    if (committed_bytes_ > peak_committed_bytes_) peak_committed_bytes_ = committed_bytes_;
    return alias_range(addr, size, prot);
}

MemStatus GuestMemory::commit(uint64_t addr, uint64_t size, Protect prot) {
    if (!base_) return MemStatus::NotReserved;
    if (!range_ok(addr, size) || !valid_protect(prot) || prot == Protect::None)
        return MemStatus::InvalidArgument;
    if (overlaps_runtime_range(addr, size)) return MemStatus::Conflict;
    if (in_alias_window(addr, size)) return MemStatus::InvalidArgument;
    return commit_impl(addr, size, prot);
}

MemStatus GuestMemory::decommit(uint64_t addr, uint64_t size) {
    if (!base_) return MemStatus::NotReserved;
    if (!range_ok(addr, size)) return MemStatus::InvalidArgument;
    if (overlaps_runtime_range(addr, size)) return MemStatus::Conflict;
    if (in_alias_window(addr, size)) return MemStatus::InvalidArgument;
    unalias_range(addr, size);
    if (!os::vm_decommit(base_ + addr, size)) return MemStatus::PlatformError;
    for (uint64_t p = addr / kGuestPageSize; p < (addr + size) / kGuestPageSize; ++p) {
        uint64_t bit = 1ull << (p % 64);
        if (committed_bits_[p / 64] & bit) {
            committed_bits_[p / 64] &= ~bit;
            committed_bytes_ -= kGuestPageSize;
        }
        set_access(readable_bits_, writable_bits_, p, Protect::None);
    }
    return MemStatus::Ok;
}

MemStatus GuestMemory::protect(uint64_t addr, uint64_t size, Protect prot) {
    if (!base_) return MemStatus::NotReserved;
    if (!range_ok(addr, size) || !valid_protect(prot)) return MemStatus::InvalidArgument;
    if (!is_committed(addr, size)) return MemStatus::InvalidArgument;
    if (overlaps_runtime_range(addr, size)) return MemStatus::Conflict;
    if (in_alias_window(addr, size)) return MemStatus::InvalidArgument;
    if (!os::vm_protect(base_ + addr, size, to_os(prot))) return MemStatus::PlatformError;
    for (uint64_t p = addr / kGuestPageSize; p < (addr + size) / kGuestPageSize; ++p)
        set_access(readable_bits_, writable_bits_, p, prot);
    return protect_alias_range(addr, size, prot);
}

bool GuestMemory::is_committed(uint64_t addr, uint64_t size) const {
    if (!base_ || size == 0 || addr >= kGuestSpaceSize || size > kGuestSpaceSize - addr) return false;
    for (uint64_t p = addr / kGuestPageSize; p <= (addr + size - 1) / kGuestPageSize; ++p) {
        if (!(committed_bits_[p / 64] & (1ull << (p % 64)))) return false;
    }
    return true;
}

bool GuestMemory::is_accessible(uint64_t guest, uint64_t size, Protect required) const {
    if (required != Protect::Read && required != Protect::ReadWrite) return false;
    const uint64_t addr = host_address(guest);
    if (!base_ || size == 0 || addr >= kGuestSpaceSize || size > kGuestSpaceSize - addr) return false;
    for (uint64_t p = addr / kGuestPageSize; p <= (addr + size - 1) / kGuestPageSize; ++p) {
        const uint64_t bit = 1ull << (p % 64);
        if (!(committed_bits_[p / 64] & readable_bits_[p / 64] & bit)) return false;
        if (required == Protect::ReadWrite && !(writable_bits_[p / 64] & bit)) return false;
    }
    return true;
}

MemStatus GuestMemory::reserve_runtime_range(uint64_t addr, uint64_t size, const char* tag) {
    if (!base_) return MemStatus::NotReserved;
    if (!range_ok(addr, size)) return MemStatus::InvalidArgument;
    if (overlaps_runtime_range(addr, size)) return MemStatus::Conflict;
    if (runtime_range_count_ == (int)(sizeof runtime_ranges_ / sizeof runtime_ranges_[0]))
        return MemStatus::OutOfMemory;
    if (!is_accessible(addr, size, Protect::ReadWrite)) {
        MemStatus s = commit_impl(addr, size, Protect::ReadWrite);
        if (s != MemStatus::Ok) return s;
    }
    runtime_ranges_[runtime_range_count_++] = {addr, size, tag};
    return MemStatus::Ok;
}

MemStatus GuestMemory::reserve_opaque_runtime_range(uint64_t addr, uint64_t size, const char* tag) {
    if (!base_) return MemStatus::NotReserved;
    if (!range_ok(addr, size) || !tag || !*tag) return MemStatus::InvalidArgument;
    for (int i = 0; i < runtime_range_count_; ++i) {
        const auto& range = runtime_ranges_[i];
        if (range.opaque && range.addr == addr && range.size == size &&
            range.tag && strcmp(range.tag, tag) == 0)
            return MemStatus::Ok;
    }
    if (overlaps_runtime_range(addr, size)) return MemStatus::Conflict;
    for (uint64_t page = addr / kGuestPageSize; page < (addr + size) / kGuestPageSize; ++page)
        if (committed_bits_[page / 64] & (1ull << (page % 64))) return MemStatus::Conflict;
    if (runtime_range_count_ == int(sizeof runtime_ranges_ / sizeof runtime_ranges_[0]))
        return MemStatus::OutOfMemory;
    runtime_ranges_[runtime_range_count_++] = {addr, size, tag, true};
    return MemStatus::Ok;
}

uint8_t* GuestMemory::translate(uint32_t addr, uint32_t size) const {
    const uint64_t host = host_address(addr);
    if (size == 0 || !is_committed(host, size)) return nullptr;
    return base_ + host;
}

GuestMemoryStats GuestMemory::stats() const {
    return {reserve_size_, committed_bytes_, peak_committed_bytes_};
}

}  // namespace rcomp
