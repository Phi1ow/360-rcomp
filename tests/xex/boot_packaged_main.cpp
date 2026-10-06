#include <cstdio>
extern "C" int rcomp_xex_selftest(FILE* out, const char* xex_path);
int main(int argc, char** argv) {
    return rcomp_xex_selftest(stdout, argc > 1 ? argv[1] : RCOMP_XEX_PATH);
}
