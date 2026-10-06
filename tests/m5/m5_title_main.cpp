// PS5 program entry for the M5 slice; main() is provided by
// gpu/vulkan/common/rcomp_title_wrap.c (logs + RCOMP-TITLE framing).
#include <stdio.h>

extern "C" int rcomp_m5_selftest(FILE* out);

extern "C" int rcomp_program_main(int, char**) { return rcomp_m5_selftest(stdout); }
