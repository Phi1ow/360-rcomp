// PS5 program entry for the synthetic graphics XEX through the game shell;
// main() is gpu/vulkan/common/rcomp_title_wrap.c (log, RCOMP-TITLE framing,
// experimental driver profile). The XEX is packaged in the title folder.
#include <stdio.h>

extern "C" int rcomp_xex_gfx_selftest(FILE* out, const char* xex_path);

extern "C" int rcomp_program_main(int, char**) {
    return rcomp_xex_gfx_selftest(stdout, "/app0/fixtures/xex/rcomp_gfx_title.xex");
}
