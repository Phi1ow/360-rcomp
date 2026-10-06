#include <cstdio>
#include <cstdlib>
#include "rcomp/xenos_gpu.h"
namespace rcomp::xenos {
namespace {
[[noreturn]] void unexpected(const char* name) {
    std::fprintf(stderr, "FAIL TESTDOUBLE_cpu_only_gpu unexpected call=%s (no rendering exercised)\n", name);
    std::abort();
}
}
bool gpu_running() { return false; }
DisplayMode gpu_display_mode() { unexpected("gpu_display_mode"); }
void gpu_initialize_ring_buffer(uint32_t, uint32_t) { unexpected("gpu_initialize_ring_buffer"); }
void gpu_enable_read_pointer_writeback(uint32_t, uint32_t) { unexpected("gpu_enable_read_pointer_writeback"); }
void gpu_set_interrupt_callback(uint32_t callback, uint32_t user_data) {
    if (callback || user_data) unexpected("gpu_set_interrupt_callback");
}
}
