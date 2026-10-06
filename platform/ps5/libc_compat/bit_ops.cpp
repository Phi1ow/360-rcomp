// Bit-scan helper absent from the PS5 title libc (owner: platform/).
// rexglue's math_gcc.cpp calls ffs(); the definition is weak so a libc that
// exports it wins.
extern "C" __attribute__((weak)) int ffs(int value) { return __builtin_ffs(value); }
