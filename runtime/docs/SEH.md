# Structured exception handling in the AOT model — 2026-10-03

Agent 3 workstream. Status: **PASS (HOST ONLY)** for the implemented subset
(`rt_kernel_seh`). PS5 execution: **NOT TESTED**. Source:
`runtime/src/hle_xboxkrnl_seh.cpp`.

## Why full SEH dispatch is not possible here

R-comp runs the title as ahead-of-time C++ translations of its PowerPC
functions. Xbox 360 structured exception handling (SEH, the `__try` /
`__except` / `__finally` and C++ `try` machinery) needs:

1. an exception source the system can resume from: in R-comp a guest fault is
   a `guest_access` diagnostic and `RtlRaiseException` stops the title for every
   code except the thread-name convention and the CRT floating-point codes
   (`src/hle_xboxkrnl_sched.cpp`);
2. a frame unwinder that walks guest frames with `.pdata` and virtual
   unwinding of each prologue;
3. a transfer of control to an arbitrary guest address in an arbitrary guest
   frame: the `__except` block of the establisher function, or the target of a
   non-local unwind. In AOT code that address is the middle of a recompiled C++
   function with live host frames above it; nothing can jump there.

Point 3 cannot be provided by the runtime alone (it would need the generator to
split functions at every handler target and a host unwinding mechanism across
generated frames). R-comp therefore implements exactly what is honest and stops
with a precise `RCOMP_FATAL_UNIMPLEMENTED` everywhere else.

## Exports

| Export | Ordinal | Status | Behaviour |
| --- | --- | --- | --- |
| `RtlCaptureContext` | 0x0119 | PASS (host) | Full CONTEXT of the caller |
| `RtlUnwind` | 0x0147 | PASS (host) for the subset, explicit fatal otherwise | No-frame unwind only |
| `__C_specific_handler` | 0x01A5 | explicit fatal | Reachable only from a dispatcher |

### RtlCaptureContext

`VOID RtlCaptureContext(PCONTEXT)`. The Xbox 360 CONTEXT is big-endian, 0xA40
bytes. Layout corroborated by two independent public descriptions of the XDK
structure (RBEnhanced/RBException `ExceptionTypesRB3E.cs`, Team-Resurgent/RXDK360
`XContext.cs`, which also gives the flag values):

| Offset | Field | Value stored |
| --- | --- | --- |
| 0x000 | ContextFlags | `0x17` = CONTROL (1) \| FLOATING_POINT (2) \| INTEGER (4) \| VECTOR (0x10) |
| 0x004 | Msr | `ctx.msr` |
| 0x008 | Iar | the return address (`lr`), as on NT/PPC |
| 0x00C | Lr | `lr` |
| 0x010 | Ctr (u64) | `ctx.ctr` |
| 0x018 | Gpr[32] (u64) | live GPRs; Gpr1 = caller's stack pointer, Gpr3 = the CONTEXT pointer |
| 0x118 | Cr | cr0..cr7 packed (LT, GT, EQ, SO per field, cr0 in the top nibble) |
| 0x11C | Xer | SO (bit 31), OV (30), CA (29) |
| 0x120 | Fpscr (double slot) | what the generated `mffs` reads (`fpscr.loadFromHost()`, the rounding mode) |
| 0x128 | Fpr[32] | live FPRs |
| 0x228 | UserModeControl | 0 (not modelled) |
| 0x22C | Fill | 0 |
| 0x230 | Vscr[4] | `{0, 0, 0, 0x00010000}`: NJ, the mode the generated VMX code runs in; SAT is not tracked |
| 0x240 | Vr[128] | VMX128 registers, byte order as the generated `stvx` stores them |

The runtime's `PPCContext` layout is the generated code's (enforced by
`src/guest_context_check.cpp` and the shared generator configuration), so CR,
CTR and XER are the live values. A title built with the rejected `*_as_local`
generator options would violate that layout contract for every runtime service,
not only this one.

A CONTEXT that is not writable or not 4-byte aligned is a `guest_access`
diagnostic.

### RtlUnwind

`VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD,
PVOID ReturnValue)`. NT semantics: run the language handler of every frame from
the caller up to `TargetFrame` with `EXCEPTION_UNWINDING`
(`EXCEPTION_TARGET_UNWIND` for the target), then continue at `TargetIp` in
`TargetFrame` with r3 = `ReturnValue`.

Implemented: the unwind that crosses no frame and runs no handler.
`TargetFrame` equals the caller's stack pointer (r1 at the call),
`TargetIp` equals the call's return address, and the `.pdata` entry of the
caller's function (read from the loaded image's `.pdata` section, the table
XenonRecomp itself uses: big-endian `{BeginAddress, PrologLength:8 |
FunctionLength:22 | ThirtyTwoBit:1 | ExceptionFlag:1}`) has no exception
handler. The unwind is then the return itself with r3 = `ReturnValue`.

Explicit `RCOMP_FATAL_UNIMPLEMENTED`, each with its own message:

- exit unwind (`TargetFrame` NULL);
- any other target frame or resume address (a non-local unwind). The message
  points at the C runtime's `longjmp`: XenonRecomp can replace the title's
  `setjmp`/`longjmp` with host ones when `setjmp_address` / `longjmp_address`
  are given in its configuration (`docs/COMPATIBILITY.md`, setjmp/longjmp row);
- no finalized main image, no `.pdata` section, or a caller address no entry
  covers (whether a handler must run is then unknown);
- a caller whose function has an exception handler (its termination handlers
  would have to run).

### __C_specific_handler

`EXCEPTION_DISPOSITION __C_specific_handler(PEXCEPTION_RECORD, PVOID
EstablisherFrame, PCONTEXT, PDISPATCHER_CONTEXT)`. The C language handler that
`.pdata` handler slots name (rexglue's code generator recognises the
`__imp____C_specific_handler` pointer stored before such functions). Only an
exception dispatcher or an unwinder calls it, to evaluate filters, run
`__finally` funclets or transfer control into an `__except` block. R-comp has
neither, so a call is always reported with the exception code and flags, the
establisher frame, the CONTEXT and DISPATCHER_CONTEXT pointers. Registering it
replaces the generic missing-import trap with that precise diagnostic; it never
returns a disposition.

## What would lift the limits

- An exception source that can continue: guest faults are diagnostics by
  design; `RtlRaiseException` would need dispatch.
- Generator support: split functions at `__except` / catch targets and expose
  them as entry points, plus a host-side unwind of the generated frames (C++
  exceptions are not available in the PS5 build, `-fno-exceptions`).
- Then: virtual unwinding of Xbox 360 prologues from `.pdata`, the
  `SCOPE_TABLE` walk of `__C_specific_handler`, and `RtlUnwind` across frames.

Until then a title that relies on SEH for normal control flow stops at the first
such use with one of the diagnostics above, never with an invented outcome.

## Reproduction

`runtime/tests/test_kernel_seh.cpp` (`rt_kernel_seh`): every CONTEXT field
group, the vector byte order, unwritable/misaligned CONTEXT; RtlUnwind's
implemented subset against a synthetic image with a `.pdata` section
(`tests/kernel_image_util.h`), and each diagnostic path; `__C_specific_handler`.
