// CPU test harness types (Agent 6). Included by generated case tables.
#pragma once

#define PPC_CONFIG_H_INCLUDED
#include <ppc_context.h>

#include <stdint.h>

#include "rcomp/func_table.h"

enum RegKind { RK_GPR, RK_FVAL, RK_FBITS, RK_VR, RK_CR, RK_XER_CA, RK_XER_OV, RK_XER_SO, RK_CTR };

struct RegVal {
    const char* reg;
    RegKind kind;
    uint64_t u;      // GPR/CTR value, FPR bits, CR (32-bit), XER bit
    uint32_t w[4];   // VR words in guest (big-endian element) order
};

struct MemVal {
    uint32_t addr;
    const uint8_t* bytes;
    uint32_t size;
};

struct TestCase {
    const char* id;
    PPCFunc* fn;
    uint32_t guest;
    const RegVal* in; int n_in;
    const RegVal* out; int n_out;
    const MemVal* mem_in; int n_mem_in;
    const MemVal* mem_out; int n_mem_out;
    const char* expect_fatal;  // rcomp_fatal kind name, or nullptr
    const char* program;       // "#_ PROGRAM" arguments: needs the guest runtime (tests/m3)
    const char* gfx_expect;    // "#_ GFX_EXPECT" image oracle (tests/m5)
};

struct TestUnit {
    const char* name;
    const TestCase* cases; int n_cases;
    const rcomp::FuncEntry* funcs; int n_funcs;
};

extern const TestUnit* const kUnits[];
extern const int kUnitCount;
