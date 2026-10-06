#include "os_vm.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__CYGWIN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rcomp::os {

namespace {
#if !defined(__CYGWIN__)
int to_native(Prot p) {
    switch (p) {
    case kRead: return PROT_READ;
    case kReadWrite: return PROT_READ | PROT_WRITE;
    default: return PROT_NONE;
    }
}
#endif
}  // namespace

size_t vm_page_size() { return (size_t)sysconf(_SC_PAGESIZE); }

void* vm_reserve(size_t size, size_t align) {
#if defined(__CYGWIN__)
    // Cygwin cannot replace a subrange of an anonymous mapping with MAP_FIXED.
    // Use the native reservation/commit contract; no memory is executable.
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (!align || (align & (align - 1)) || align > info.dwAllocationGranularity) return nullptr;
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
#else
    // Over-reserve then trim so the base is aligned without MAP_FIXED guessing.
    int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    size_t total = size + align;
    void* raw = mmap(nullptr, total, PROT_NONE, flags, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    uintptr_t start = (uintptr_t)raw;
    uintptr_t aligned = (start + align - 1) & ~(uintptr_t)(align - 1);
    size_t head = aligned - start;
    size_t tail = total - head - size;
    if (head) munmap(raw, head);
    if (tail) munmap((void*)(aligned + size), tail);
    return (void*)aligned;
#endif
}

void vm_release(void* p, size_t size) {
#if defined(__CYGWIN__)
    (void)size;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
#else
    if (p) munmap(p, size);
#endif
}

bool vm_commit(void* p, size_t size, Prot prot) {
#if defined(__CYGWIN__)
    DWORD protection = prot == kReadWrite ? PAGE_READWRITE : prot == kRead ? PAGE_READONLY : PAGE_NOACCESS;
    return VirtualAlloc(p, size, MEM_COMMIT, protection) == p;
#else
    return mprotect(p, size, to_native(prot)) == 0;
#endif
}

bool vm_decommit(void* p, size_t size) {
#if defined(__CYGWIN__)
    return VirtualFree(p, size, MEM_DECOMMIT) != 0;
#else
    // Replace with a fresh PROT_NONE anonymous mapping: drops the pages on both
    // Linux and FreeBSD without relying on madvise semantics.
    int flags = MAP_PRIVATE | MAP_ANON | MAP_FIXED;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    return mmap(p, size, PROT_NONE, flags, -1, 0) == p;
#endif
}

bool vm_alias(void*, const void*, size_t, Prot) {
    errno = ENOSYS;  // anonymous private mappings cannot be mapped twice
    return false;
}

bool vm_unalias(void*, size_t) {
    errno = ENOSYS;
    return false;
}

bool vm_protect(void* p, size_t size, Prot prot) {
#if defined(__CYGWIN__)
    DWORD old_protection;
    DWORD protection = prot == kReadWrite ? PAGE_READWRITE : prot == kRead ? PAGE_READONLY : PAGE_NOACCESS;
    return VirtualProtect(p, size, protection, &old_protection) != 0;
#else
    return mprotect(p, size, to_native(prot)) == 0;
#endif
}

}  // namespace rcomp::os
