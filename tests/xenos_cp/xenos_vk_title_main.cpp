// PS5 program entry for the Xenos GPU on PS5_Vulkan (tests/xenos_cp/test_vulkan.cpp):
// main() is gpu/vulkan/common/rcomp_title_wrap.c (log + RCOMP-TITLE framing,
// experimental driver profile: the backend needs independentBlend and
// fragmentStoresAndAtomics, which the default profile does not expose).
#include <stdio.h>

extern "C" int rcomp_xenos_vulkan_selftest(FILE* out);

extern "C" int rcomp_program_main(int, char**) { return rcomp_xenos_vulkan_selftest(stdout); }
