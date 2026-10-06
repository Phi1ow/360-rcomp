// Platform VM primitives (owner: Agent 2). One implementation per target:
// platform/posix/os_vm_posix.cpp (Linux host) and platform/ps5/os_vm_ps5.cpp
// (PS5: anonymous PROT_NONE mmap is charged to the ~448 MiB flexible budget
// there, so reservations use sceKernelReserveVirtualRange and commits use
// direct memory; see platform/ps5/README.md). The PS5 backend requires
// vm_commit/vm_decommit ranges aligned to 64 KiB.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rcomp::os {

enum Prot : uint32_t { kNone = 0, kRead = 1, kReadWrite = 3 };

// Reserve `size` bytes of address space, no access, aligned to `align`
// (power of two, >= page size). Returns nullptr on failure; errno preserved.
void* vm_reserve(size_t size, size_t align);
void vm_release(void* p, size_t size);
// Make [p, p+size) accessible with `prot`. Must be inside a reservation.
bool vm_commit(void* p, size_t size, Prot prot);
// Drop backing store and make inaccessible again; stays reserved.
bool vm_decommit(void* p, size_t size);
bool vm_protect(void* p, size_t size, Prot prot);
size_t vm_page_size();

// Maps the backing of the committed range [src, src+size) a second time at
// [dst, dst+size), inside the same reservation, so both views show the same
// bytes (the Xbox 360 physical-memory windows). Both ranges are 64 KiB
// aligned and disjoint. Returns false with errno == ENOSYS where the backend
// cannot alias; any other errno is a real failure.
bool vm_alias(void* dst, const void* src, size_t size, Prot prot);
// Removes a view created by vm_alias; the address space stays reserved and
// the shared backing is untouched.
bool vm_unalias(void* dst, size_t size);

}  // namespace rcomp::os
