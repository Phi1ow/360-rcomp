// External GPU link boundary only; no rendering is asserted.
#include "rcomp/xenos_gpu.h"
#include "rcomp/diag.h"
namespace rcomp::xenos {
bool gpu_running() { return false; }
DisplayMode gpu_display_mode() { rcomp_fatal(RCOMP_FATAL_INTERNAL,"TESTDOUBLE unused GPU display call"); }
void gpu_initialize_ring_buffer(uint32_t,uint32_t) { rcomp_fatal(RCOMP_FATAL_INTERNAL,"TESTDOUBLE unused GPU ring call"); }
void gpu_enable_read_pointer_writeback(uint32_t,uint32_t) { rcomp_fatal(RCOMP_FATAL_INTERNAL,"TESTDOUBLE unused GPU writeback call"); }
void gpu_set_interrupt_callback(uint32_t,uint32_t) { rcomp_fatal(RCOMP_FATAL_INTERNAL,"TESTDOUBLE unused GPU callback installation"); }
}
