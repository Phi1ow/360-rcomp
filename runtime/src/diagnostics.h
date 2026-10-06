// Optional bring-up tracing. Keep counters, clocks, stack walks and emulated
// TLS out of normal runtime builds, including builds outside CMake.
#pragma once

#ifndef RCOMP_RUNTIME_DIAGNOSTICS
#define RCOMP_RUNTIME_DIAGNOSTICS 0
#endif
