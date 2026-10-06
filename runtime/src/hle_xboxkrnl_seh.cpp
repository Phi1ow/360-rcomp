// xboxkrnl structured exception handling exports: RtlCaptureContext,
// RtlUnwind, __C_specific_handler (owner: Agent 3, runtime/).
// Contract, model limits and decisions: runtime/docs/SEH.md.
//
// R-comp executes AOT C++ translations of the title's functions. Structured
// exception *dispatch* needs two things this model does not have: an exception
// source the runtime can resume from (guest faults and RtlRaiseException stop
// the title, src/hle_xboxkrnl_sched.cpp), and a way to resume execution at an
// arbitrary guest address in an arbitrary guest frame (an __except block or a
// longjmp target is the middle of a recompiled C++ function, with live host
// frames above it). What is implementable exactly is implemented:
//  * RtlCaptureContext: a complete Xbox 360 CONTEXT of the caller.
//  * RtlUnwind: the unwind that crosses no frame and runs no handler (the
//    target is the caller's own frame at the call's return address, and the
//    caller's .pdata entry has no exception handler). Everything else is an
//    explicit RCOMP_FATAL_UNIMPLEMENTED naming what would be needed.
//  * __C_specific_handler: a language handler is called only by an exception
//    dispatcher or unwinder; R-comp has neither, so a call always traps.
//
// CONTEXT layout (big-endian, 0xA40 bytes), corroborated by two independent
// public descriptions of the XDK structure: RBEnhanced/RBException
// ExceptionTypesRB3E.cs (XeContext) and Team-Resurgent/RXDK360 XContext.cs
// (with the ContextFlags values):
//   +0x000 ContextFlags  +0x004 Msr  +0x008 Iar  +0x00C Lr  +0x010 u64 Ctr
//   +0x018 u64 Gpr[32]   +0x118 Cr   +0x11C Xer  +0x120 double Fpscr
//   +0x128 double Fpr[32] +0x228 UserModeControl +0x22C Fill
//   +0x230 u32 Vscr[4]   +0x240 Vr[128] (16 bytes each)
// CONTEXT_CONTROL 0x1, CONTEXT_FLOATING_POINT 0x2, CONTEXT_INTEGER 0x4,
// CONTEXT_VECTOR 0x10.
#include <stdio.h>
#include <string.h>

#include <mutex>

#include "module_state.h"
#include "rcomp/diag.h"
#include "rcomp/guest_memory.h"
#include "physical_window.h"
#include "rcomp/runtime/guest_context.h"
#include "rcomp/runtime/import_registry.h"
#include "rcomp/runtime/runtime.h"

namespace rcomp::rt {

Status register_xboxkrnl_seh_hle();

namespace {

constexpr uint32_t kContextBytes = 0xA40;
constexpr uint32_t kContextControl = 0x1, kContextFloatingPoint = 0x2, kContextInteger = 0x4,
                   kContextVector = 0x10;
constexpr uint32_t kVscrNonJava = 0x00010000u;  // VSCR[NJ]: the generated VMX code flushes denormals

void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
void put64(uint8_t* p, uint64_t v) {
    put32(p, uint32_t(v >> 32));
    put32(p + 4, uint32_t(v));
}
uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint32_t le32(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

Runtime& rt_or_die(const char* fn) {
    Runtime* r = runtime();
    if (!r) rcomp_fatal(RCOMP_FATAL_INTERNAL, "%s before runtime_init", fn);
    return *r;
}

uint32_t pack_cr(const PPCContext& c) {
    const PPCCRRegister* fields[8] = {&c.cr0, &c.cr1, &c.cr2, &c.cr3, &c.cr4, &c.cr5, &c.cr6, &c.cr7};
    uint32_t cr = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const PPCCRRegister& f = *fields[i];
        const uint32_t nibble = (f.lt ? 8u : 0u) | (f.gt ? 4u : 0u) | (f.eq ? 2u : 0u) | (f.so ? 1u : 0u);
        cr |= nibble << (28 - 4 * i);
    }
    return cr;
}

// RtlCaptureContext (0x0119): VOID (PCONTEXT). Stores the caller's register
// state as it is at the call: Iar and Lr = the return address, Gpr1 = the
// caller's stack pointer, Gpr3 = the CONTEXT pointer itself, all other
// registers from the live guest context (the runtime's PPCContext layout is the
// generated code's, guest_context_check.cpp). Fpscr holds what the generated
// code's mffs reads (the rounding mode); Vscr holds NJ (the generated VMX code
// flushes denormals; the saturation bit is not tracked). UserModeControl and
// Fill are not modelled and are stored as 0. ContextFlags = CONTROL | FLOATING_POINT
// | INTEGER | VECTOR, since every group is filled.
void RtlCaptureContext(PPCContext& ctx, uint8_t*) {
    const char* fn = "RtlCaptureContext";
    Runtime& r = rt_or_die(fn);
    const uint32_t record = ctx.r3.u32;
    if (!record || (record & 3) || !r.mem->is_accessible(record, kContextBytes, Protect::ReadWrite))
        rcomp_fatal(RCOMP_FATAL_GUEST_ACCESS, "xboxkrnl.exe!%s CONTEXT 0x%08X unwritable or misaligned lr=0x%08X", fn,
                    record, (uint32_t)ctx.lr);
    uint8_t* p = r.mem->host(record);
    put32(p + 0x000, kContextControl | kContextFloatingPoint | kContextInteger | kContextVector);
    put32(p + 0x004, ctx.msr);
    put32(p + 0x008, uint32_t(ctx.lr));
    put32(p + 0x00C, uint32_t(ctx.lr));
    put64(p + 0x010, ctx.ctr.u64);
    const PPCRegister* gpr[32] = {&ctx.r0,  &ctx.r1,  &ctx.r2,  &ctx.r3,  &ctx.r4,  &ctx.r5,  &ctx.r6,  &ctx.r7,
                                  &ctx.r8,  &ctx.r9,  &ctx.r10, &ctx.r11, &ctx.r12, &ctx.r13, &ctx.r14, &ctx.r15,
                                  &ctx.r16, &ctx.r17, &ctx.r18, &ctx.r19, &ctx.r20, &ctx.r21, &ctx.r22, &ctx.r23,
                                  &ctx.r24, &ctx.r25, &ctx.r26, &ctx.r27, &ctx.r28, &ctx.r29, &ctx.r30, &ctx.r31};
    for (uint32_t i = 0; i < 32; ++i) put64(p + 0x018 + 8 * i, gpr[i]->u64);
    put32(p + 0x118, pack_cr(ctx));
    put32(p + 0x11C, (ctx.xer.so ? 0x80000000u : 0u) | (ctx.xer.ov ? 0x40000000u : 0u) |
                         (ctx.xer.ca ? 0x20000000u : 0u));
    put64(p + 0x120, uint64_t(ctx.fpscr.loadFromHost()));
    const PPCRegister* fpr[32] = {&ctx.f0,  &ctx.f1,  &ctx.f2,  &ctx.f3,  &ctx.f4,  &ctx.f5,  &ctx.f6,  &ctx.f7,
                                  &ctx.f8,  &ctx.f9,  &ctx.f10, &ctx.f11, &ctx.f12, &ctx.f13, &ctx.f14, &ctx.f15,
                                  &ctx.f16, &ctx.f17, &ctx.f18, &ctx.f19, &ctx.f20, &ctx.f21, &ctx.f22, &ctx.f23,
                                  &ctx.f24, &ctx.f25, &ctx.f26, &ctx.f27, &ctx.f28, &ctx.f29, &ctx.f30, &ctx.f31};
    for (uint32_t i = 0; i < 32; ++i) put64(p + 0x128 + 8 * i, fpr[i]->u64);
    put32(p + 0x228, 0);
    put32(p + 0x22C, 0);
    put32(p + 0x230, 0);
    put32(p + 0x234, 0);
    put32(p + 0x238, 0);
    put32(p + 0x23C, kVscrNonJava);
    // The 128 VMX128 registers are contiguous in PPCContext (v0..v127); each is
    // held byte-reversed, as the generated stvx stores it (VectorMaskL).
    const PPCVRegister* vr = &ctx.v0;
    static_assert(offsetof(PPCContext, v127) - offsetof(PPCContext, v0) == 127 * sizeof(PPCVRegister),
                  "PPCContext vector registers are not contiguous");
    for (uint32_t i = 0; i < 128; ++i)
        for (uint32_t b = 0; b < 16; ++b) p[0x240 + 16 * i + b] = vr[i].u8[15 - b];
    note_title_write(record, kContextBytes);
}

// The .pdata entry (IMAGE_CE_RUNTIME_FUNCTION, big-endian {BeginAddress, Data:
// PrologLength 8 | FunctionLength 22 | ThirtyTwoBit 1 | ExceptionFlag 1}) of
// the main image's function containing `pc`, read from the loaded PE's
// ".pdata" section, the table XenonRecomp itself uses.
enum class PdataLookup { Found, NoImage, NoTable, NotCovered };
PdataLookup find_function_entry(Runtime& r, uint32_t pc, uint32_t* begin, uint32_t* data) {
    const ModuleState* m = r.modules.get();
    if (!m || !m->ready || m->generation != r.generation || !m->image_base) return PdataLookup::NoImage;
    const uint32_t base = m->image_base;
    if (!r.mem->is_accessible(base, 0x40, Protect::Read)) return PdataLookup::NoImage;
    const uint8_t* image = r.mem->host(base);
    const uint32_t nt = le32(image + 0x3C);
    if (le16(image) != 0x5A4D || uint64_t(nt) + 24 > m->image_size ||
        !r.mem->is_accessible(base + nt, 24, Protect::Read) || le32(image + nt) != 0x4550)
        return PdataLookup::NoImage;
    const uint32_t sections = le16(image + nt + 6), optional = le16(image + nt + 20);
    const uint64_t table = uint64_t(nt) + 24 + optional;
    if (table + uint64_t(sections) * 40 > m->image_size || !r.mem->is_accessible(base + uint32_t(table), sections * 40, Protect::Read))
        return PdataLookup::NoImage;
    for (uint32_t i = 0; i < sections; ++i) {
        const uint8_t* s = image + table + 40 * i;
        if (memcmp(s, ".pdata\0\0", 8) != 0) continue;
        const uint32_t size = le32(s + 8), rva = le32(s + 12);
        if (uint64_t(rva) + size > m->image_size || !r.mem->is_accessible(base + rva, size, Protect::Read))
            return PdataLookup::NoTable;
        const uint8_t* entries = image + rva;
        uint32_t lo = 0, hi = size / 8;
        while (lo < hi) {  // sorted by BeginAddress, as the linker emits it
            const uint32_t mid = (lo + hi) / 2;
            const uint32_t b = be32(entries + 8 * mid), d = be32(entries + 8 * mid + 4);
            const uint64_t end = uint64_t(b) + 4ull * ((d >> 8) & 0x3FFFFFu);
            if (pc < b) hi = mid;
            else if (pc >= end) lo = mid + 1;
            else { *begin = b; *data = d; return PdataLookup::Found; }
        }
        return PdataLookup::NotCovered;
    }
    return PdataLookup::NoTable;
}

// RtlUnwind (0x0147): VOID (PVOID TargetFrame, PVOID TargetIp,
// PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue). NT semantics: call
// the language handler of every frame from the caller up to TargetFrame with
// EXCEPTION_UNWINDING (EXCEPTION_TARGET_UNWIND for the target), then continue
// at TargetIp in TargetFrame with r3 = ReturnValue.
// Implemented: TargetFrame equal to the caller's stack pointer and TargetIp
// equal to the call's return address, with no exception handler in the
// caller's .pdata entry: no frame is crossed and no handler runs, so the
// unwind is the return itself with r3 = ReturnValue. Every other request
// needs a frame unwinder and a resume at an arbitrary guest address, which AOT
// code cannot do: RCOMP_FATAL_UNIMPLEMENTED with the reason.
void RtlUnwind(PPCContext& ctx, uint8_t*) {
    const char* fn = "RtlUnwind";
    Runtime& r = rt_or_die(fn);
    const uint32_t target_frame = ctx.r3.u32, target_ip = ctx.r4.u32, record = ctx.r5.u32, value = ctx.r6.u32;
    const uint32_t caller_sp = ctx.r1.u32, return_address = uint32_t(ctx.lr);
    if (!target_frame)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!RtlUnwind exit unwind (TargetFrame NULL, TargetIp=0x%08X record=0x%08X) lr=0x%08X: "
                    "unwinding every guest frame and ending the thread needs a frame unwinder R-comp does not have",
                    target_ip, record, return_address);
    if (target_frame != caller_sp || target_ip != return_address)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!RtlUnwind TargetFrame=0x%08X TargetIp=0x%08X record=0x%08X from sp=0x%08X lr=0x%08X: "
                    "a non-local unwind must run the termination handlers of the frames in between and resume in the "
                    "middle of a recompiled function, which AOT code cannot do (if this is the C runtime's longjmp, "
                    "give XenonRecomp its setjmp_address/longjmp_address so the host setjmp/longjmp replaces it)",
                    target_frame, target_ip, record, caller_sp, return_address);
    uint32_t begin = 0, data = 0;
    const PdataLookup found = find_function_entry(r, return_address - 4, &begin, &data);
    if (found != PdataLookup::Found)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!RtlUnwind lr=0x%08X: no .pdata entry of the main image covers the caller (%s), so "
                    "whether a language handler must run in the target frame is unknown",
                    return_address,
                    found == PdataLookup::NoImage ? "no finalized main image" : found == PdataLookup::NoTable ? "no .pdata section" : "address not covered");
    if (data & 0x80000000u)
        rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                    "xboxkrnl.exe!RtlUnwind lr=0x%08X: the target frame's function 0x%08X has an exception handler; a "
                    "target unwind would have to run its termination handlers, and R-comp has no exception dispatcher",
                    return_address, begin);
    ctx.r3.u64 = value;
}

// __C_specific_handler (0x01A5): EXCEPTION_DISPOSITION (PEXCEPTION_RECORD,
// PVOID EstablisherFrame, PCONTEXT, PDISPATCHER_CONTEXT). The C language
// handler named by .pdata handler slots: only an exception dispatcher or an
// unwinder calls it (filters, __finally funclets, transfer to an __except
// block). R-comp has neither (see RtlUnwind), so reaching it is reported with
// the record instead of guessing a disposition.
void C_specific_handler(PPCContext& ctx, uint8_t*) {
    Runtime& r = rt_or_die("__C_specific_handler");
    const uint32_t record = ctx.r3.u32;
    uint32_t code = 0, flags = 0;
    if (record && r.mem->is_accessible(record, 8, Protect::Read)) {
        guest_read_be32(record, &code);
        guest_read_be32(record + 4, &flags);
    }
    rcomp_fatal(RCOMP_FATAL_UNIMPLEMENTED,
                "xboxkrnl.exe!__C_specific_handler record=0x%08X code=0x%08X flags=0x%08X frame=0x%08X context=0x%08X "
                "dispatcher=0x%08X lr=0x%08X: language handlers run only under exception dispatch/unwinding, which "
                "R-comp's AOT model does not provide (runtime/docs/SEH.md)",
                record, code, flags, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
}

struct Impl { uint32_t ordinal; const char* name; PPCFunc* function; };
constexpr Impl kImpls[] = {
    {0x0119, "RtlCaptureContext", &RtlCaptureContext},
    {0x0147, "RtlUnwind", &RtlUnwind},
    {0x01A5, "__C_specific_handler", &C_specific_handler},
};

}  // namespace

Status register_xboxkrnl_seh_hle() {
    for (const auto& impl : kImpls) {
        uint32_t ordinal = 0;
        if (!export_ordinal(kModuleXboxkrnl, impl.name, &ordinal) || ordinal != impl.ordinal)
            return Status::InvalidArgument;
        const Status status = register_import(kModuleXboxkrnl, impl.ordinal, impl.function, impl.name);
        if (status != Status::Ok) return status;
    }
    return Status::Ok;
}

}  // namespace rcomp::rt
