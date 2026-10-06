// Test-only failure injection around the real host VM backend. No fake success.
// Rename only the three mutation entry points; every non-failing operation
// still uses the production mmap/VirtualAlloc implementation.
#define vm_commit TESTDOUBLE_native_vm_commit
#define vm_protect TESTDOUBLE_native_vm_protect
#define vm_decommit TESTDOUBLE_native_vm_decommit
#include "../posix/os_vm_posix.cpp"
#undef vm_commit
#undef vm_protect
#undef vm_decommit

namespace {
int fail_next = -1;
bool fail(int operation) {
    if (fail_next != operation) return false;
    fail_next = -1;
    errno = operation == 0 ? ENOMEM : EACCES;
    return true;
}
}

void TESTDOUBLE_fail_vm_once(int operation) { fail_next = operation; }

namespace rcomp::os {
bool vm_commit(void* p, size_t size, Prot prot) {
    return !fail(0) && TESTDOUBLE_native_vm_commit(p, size, prot);
}
bool vm_protect(void* p, size_t size, Prot prot) {
    return !fail(1) && TESTDOUBLE_native_vm_protect(p, size, prot);
}
bool vm_decommit(void* p, size_t size) {
    return !fail(2) && TESTDOUBLE_native_vm_decommit(p, size);
}
}
