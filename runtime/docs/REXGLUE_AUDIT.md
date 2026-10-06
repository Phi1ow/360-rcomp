# ReXGlue audit for the R-comp runtime (Agent 3)

- Audited: `rexglue-sdk` @ `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518` (v0.10.0), read-only in `$RCOMP_DEPS/rexglue-sdk`.
- Compared against: XenonRecomp @ `ddd128bcca99fe8bfbb99bea583c972351fa6ace` (`third_party/XenonRecomp`), our generator. **We do not switch generators.**
- Date: 2026-09-25. Nothing from ReXGlue is vendored or copied into `runtime/`.

## 1. License

`LICENSE`: BSD-3-Clause, "Copyright (c) 2026, Tom Clay", plus "Portions ... derived from the Xenia project: Copyright (c) 2022, Ben Vanik and Xenia project contributors".
In practice 464 files carry the Xenia BSD header (`Copyright 20xx Ben Vanik ... Released under the BSD license`) with a `@modified Tom Clay, 2026` line (e.g. `src/system/xmemory.cpp`, `src/system/util/object_table.cpp`, `src/kernel/xboxkrnl/*.cpp`, `src/filesystem/**`); newer files (`include/rex/ppc/*.h`, `src/system/runtime.cpp`, `guest_path.cpp`) are Tom Clay only.

Obligations if any code is reused or adapted:
- keep **both** notices (Tom Clay BSD-3 and the Xenia/Ben Vanik BSD notice) in source files, and reproduce them in the binary distribution's documentation (clause 2); for the PS5 title that means a third-party notices file shipped with the package;
- no use of the names of the holders/contributors for endorsement (clause 3);
- BSD-3 is compatible with our tree (MIT XenonRecomp, GPL-3.0 PS5 SDK wrappers); it adds no copyleft.
The export tables we *do* use (`xboxkrnl_table.inc`, `xam_table.inc`) are consumed from XenonRecomp's copy, which is byte-identical to ReXGlue's `src/kernel/xboxkrnl/export_table.inc` (verified with `diff`) and carries the Xenia BSD header; it is included read-only, not copied.

## 2. Side-by-side comparison

| Aspect | XenonRecomp @ddd128b (our generator) | ReXGlue @c94f5eb | Consequence |
| --- | --- | --- | --- |
| CPU context | `struct alignas(0x40) PPCContext` in `XenonUtils/ppc_context.h`; field set depends on `PPC_CONFIG_*` (`NON_ARGUMENT_AS_LOCAL`, `NON_VOLATILE_AS_LOCAL`, `CTR/XER/CR/RESERVED_AS_LOCAL`, `SKIP_LR`, `SKIP_MSR`). Default: 2688 bytes, r3@0, r1@16, r13@104, lr@256 (u64). | `include/rex/ppc/context.h`: same register order but **no config variants**, plus extra fields `vscr_sat` and `last_indirect_target` inserted after `fpscr`, before `f0`. | **Not ABI compatible**: every FPR/VR offset differs. A ReXGlue HLE body compiled against its context would read the wrong registers from our context. |
| Function signature | `void f(PPCContext& __restrict ctx, uint8_t* base)`; recompiled bodies `extern "C" __imp__sub_X`, callers use weak C++ `sub_X`. | Same shape `void f(PPCContext&, uint8_t*)`, but `REX_FUNC`/`REX_HOOK` define **`extern "C"`** symbols. | Same calling shape, different linkage and different context type → no link compatibility. |
| Memory base / addressing | `base + (uint32_t)ea`, byte-swapped volatile loads/stores (`PPC_LOAD_U32`...). One flat 4 GiB reservation (ours: `GuestMemory`, commit on demand, 64 KiB pages). | `GuestPtr(base, a) = base + a + PhysicalHostOffset(a)`; on Windows / macOS-arm64 addresses >= 0xE0000000 get +0x1000. Memory is a file-backed mapping with **aliased views** (0xA0000000/0xC0000000/0xE0000000 all view the same 512 MiB physical heap), Xenia heap layout (`xmemory.cpp` l.52-59). | Their memory model needs shared-memory aliasing (memfd/`CreateFileMapping`) that our `GuestMemory` does not provide and PS5 has not been shown to support. Physical-alias semantics are a future Agent 2 decision, not a runtime one. |
| Import representation | XEX thunk overwritten with `nop,nop,nop,blr`; symbol `__imp__<Name>` **only if** the ordinal is in the xboxkrnl/xam table; generated code *declares* `PPC_EXTERN_FUNC(__imp__<Name>)` (C++ linkage) and calls it by name. Ordinal is lost. Unknown ordinal / other module → the thunk is recompiled as an **empty function** (silent no-op). Variable imports ignored. | Codegen resolves `module@ordinal` to `__imp__<Name>` via `ExportResolver`, else `__imp__<module>_<ordinal>`; unresolved calls emit `REX_FATAL`. Runtime defines `extern "C" __imp__<Name>` via `REX_EXPORT`, registered by name in a static registry; variable exports patched through `SetVariableMapping`. | Our runtime must define C++-linkage `__imp__<Name>` itself (done: `src/import_thunks.cpp`). The silent no-op for unknown ordinals is a generator gap (reported to Agent 1/PRIME). |
| Import dispatch / missing | n/a (generator only). | 616 `REX_EXPORT_STUB*`/`REX_STUB*` exports in `src/kernel/xboxkrnl/*.cpp` (e.g. 146 in `xboxkrnl_misc.cpp`, 129 in `xboxkrnl_crypt.cpp`) **log a warning and return** (0 or a fixed value). | Violates our "no fake success" rule; not reusable as-is. Our registry traps every unregistered import with `RCOMP_FATAL_MISSING_IMPORT`. |
| Indirect dispatch | `PPC_LOOKUP_FUNC`: table at `PPC_IMAGE_BASE+PPC_IMAGE_SIZE` inside guest memory, unchecked; our prelude replaces it with `rcomp_call_indirect` (checked, Agent 1). | `REX_CALL_INDIRECT_FUNC` → `ResolveIndirectFunction` → `FunctionDispatcher::GetFunction`, falls back to `InvalidFunctionTrap` (REX_FATAL with `last_indirect_target`). | Same idea as our checked prelude; nothing to import. |
| Threading / TLS / stack | Generator has no model. `mftb` → `__rdtsc()`; `lwarx/stwcx.` → load + `__sync_bool_compare_and_swap` on the reserved value; `sync/lwsync/eieio` → nothing. | One host thread per `XThread` (16 MiB host stack), guest stack from the guest heap with guard pages at both ends filled 0xBE, r13 = KPCR (0x2D8 bytes: tls_ptr@0, pcr_ptr@0x30, stack_base@0x70, stack_end@0x74, PRCB@0x100), TLS slots from XEX TLS header, APC queue, suspend/resume, priorities; `mftb` → `REX_QUERY_TIMEBASE()` scaled to 50 MHz. | The KPCR field offsets are Xbox facts we reuse (documented in `guest_thread.h`). The full XThread is far bigger than needed and tied to `KernelState`. |
| Kernel objects / handles | none | `ObjectTable` (Xenia): handles `0xF8000000 + slot*4`, refcounted `XObject`, name table, `LookupObject<T>` returns null on type mismatch (caller then returns `X_STATUS_INVALID_HANDLE`), guest-side `X_DISPATCHER_HEADER` mirroring for events/mutants/semaphores/threads. | Semantics useful as reference; the implementation drags `XObject`, `KernelState`, global critical region, guest-object mirroring. |
| File system | none | `VirtualFileSystem` + devices (`HostPathDevice`, disc image, STFS, null) + symlinks (`game:` → device). `ResolvePath` canonicalises with `utf8_canonicalize_guest_path` (collapses `..` instead of rejecting), case-insensitive fallback lookup on the host, `HostPathDevice::Initialize` **creates the host directory** if missing when not read-only; no realpath containment check against host symlinks. | Useful model for later (ISO/STFS). For the sandbox we need explicit rejection, which theirs does not do. |

## 3. Per-component decision

| Component | Decision | Reason | License obligation |
| --- | --- | --- | --- |
| xboxkrnl/xam export tables (ordinal ↔ name) | **Reuse** (already vendored by XenonRecomp; identical file) | Pure data, exactly the tables the generator used to name `__imp__` symbols, so names match by construction. | Xenia BSD notice already in the `.inc` files; list in third-party notices. |
| Import registry / dispatch | **Write minimal ourselves** (done) | ReXGlue's registry is name-based `extern "C"` + stubs that fake success; ours must match XenonRecomp's C++-linkage symbols and trap. ~150 lines. | none |
| Handle table | **Write minimal ourselves** (done) | Need kind-checked lookup with distinct WrongHandleKind vs InvalidHandle and stale-handle detection; `ObjectTable` needs `XObject`/`KernelState`. ~120 lines. | none |
| Heap / NtAllocateVirtualMemory | **Write minimal ourselves** (done); keep Xenia's parameter validation rules as the **reference** for semantics. | Their `BaseHeap/VirtualHeap/PhysicalHeap` assume aliased file-backed views and 4 KiB page tables; adapting is larger and less verifiable than a 200-line host-metadata allocator over `GuestMemory`. | none (semantics are facts; no code copied) |
| Guest thread/stack/KPCR | **Write minimal ourselves** (done); KPCR offsets taken as documented facts. | `XThread` is ~1500 lines entangled with scheduler, APCs, kernel state. | none |
| VFS / host path device | **Write minimal ourselves** (done) for the sandbox; **adapt later** for disc image / STFS if titles need them. | Our sandbox must *reject* traversal and host-symlink escapes; theirs normalises `..` and may create directories. Disc/STFS parsers are worth adapting later (large, format-specific). | If STFS/ISO code is adapted: keep Tom Clay + Xenia BSD notices in those files and in shipped notices. |
| Synchronization (events, mutants, waits) | **Write minimal ourselves** now (host `Event`/`Mutex`), **adapt semantics later** for `KeWaitForMultipleObjects`/dispatcher headers. | Guest-visible dispatcher-header mirroring is needed only when titles use `Ke*` objects in guest memory; not needed for M3. | As above if adapted. |
| Time (`KeQuerySystemTime`, timebase) | **Write ourselves** (done for system time). Timebase: **blocked** on a generator decision (see §4). | ReXGlue scales `mftb` to 50 MHz in the generator; XenonRecomp emits raw `__rdtsc()`. | none |
| NT status codes, flag constants | **Reuse as facts** (public NT values) | Standard NTSTATUS/MEM_*/PAGE_*/FILE_* values. | none |

Rule applied: adaptation only where it is smaller and more verifiable than a minimal implementation. For M3 it never was.

## 4. Findings to escalate (not fixed here)

1. **Silent no-op imports (XenonRecomp)**: an import whose ordinal is not in the tables, or from a module other than `xboxkrnl.exe`/`xam.xex` (e.g. `xbdm.xex`, `xapi`), gets no symbol; its `nop,nop,nop,blr` thunk is then recompiled as an empty `sub_XXXXXXXX`. The call does nothing and returns the caller's r3. Fix belongs in the generator/config (Agent 1): fail generation, or emit a symbol `__imp__<module>_<ordinal>` that the runtime traps.
2. **`mftb` → `__rdtsc()`**: guest timebase runs at the host TSC rate, not 50 MHz, so `KeQueryPerformanceFrequency` cannot be implemented honestly; it is left unimplemented (traps). Needs a prelude/generator hook (Agent 1/PRIME).
3. **`lwarx/stwcx.` is a value CAS**, not a reservation (ABA). Acceptable on x86 for most spinlocks, but not identical to hardware.
4. **PPCContext layout depends on `PPC_CONFIG_*`**: runtime and generated code must be compiled with the same `ppc_config.h` (see interface proposal).
