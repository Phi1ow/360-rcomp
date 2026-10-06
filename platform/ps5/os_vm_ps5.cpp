// PS5 backend of platform/common/os_vm.h (owner: Agent 2).
//
// Why not os_vm_posix.cpp: on the PS5 every anonymous mmap, PROT_NONE
// reservations included, is charged to the title's flexible-memory budget
// (~448 MiB configured), so a 4 GiB + guard PROT_NONE mapping cannot succeed
// there (PS5_RetroArch PPSSPP_Implementation_Plan.md section 5, Dolphin port
// MemArenaUnix.cpp comment). FreeBSD also has no MAP_NORESERVE (the bit is
// MAP_RESERVED0040 in the SDK's sys/mman.h). See platform/ps5/README.md.
//
// Design (all console behaviour below is NOT TESTED by this project):
//  * vm_reserve: sceKernelReserveVirtualRange -- address space only, no
//    flexible or direct memory. Placed away from PS5_Vulkan's GPU window.
//  * vm_commit: for each run of not-yet-backed 64 KiB pages, one
//    sceKernelAllocateDirectMemory + sceKernelMapDirectMemory(MAP_FIXED) over
//    the reservation, zero-filled, then sceKernelMprotect to the requested
//    CPU protection. Already-backed pages keep their contents.
//  * vm_decommit: unmap each backed run, re-reserve the hole (MAP_FIXED) so
//    nothing else can be mapped into the guest range, release the direct
//    memory.
//  * Guest memory is never GPU-visible and never executable.
//
// Tracking granularity is 64 KiB (rcomp::kGuestPageSize); vm_commit and
// vm_decommit reject ranges that are not 64 KiB aligned. The per-reservation
// table holds one direct-memory offset per 64 KiB page (512 KiB for the guest
// space), allocated with plain mmap (flexible memory).
#include "../common/os_vm.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "os_vm_ps5.h"
#include "ps5_kernel.h"

namespace rcomp::os {

namespace sce = ::rcomp::ps5;  // kernel constants (ps5_kernel.h)

namespace {

constexpr size_t kTrackPage = 0x10000;  // 64 KiB, matches kGuestPageSize
constexpr int64_t kNoBacking = -1;
constexpr int64_t kNewTag = (int64_t)1 << 62;  // transient, see vm_commit
constexpr int kMaxReservations = 4;

// Placement hints, tried in order; the kernel refuses a hint it cannot honour
// instead of relocating it. 0x10_0000_0000 is where the Dolphin port reserved
// its fastmem arena on a console; the others are free space in the same
// region recorded by the reference projects (libkernel modules sit at
// 0x8_0000_0000). The last attempt lets the kernel choose, and is rejected if
// it lands in the GPU window.
constexpr uintptr_t kReserveHints[] = {
    0x1000000000ull,
    0x1800000000ull,
    0x2000000000ull,
    0x0400000000ull,
    0,
};

struct Reservation {
    uintptr_t base;
    size_t size;
    int64_t* backing;  // kNoBacking or direct-memory offset, one per 64 KiB page
    size_t pages;
    size_t table_bytes;
};

Reservation g_res[kMaxReservations];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Largest direct-memory allocation made for one run. SIZE_MAX = one allocation
// per contiguous run. Setting it to kTrackPage removes the assumption that
// sceKernelReleaseDirectMemory accepts a sub-range of an allocation.
size_t g_max_run_bytes = (size_t)-1;
// Direct memory currently held by all reservations (bytes), for leak checks.
uint64_t g_direct_bytes = 0;

bool sce_ok(int32_t r) {
    if (r == 0) return true;
    // 0x8002xxxx carries a FreeBSD errno in its low bits.
    uint32_t u = (uint32_t)r;
    errno = ((u & 0xFFFF0000u) == 0x80020000u) ? (int)(u & 0xFFFFu) : EIO;
    return false;
}

int to_sce(Prot p) {
    switch (p) {
    case kRead: return sce::kProtCpuRead;
    case kReadWrite: return sce::kProtCpuRead | sce::kProtCpuWrite;
    default: return 0;
    }
}

bool overlaps_gpu_window(uintptr_t a, size_t n) {
    return a < sce::kGpuWindowEnd && sce::kGpuWindowBegin < a + n;
}

Reservation* find_locked(const void* p, size_t size) {
    uintptr_t a = (uintptr_t)p;
    for (auto& r : g_res) {
        if (r.base && a >= r.base && size <= r.size && a - r.base <= r.size - size) return &r;
    }
    return nullptr;
}

// Re-reserve [va, va+len) after unmapping so the hole stays owned by us.
bool rereserve(uintptr_t va, size_t len) {
    void* at = (void*)va;
    return sce_ok(sceKernelReserveVirtualRange(&at, len, sce::kMapFixed, 0)) && at == (void*)va;
}

// Unmap and release pages [first, first+count) of r that are backed. Runs of
// contiguous virtual pages with contiguous direct offsets are released together.
bool unback_locked(Reservation& r, size_t first, size_t count) {
    bool ok = true;
    size_t i = first, end = first + count;
    while (i < end) {
        if (r.backing[i] == kNoBacking) { ++i; continue; }
        size_t j = i + 1;
        while (j < end && r.backing[j] != kNoBacking &&
               (r.backing[j] & ~kNewTag) == (r.backing[j - 1] & ~kNewTag) + (int64_t)kTrackPage)
            ++j;
        uintptr_t va = r.base + i * kTrackPage;
        size_t len = (j - i) * kTrackPage;
        if (!sce_ok(sceKernelMunmap((void*)va, len))) ok = false;
        if (!rereserve(va, len)) ok = false;
        if (!sce_ok(sceKernelReleaseDirectMemory(r.backing[i] & ~kNewTag, len))) ok = false;
        g_direct_bytes -= len;
        for (size_t k = i; k < j; ++k) r.backing[k] = kNoBacking;
        i = j;
    }
    return ok;
}

// Back pages [first, first+count) (all currently unbacked) with one
// direct-memory run, mapped RW and zero-filled.
bool back_run_locked(Reservation& r, size_t first, size_t count) {
    size_t len = count * kTrackPage;
    int64_t phys = -1;
    if (!sce_ok(sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len, kTrackPage,
                                              sce::kDirectMemoryTypeCpuRw, &phys)))
        return false;
    void* at = (void*)(r.base + first * kTrackPage);
    void* want = at;
    if (!sce_ok(sceKernelMapDirectMemory(&at, len, sce::kProtCpuRead | sce::kProtCpuWrite,
                                         sce::kMapFixed, phys, kTrackPage)) ||
        at != want) {
        int e = errno;
        if (at != want && at) sceKernelMunmap(at, len);
        rereserve((uintptr_t)want, len);
        sceKernelReleaseDirectMemory(phys, len);
        errno = (e == 0) ? EFAULT : e;
        return false;
    }
    // The contract promises zero-filled fresh pages; whether the kernel zeroes
    // recycled direct memory is not established, so do it here.
    memset(want, 0, len);
    g_direct_bytes += len;
    for (size_t k = 0; k < count; ++k) r.backing[first + k] = phys + (int64_t)(k * kTrackPage);
    return true;
}

}  // namespace

size_t vm_page_size() { return (size_t)getpagesize(); }

void* vm_reserve(size_t size, size_t align) {
    if (size == 0 || (align & (align - 1)) != 0 || align < kTrackPage ||
        size % kTrackPage != 0) {
        errno = EINVAL;
        return nullptr;
    }
    pthread_mutex_lock(&g_lock);
    Reservation* slot = nullptr;
    for (auto& r : g_res)
        if (!r.base) { slot = &r; break; }
    if (!slot) {
        pthread_mutex_unlock(&g_lock);
        errno = ENOMEM;
        return nullptr;
    }
    size_t pages = size / kTrackPage;
    size_t table_bytes = (pages * sizeof(int64_t) + 0x3FFF) & ~(size_t)0x3FFF;
    void* table = mmap(nullptr, table_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (table == MAP_FAILED) {
        pthread_mutex_unlock(&g_lock);
        errno = ENOMEM;
        return nullptr;
    }
    int last_errno = ENOMEM;
    uintptr_t got = 0;
    for (uintptr_t hint : kReserveHints) {
        if (hint % align) continue;
        void* at = (void*)hint;
        if (!sce_ok(sceKernelReserveVirtualRange(&at, size, 0, align))) {
            last_errno = errno;
            continue;
        }
        uintptr_t a = (uintptr_t)at;
        if (!at || a % align || overlaps_gpu_window(a, size)) {
            sceKernelMunmap(at, size);
            last_errno = ENOMEM;
            continue;
        }
        got = a;
        break;
    }
    if (!got) {
        munmap(table, table_bytes);
        pthread_mutex_unlock(&g_lock);
        errno = last_errno;
        return nullptr;
    }
    auto* backing = (int64_t*)table;
    for (size_t i = 0; i < pages; ++i) backing[i] = kNoBacking;
    *slot = {got, size, backing, pages, table_bytes};
    pthread_mutex_unlock(&g_lock);
    return (void*)got;
}

void vm_release(void* p, size_t size) {
    if (!p) return;
    pthread_mutex_lock(&g_lock);
    for (auto& r : g_res) {
        if (r.base != (uintptr_t)p || r.size != size) continue;
        unback_locked(r, 0, r.pages);
        sceKernelMunmap((void*)r.base, r.size);
        munmap(r.backing, r.table_bytes);
        r = {};
        break;
    }
    pthread_mutex_unlock(&g_lock);
}

bool vm_commit(void* p, size_t size, Prot prot) {
    if (prot == kNone || size == 0 || (uintptr_t)p % kTrackPage || size % kTrackPage) {
        errno = EINVAL;
        return false;
    }
    pthread_mutex_lock(&g_lock);
    Reservation* r = find_locked(p, size);
    if (!r) {
        pthread_mutex_unlock(&g_lock);
        errno = EINVAL;
        return false;
    }
    size_t first = ((uintptr_t)p - r->base) / kTrackPage;
    size_t count = size / kTrackPage;
    size_t max_run = g_max_run_bytes / kTrackPage;
    if (max_run == 0) max_run = 1;
    // Pages backed by this call carry kNewTag until the call succeeds, so a
    // failure unwinds exactly those and leaves pre-existing pages untouched.
    bool ok = true;
    size_t i = first, end = first + count;
    while (ok && i < end) {
        if (r->backing[i] != kNoBacking) { ++i; continue; }
        size_t j = i;
        while (j < end && r->backing[j] == kNoBacking && j - i < max_run) ++j;
        ok = back_run_locked(*r, i, j - i);
        if (ok)
            for (size_t k = i; k < j; ++k) r->backing[k] |= kNewTag;
        i = j;
    }
    if (ok) ok = sce_ok(sceKernelMprotect(p, size, to_sce(prot)));
    int e = errno;
    for (size_t k = first; k < end;) {
        if (r->backing[k] == kNoBacking || !(r->backing[k] & kNewTag)) { ++k; continue; }
        size_t j = k;
        while (j < end && r->backing[j] != kNoBacking && (r->backing[j] & kNewTag)) {
            r->backing[j] &= ~kNewTag;
            ++j;
        }
        if (!ok) unback_locked(*r, k, j - k);
        k = j;
    }
    pthread_mutex_unlock(&g_lock);
    if (!ok) errno = e;
    return ok;
}

bool vm_decommit(void* p, size_t size) {
    if (size == 0 || (uintptr_t)p % kTrackPage || size % kTrackPage) {
        errno = EINVAL;
        return false;
    }
    pthread_mutex_lock(&g_lock);
    Reservation* r = find_locked(p, size);
    if (!r) {
        pthread_mutex_unlock(&g_lock);
        errno = EINVAL;
        return false;
    }
    bool ok = unback_locked(*r, ((uintptr_t)p - r->base) / kTrackPage, size / kTrackPage);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool vm_protect(void* p, size_t size, Prot prot) {
    return sce_ok(sceKernelMprotect(p, size, to_sce(prot)));
}

// Second view of already committed pages: the same direct-memory offsets are
// mapped again at `dst` (no new direct memory). The alias pages are not in the
// backing table, so vm_decommit of the source never releases them by itself:
// callers vm_unalias first.
bool vm_alias(void* dst, const void* src, size_t size, Prot prot) {
    if (prot == kNone || size == 0 || (uintptr_t)dst % kTrackPage || (uintptr_t)src % kTrackPage ||
        size % kTrackPage) {
        errno = EINVAL;
        return false;
    }
    pthread_mutex_lock(&g_lock);
    Reservation* r = find_locked(src, size);
    if (!r || find_locked(dst, size) != r) {
        pthread_mutex_unlock(&g_lock);
        errno = EINVAL;
        return false;
    }
    size_t first = ((uintptr_t)src - r->base) / kTrackPage;
    size_t count = size / kTrackPage;
    for (size_t k = 0; k < count; ++k)
        if (r->backing[first + k] == kNoBacking) {
            pthread_mutex_unlock(&g_lock);
            errno = EINVAL;
            return false;
        }
    bool ok = true;
    for (size_t i = 0; ok && i < count;) {
        size_t j = i + 1;
        while (j < count && (r->backing[first + j] & ~kNewTag) == (r->backing[first + j - 1] & ~kNewTag) + (int64_t)kTrackPage)
            ++j;
        void* at = (char*)dst + i * kTrackPage;
        void* want = at;
        ok = sce_ok(sceKernelMapDirectMemory(&at, (j - i) * kTrackPage, to_sce(prot), sce::kMapFixed,
                                             r->backing[first + i] & ~kNewTag, kTrackPage)) && at == want;
        i = j;
    }
    int e = errno;
    pthread_mutex_unlock(&g_lock);
    if (!ok) errno = e ? e : EFAULT;
    return ok;
}

bool vm_unalias(void* dst, size_t size) {
    if (size == 0 || (uintptr_t)dst % kTrackPage || size % kTrackPage) {
        errno = EINVAL;
        return false;
    }
    pthread_mutex_lock(&g_lock);
    bool ok = sce_ok(sceKernelMunmap(dst, size)) && rereserve((uintptr_t)dst, size);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

namespace ps5 {
void set_max_run_bytes(size_t bytes) {
    pthread_mutex_lock(&g_lock);
    g_max_run_bytes = bytes < kTrackPage ? kTrackPage : bytes;
    pthread_mutex_unlock(&g_lock);
}
uint64_t direct_bytes_held() {
    pthread_mutex_lock(&g_lock);
    uint64_t v = g_direct_bytes;
    pthread_mutex_unlock(&g_lock);
    return v;
}
}  // namespace ps5

}  // namespace rcomp::os
