// TESTDOUBLE (owner: Agent 2): host (Linux) stand-in for the PS5 kernel memory
// calls declared in platform/ps5/ps5_kernel.h, so os_vm_ps5.cpp's bookkeeping,
// unwinding and page tracking can be exercised by test_guest_memory on the
// host. It proves nothing about the console: it models only the behaviour the
// backend relies on (listed below) and every model choice is an assumption.
//
//  * Reserve: PROT_NONE anonymous mapping. flags 0 + non-null hint -> placed
//    exactly at the hint or refused (EEXIST -> ENOMEM), like the console's
//    refusal of hints it cannot honour. kMapFixed -> MAP_FIXED.
//  * Direct memory: a sparse memfd "pool"; a first-fit 64 KiB-block allocator
//    hands out offsets; release accepts any allocated sub-range and punches a
//    hole (pages read back as zero -- the backend zero-fills anyway).
//  * MapDirectMemory: MAP_SHARED view of the pool at the offset.
//  * Failure injection: TESTDOUBLE_ps5_fail_allocate_after(n) makes the n-th
//    following sceKernelAllocateDirectMemory fail with ENOMEM.
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../ps5/ps5_kernel.h"

namespace {

constexpr uint64_t kBlock = 0x10000;
constexpr uint64_t kPoolSize = 8ull << 30;  // 8 GiB, sparse
constexpr uint64_t kBlocks = kPoolSize / kBlock;
constexpr int32_t kENOMEM = (int32_t)0x8002000C;
constexpr int32_t kEINVAL = (int32_t)0x80020016;

int g_fd = -1;
uint64_t g_used[kBlocks / 64];
uint64_t g_used_bytes = 0;
long g_fail_after = -1;

int32_t sce_err(int e) { return (int32_t)(0x80020000u | (uint32_t)(e & 0xFFFF)); }

bool pool() {
    if (g_fd >= 0) return true;
    g_fd = memfd_create("TESTDOUBLE_ps5_dmem", MFD_CLOEXEC);
    if (g_fd < 0) return false;
    if (ftruncate(g_fd, (off_t)kPoolSize) != 0) {
        close(g_fd);
        g_fd = -1;
        return false;
    }
    return true;
}
bool used(uint64_t b) { return g_used[b / 64] >> (b % 64) & 1; }
void set_used(uint64_t b, bool v) {
    if (v) g_used[b / 64] |= 1ull << (b % 64);
    else g_used[b / 64] &= ~(1ull << (b % 64));
}
int prot_native(int p) {
    int n = PROT_NONE;
    if (p & rcomp::ps5::kProtCpuRead) n |= PROT_READ;
    if (p & rcomp::ps5::kProtCpuWrite) n |= PROT_WRITE;
    return n;
}

}  // namespace

extern "C" {

void TESTDOUBLE_ps5_fail_allocate_after(long n) { g_fail_after = n; }
uint64_t TESTDOUBLE_ps5_direct_used_bytes() { return g_used_bytes; }

int32_t sceKernelReserveVirtualRange(void** addr, size_t len, int flags, size_t alignment) {
    if (!addr || len == 0) return kEINVAL;
    int f = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    if (flags & rcomp::ps5::kMapFixed) {
        void* p = mmap(*addr, len, PROT_NONE, f | MAP_FIXED, -1, 0);
        return p == MAP_FAILED ? sce_err(errno) : 0;
    }
    if (*addr) {
        if (alignment && (uintptr_t)*addr % alignment) return kEINVAL;
        void* p = mmap(*addr, len, PROT_NONE, f | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) return kENOMEM;
        if (p != *addr) {  // old kernels ignore NOREPLACE
            munmap(p, len);
            return kENOMEM;
        }
        return 0;
    }
    size_t align = alignment ? alignment : 0x4000;
    void* raw = mmap(nullptr, len + align, PROT_NONE, f, -1, 0);
    if (raw == MAP_FAILED) return sce_err(errno);
    uintptr_t s = (uintptr_t)raw, a = (s + align - 1) & ~(uintptr_t)(align - 1);
    if (a > s) munmap(raw, a - s);
    if (s + len + align > a + len) munmap((void*)(a + len), s + len + align - (a + len));
    *addr = (void*)a;
    return 0;
}

int64_t sceKernelGetDirectMemorySize(void) { return (int64_t)kPoolSize; }

int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t len,
                                      size_t alignment, int memory_type, int64_t* phys_out) {
    (void)memory_type;
    if (!pool()) return kENOMEM;
    if (g_fail_after >= 0 && g_fail_after-- == 0) return kENOMEM;
    if (len == 0 || len % kBlock || !phys_out) return kEINVAL;
    uint64_t step = alignment > kBlock ? alignment / kBlock : 1;
    uint64_t need = len / kBlock;
    uint64_t lo = (uint64_t)search_start / kBlock, hi = (uint64_t)search_end / kBlock;
    if (hi > kBlocks) hi = kBlocks;
    lo = (lo + step - 1) / step * step;
    for (uint64_t b = lo; b + need <= hi; b += step) {
        uint64_t k = 0;
        while (k < need && !used(b + k)) ++k;
        if (k == need) {
            for (k = 0; k < need; ++k) set_used(b + k, true);
            g_used_bytes += len;
            *phys_out = (int64_t)(b * kBlock);
            return 0;
        }
    }
    return kENOMEM;
}

int32_t sceKernelReleaseDirectMemory(int64_t start, size_t len) {
    if (start < 0 || (uint64_t)start % kBlock || len == 0 || len % kBlock ||
        (uint64_t)start + len > kPoolSize)
        return kEINVAL;
    for (uint64_t b = (uint64_t)start / kBlock; b < ((uint64_t)start + len) / kBlock; ++b)
        if (!used(b)) return kEINVAL;
    for (uint64_t b = (uint64_t)start / kBlock; b < ((uint64_t)start + len) / kBlock; ++b)
        set_used(b, false);
    g_used_bytes -= len;
    fallocate(g_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, start, (off_t)len);
    return 0;
}

int32_t sceKernelMapDirectMemory(void** addr, size_t len, int prot, int flags,
                                 int64_t direct_start, size_t alignment) {
    (void)alignment;
    if (!addr || !pool()) return kEINVAL;
    for (uint64_t b = (uint64_t)direct_start / kBlock;
         b < ((uint64_t)direct_start + len + kBlock - 1) / kBlock; ++b)
        if (!used(b)) return kEINVAL;
    int f = MAP_SHARED | ((flags & rcomp::ps5::kMapFixed) ? MAP_FIXED : 0);
    void* p = mmap(*addr, len, prot_native(prot), f, g_fd, (off_t)direct_start);
    if (p == MAP_FAILED) return sce_err(errno);
    *addr = p;
    return 0;
}

int32_t sceKernelMunmap(void* addr, size_t len) {
    return munmap(addr, len) == 0 ? 0 : sce_err(errno);
}

int32_t sceKernelMprotect(const void* addr, size_t len, int prot) {
    return mprotect((void*)addr, len, prot_native(prot)) == 0 ? 0 : sce_err(errno);
}

int32_t sceKernelAvailableFlexibleMemorySize(size_t* size) {
    *size = 448ull << 20;
    return 0;
}
int32_t sceKernelConfiguredFlexibleMemorySize(size_t* size) {
    *size = 448ull << 20;
    return 0;
}
int32_t sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t* phys_out,
                                           size_t* size_out) {
    if (phys_out) *phys_out = 0;
    if (size_out) *size_out = (size_t)(kPoolSize - g_used_bytes);
    return 0;
}

}  // extern "C"
