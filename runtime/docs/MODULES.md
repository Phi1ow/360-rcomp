# XEX modules and AOT exports

Agent 3 workstream, 2026-09-28. The local implementation is limited to the
loaded title's metadata, HLE libraries that are actually linked and, since
2026-10-03, secondary XEX modules whose code was compiled into the title ahead
of time (section "AOT secondary modules"). There is no runtime code generation,
runtime-created trampoline, exposed host pointer or RWX allocation.

## Public sources and scope

Xenia source was read at explicit commit
`95a5c3ee250f80c3b9d139658649d9ffb6db3eec`, already pinned in `deps/deps.lock`,
using `git show SHA:path` in the local public checkout. This emulator's behavior
is an implementation reference, not an Xbox kernel specification or hardware
proof. None of its runtime subsystems is imported.

- Ordinals, variables and calls:
  [xboxkrnl_table.inc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc),
  [xboxkrnl_module.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc),
  [xboxkrnl_modules.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc),
  [xbox.h](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/xbox.h).
- LDR structure and header:
  [xmodule.h](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xmodule.h),
  [xex2_info.h](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/util/xex2_info.h).
  The MIT homebrew declaration
  [XexUtils Kernel.h](https://github.com/ClementDreptin/XexUtils/blob/d5afceed0e0167cbbced256d5853a4926afa7c25/src/Kernel.h),
  commit `d5afceed0e0167cbbced256d5853a4926afa7c25`, corroborates the
  0x64-byte LDR32 and names `NtHeadersBase` at +0x18. The inspected blob is
  `b44685a512c17b7a1096d97d43883cfad5899dfd`.
- XEX ordinal exports, loading and PE size:
  [cpu/xex_module.cc](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/cpu/xex_module.cc)
  and [XenonUtils/xex.cpp](https://github.com/hedge-dev/XenonRecomp/blob/ddd128bcca99fe8bfbb99bea583c972351fa6ace/XenonUtils/xex.cpp).
  The decompressed payload is already a loaded image with the XEX security
  `image_size`. `PointerToRawData` therefore does not remap it. The PE size can
  be larger and describe unloaded sections; the runtime preserves that metadata
  without expanding accessible memory.
- Ordinary PE format:
  [Microsoft PE format](https://github.com/MicrosoftDocs/win32/blob/e103fa4e8810bd8d42c4777e17081e24dbe62dbd/desktop-src/Debug/pe-format.md),
  commit `e103fa4e8810bd8d42c4777e17081e24dbe62dbd`.
  This source describes PE without resolving the ambiguity of named XEX exports.

## Bootstrap and lifetime

After `runtime_init` and HLE/video registration, and before `load_xex_image`,
call `runtime_prepare_main_module(ModuleConfig)`. The launcher supplies the
exact guest path and command line; neither a host path nor arguments are
inferred. Defaults are `game:\default.xex` and an empty command line.

The path must be ASCII, nonempty, qualified by a device whose name contains at
least two characters, and at most 1023 bytes. Its last nonempty component becomes
the module name. `xboxkrnl.exe` and `xam.xex` are reserved names. The command line
must be ASCII, contain no internal NUL, and be at most 4095 bytes. These bounds
are runtime policy. Quoting grammar and non-ASCII launch encodings are not
implemented.

Prepare allocates a 64 KiB guest RW block aligned to 64 KiB for the cells, command
line, three LDR entries and their UTF-16BE names. It registers the four real
variable storage areas below. The HMODULE cell remains null. The loader can then
relocate imports to these storage areas. Before mutating slots, it retains an
exact copy of the input header and `(module, ordinal, VA)` records for type 1
thunks.

After loading and `register_functions`, and before any guest thread, call
`runtime_finalize_main_module(const XexImage&)`. The runtime compares the base,
size and entry against the retained header; validates PPC32 PE, exports and AOT
addresses; and rejects an image overlapping a runtime heap. It copies the header
into a separate allocation rounded and aligned to 64 KiB, then protects it as
read-only. It publishes the real LDR and HMODULE only after all validations.
The retained header size is bounded to 16 MiB.

The two persistent allocations belong to `Runtime`, with a private generation
registry. HLE reads do not add module references. New threads must not run during
prepare/finalize. Shutdown quiesces threads before removing bindings owned by
this runtime and then destroys its owner. Releasing the GuestMemory reservation
belongs to the caller, as with other runtime heaps.

A normal preparation failure removes all its partial bindings. On hosts with
exceptions, `bad_alloc` becomes `OutOfMemory`. If persistent exhaustion prevents
even preparing free-list nodes, the runtime retains the allocation without a
binding until destruction; it does not become orphaned. Finalize's metadata
validation and allocations precede guest allocation and publication. A
protection failure attempts to restore RW before freeing; if recovery fails,
ownership is retained and any further finalization is refused. With
`-fno-exceptions`, host memory exhaustion remains terminal; no general recovery
is claimed.

An HMODULE is an exact guest LDR address. An old number reused at the same address
in a new runtime is indistinguishable from the new pointer: the private
generation prevents retaining old metadata, not this intrinsic pointer-ABI ABA.
The absence of a thread surviving shutdown is therefore a real contract
requirement.

## Exposed ABI

| Export | Ordinal | Real storage |
| --- | --- | --- |
| XexExecutableModuleHandle | 0x0193 | BE32 cell pointing to the main LDR; null before finalize |
| ExLoadedCommandLine | 0x01AE | Direct NUL-terminated ASCII bytes, with no extra pointer cell |
| KeDebugMonitorData | 0x0059 | Null BE32 cell: no Xbox monitor service is loaded |
| KeCertMonitorData | 0x0266 | Null BE32 cell: no Xbox certification service is loaded |

Import slots receive the addresses of these storage areas. PS5 services such as
FTP, kstuff, klogs or ShadowMount are not Xbox monitors; their presence does not
justify nonzero values in these cells.

The main LDR contains guest BE values, never a host pointer:

| Offset | Value |
| --- | --- |
| +0x00/+0x04 | Real circular list of the three loaded modules |
| +0x18 | Address of the real PE NT headers in the loaded image |
| +0x1C | Image base |
| +0x20 | Real PE `SizeOfImage`, even if it exceeds the XEX payload |
| +0x24/+0x2C | UNICODE_STRING8 descriptors for path and base name, UTF-16BE buffers |
| +0x38 | Actually loaded XEX size |
| +0x3C | Image entry |
| +0x40/+0x42 | Single bootstrap reference and local registry index |
| +0x44 | Original base, identical to the loaded base (no relocation) |
| +0x48/+0x50 | Real PE checksum and timestamp |
| +0x58 | Exact, protected copy of the XEX header |

The LDR subset above is the implemented contract. Other lists, flags, loaded
imports and dependency closure remain unimplemented and zero-initialized; no
complete Xbox loader contract is claimed. The checksum does not hide a host
handle. Kernel/XAM descriptors represent linked HLE libraries without a
fabricated PE, image or XEX; their absent image fields are zero.

| Function / ordinal | Contract |
| --- | --- |
| RtlImageXexHeaderField / 0x012B | r3=retained main header, r4=key. Low byte 00: BE32 value; 01: address of the word in the entry; otherwise: block address. Absent field: 0. |
| XexCheckExecutablePrivilege / 0x0194 | r3=bit index in real SYSTEM_FLAGS; returns 0 or 1. Index >=32: 0. |
| XexGetModuleHandle / 0x0195 | r3=name or zero for main, r4=BE32 output. Found: 0 and real LDR; absent: 1168 and null output. |
| XexGetProcedureAddress / 0x0197 | r3=HMODULE or zero for main; r4=ordinal or name if high bits are nonzero; r5=BE32 output. Success: 0 and real guest address. |

Accepted module names are the title's base name or exact guest path,
`xboxkrnl.exe` and `xam.xex`, with ASCII case comparison. Kernel/XAM export names
are exact and case-sensitive, from the pinned tables already linked to the
project. Queries may contain non-ASCII bytes: they simply do not match these
names; no additional encoding is inferred. Every string is bounded to 4096
bytes including NUL.

GetProcedureAddress: invalid module = `0xC0000008`, output preserved; absent
export = `0xC0000263`, output zeroed. Unreadable string pointers and non-RW output
produce `0xC0000005`, without partial writes. This pointer handling is runtime
policy, not proof of a hardware-kernel exception. Ordinal zero, whose contract
has not been established, causes fatal `RCOMP_FATAL_UNIMPLEMENTED`. HeaderField
on an unknown, null or unreadable header causes fatal `RCOMP_FATAL_GUEST_ACCESS`,
not a fake absent field. It does not arbitrarily validate a null kernel-header
pointer.

## Effective resolution and limitations

A kernel/XAM function must have a registered HLE AND an imported XEX thunk whose
`lookup_function(VA)` equals its actual `__imp__` wrapper. The new `import_thunk`
catalog contains pointers to already linked wrappers without creating a
function or VA. A wrapper ending in `MISSING_IMPORT` is not proof of support. A
known HLE without a static thunk or implementation initially caused a fatal
UNIMPLEMENTED. Since the September 29 work, a function lacking an implementation
in the loaded HLE module returns `0xC0000263` with null output during dynamic
lookup. An implemented function without an AOT address remains a fatal
integration diagnostic. Variables require real registered, readable storage.
A future catalog of reserved AOT addresses must be decided during generation;
no address is invented on demand.

The lookup change follows public rexglue `c94f5eb`,
`src/system/kernel_module.cpp:39-83` and
`src/kernel/xboxkrnl/xboxkrnl_modules.cpp:151-197`: when no native implementation
is resolved, lookup produces a null address, and the service returns the absent
export status. The actual GTA IV caller checks signed status and then the
pointer for its optional XAM queries `0x24` and `0x50`, and has its own fallback
paths. R-comp neither forces the branch nor implements a fake network service.
The production inventory remains blocked on absent static imports; a direct
call to an absent import remains fatal `missing_import`. Not-found lookup is
tested by name and ordinal without registering a stub.

For the main module, the ordinal table at the BE32 address security+0x160 is
parsed: +0x20 is the high base to shift by 16, +0x24 the count, +0x28 the first
ordinal, and +0x2C the BE32 offsets. The entire table and each target stay inside
the actually loaded XEX image, with 64-bit arithmetic. A target must belong to
exactly one PE section, classified by VirtualSize and its characteristics. Code
requires an exact AOT address, rechecked at lookup; data must be readable.
Descriptive sections may exceed the payload but stay within PE SizeOfImage and
4 GiB.

This ordinal parser is a limited structural model: the table's magic, module
and version words are neither interpreted nor authenticated. The original
oracle checks the formula, bounds and dispatch without claiming universal XEX
export validation. No export table is fabricated for the game or HLE libraries.

**BLOCKED:** named XEX exports (key 0x00E10402), ordinary PE exports and their
forwarders. The base of XEX name tables is contradictory between Xenia's comments
and implementation; public research provided no independent corroboration.
Finalize explicitly rejects these variants with `Unsupported` rather than
incorrectly answering "absent". The case without an export table works normally.
No active monitor, exported thread object or invented kernel version is added.
`XexLoadImage` / `XexUnloadImage` (3 October 2026) reference-count the existing
images (main module, `xboxkrnl.exe`, `xam.xex`; LDR LoadCount at +0x40): a
missing file returns the open's NTSTATUS, an existing other XEX traps unless it
is an AOT secondary module (below), and the bootstrap reference is never
released (`KERNEL_IMPORTS_20261003.md`).

## AOT secondary modules (title DLLs)

Added 2026-10-03 for Halo 3, which calls
`XexLoadImage("game:\WaveShell-Xbox.dll", 9, 0, &h)` and whose disc also carries
`WavesLibDLL.dll`, `waves/L360.dll` and `waves/Q10.dll` (module flags 0x9, fixed
bases 0x88000000 / 0x8A000000 / 0x89400000 / 0x8B900000; L360 and Q10 import
from `WavesLibDLL.dll`; all import `xboxkrnl.exe`). R-comp has no JIT: the build
recompiles each module and the title registers one `rcomp::AotModule` per module
(`include/rcomp/aot_modules.h`, owner PRIME). Implementation:
`src/aot_modules.cpp`, the Xex* exports in `src/hle_xboxkrnl_modules.cpp` and the
import resolver hook of `src/xex_loader.cpp`.

### Registry

`register_aot_modules(modules, count)` keeps the caller's pointer (static
descriptors), once per title; `rt::clear_aot_modules()` resets it (bootstrap and
tests only). It returns false, registering nothing, for: a null or empty list; a
second registration; a disc path that is empty or has an empty, `.` or `..`
component, a `:` or a non-printable character; an empty module name or one with
a separator; a base that is zero or not 64 KiB aligned; an empty image or one
beyond 4 GiB; an entry point outside the image; a function entry that is null,
misaligned or outside the image; and, between descriptors, a duplicate disc path
(ASCII case-insensitive, `/` or `\`), a duplicate module name (case-insensitive,
or equal after XenonRecomp's `_` substitution) or image ranges sharing a 64 KiB
page. `find_aot_module` applies the same normalization; `aot_modules` returns the
registered array.

### Guest address-space decision

Secondary images live at their fixed XEX bases: their code is compiled for those
addresses and nothing is relocated. The 64 KiB-page XEX window
0x80000000-0x9FFFFFFF holds only images: the runtime heap is
0x00010000-0x7F000000 (`GuestHeap`; the opaque arena at 0x70000000 and the Xenos
register window at 0x7FC80000 are inside it), the physical windows start at
0xA0000000, and the main image's lookup table is not materialized in guest
memory (`lookup_function` is a host table). Nothing is reserved up front: a load
commits exactly the image's 64 KiB pages and stops with an explicit
`rcomp_fatal` naming the range if they overlap the heap, the physical windows (or
reach 0xA0000000), the main image, a runtime range or any committed page. The
last unload decommits them, so a stale access faults. Halo 3's modules occupy
0x88000000-0x880A0000, 0x89400000-0x89470000, 0x8A000000-0x8A0C0000 and
0x8B900000-0x8B970000, clear of its main image 0x82000000-0x832A0000.

### Image source

The disc file is opened through the VFS (devices `game:` and `d:`, optional
`\??\` prefix); when the open fails its NTSTATUS is returned. A plain XEX2 (no
encryption, no compression) is loaded as is. Retail modules are encrypted and
compressed and the runtime holds no XEX key (none may be in the repository), so
the image decoded on the host at packaging time (`rcomp_xex_decode`) is read from
the directory set by `rt::runtime_configure_module_images(host_root)`, at
`<host_root>/<disc_path>`. It must carry the disc file's exact header, the two
encryption/compression words excepted; the header contains the image digest, so
another image cannot be substituted silently. No root, a missing image or a
header mismatch is an explicit fatal.

### XexLoadImage

For a name that is not the main or an HLE module:

1. A loaded secondary module matching the bare name, the LDR path, the import
   name or the same disc file gains one reference (LDR LoadCount +0x40 mirrors
   the host count) and its handle is returned. Flags other than the image's
   module flags, or a non-zero MinimumVersion, trap (no established result).
2. A name without `:` is `STATUS_NO_SUCH_FILE`; a device path that does not
   exist returns the VFS NTSTATUS, output untouched.
3. An existing file without a descriptor traps as before.
4. A non-zero MinimumVersion traps. Then, under the loader lock: read and decode
   (above); validate the XEX2 header; require the DLL module flag (0x8) and
   `ModuleTypeFlags` equal to the image's flags; require base, size and entry
   equal to the descriptor's; reject named exports (0xE10402) and static TLS (no
   contract for a DLL); check the address range.
5. Dependencies first, as the console loader does: every import library other
   than `xboxkrnl.exe` / `xam.xex` must be the `module_name` of another
   descriptor; it is loaded (its DllMain runs first) or referenced once.
   Self-imports, cycles and a missing dependency file trap.
6. The module's `functions` are added with `rcomp::add_functions` (a reload finds
   them all registered with the same host functions; any other overlap traps).
7. `load_xex_image` maps the image and binds imports exactly as for the main
   image (variables from the registry, kernel function slots poisoned), plus the
   type 0 slots of module libraries, which receive the export's guest address
   (the console loader's IAT write). Unresolved variables or module imports (an
   absent ordinal or a hole) trap. Every kernel thunk must map to its `__imp__`
   wrapper in the function table; every module thunk must resolve to an export
   and have an AOT function.
8. PE metadata as for the main module, with one difference: a PE export
   directory is accepted only when provably not loaded, i.e. beyond the XEX
   payload (WaveShell-Xbox, WavesLibDLL) or with its 40-byte header inside a XEX
   resource (L360, Q10: resource `xcli1000` occupies the `.edata` RVA). imagexex
   turns PE exports into the XEX ordinal table; a possibly loaded PE table is
   `Unsupported`. The XEX ordinal table is parsed as for the main module, except
   that a zero offset is a hole (no export). An export in an executable section
   must be an AOT function.
9. A read-only, 64 KiB-aligned copy of the header and an LDR entry (heap
   allocations) are created with the main module's field subset (+0x18 NT
   headers, +0x1C base, +0x20 PE SizeOfImage, +0x24/+0x2C path and base name,
   +0x38 XEX size, +0x3C entry, +0x40 LoadCount 1, +0x42 index 3+n, +0x44 base,
   +0x48 checksum, +0x50 timestamp, +0x58 header copy) and linked at the tail of
   the circular load-order list (main, xboxkrnl, xam, then secondary modules in
   load order). The exports are published.
10. `DllMain(handle, DLL_PROCESS_ATTACH = 1, 0)` runs on the calling guest thread
    (`call_guest_routine_on_current_context`; Xenia's `FinishLoadingUserModule`
    passes the same arguments) with the loader lock held; FALSE traps (the
    console's failure path is not established). The handle is written and
    `STATUS_SUCCESS` returned.

The loader lock is recursive and held across DllMain, like the console/Windows
loader lock: a DllMain may call XexLoadImage, XexGetModuleHandle and the other
module queries; a DllMain waiting for another thread blocked on the loader
deadlocks, as on Windows.

### XexUnloadImage

A secondary handle loses one reference. The last one runs
`DllMain(handle, DLL_PROCESS_DETACH = 0, 0)`, unpublishes the exports, unlinks
the LDR entry, frees the LDR block and the header copy, decommits the image pages
and then releases the references held on its dependencies (which may detach them
in turn). The AOT functions stay in the function table (it has no removal; the
addresses stay reserved for this module); a reload re-reads the image from the
disc, rebinds imports and runs DllMain again. Releasing the last reference while
the module is still loading or detaching traps.

### Queries

* `XexGetModuleHandle`: base name, LDR path, import name or disc file of a loaded
  module.
* `XexGetProcedureAddress`: by ordinal from the XEX table (absent ordinal:
  `0xC0000263` with a null output; a hole traps: no established result); by
  name: `0xC0000263` (no name table is loaded, as for the main module); ordinal
  zero traps.
* `XexGetModuleSection`: the module's own resource directory (0x2FF), bounded by
  its image.
* `RtlImageXexHeaderField`: also accepts a loaded module's header copy (LDR
  +0x58).

### Calls through module import thunks

XenonRecomp names a thunk of a non-system library
`__imp__rcomp_unresolved_<library>_<ordinal>` and emits
`PPC_UNRESOLVED_IMPORT("<library, non-alphanumerics as _>", ordinal)`, which
today ends in `rcomp_unresolved_import` (fatal). The runtime provides
`rcomp_module_import(ctx, base, module, ordinal)` (`rcomp/runtime/modules.h`): it
finds the loaded module by import name (or its `_` form) and calls the export's
AOT function on the caller's context, which is what the console thunk
(`lis r11; ori r11; mtctr r11; bctr`) does: no frame, LR unchanged. An unknown
library or ordinal stops with `RCOMP_FATAL_MISSING_IMPORT` and the same line as
`rcomp_unresolved_import`; a data export stops with `INDIRECT_TARGET`. Each call
takes one short mutex (`ModuleState::export_mutex`), never the loader lock.
**Proposal to PRIME (`include/rcomp/ppc_prelude.h`):**
`#define PPC_UNRESOLVED_IMPORT(module, ordinal) rcomp_module_import(ctx, base, module, (uint32_t)(ordinal))`
with its declaration, so that generated module thunks reach it. Indirect calls
through module IAT slots already work: the slot holds the export address and
`lookup_function` finds its AOT function.

### Requirements on the build (cpu/, tools/, app/m6)

* Register the descriptors before the guest starts and call
  `runtime_configure_module_images` with the packaged directory of decoded
  modules (same relative paths as on the disc) when a module is encrypted or
  compressed.
* `functions` must contain the entry point, every export target in an executable
  section (XenonRecomp may not discover exported functions the DLL never calls
  itself) and a function at every import thunk.
* Kernel/XAM thunks of a module must map to the shared `__imp__<Name>` wrappers
  of `src/import_thunks.cpp`, not to prefixed copies.

### Validation (3 October 2026)

`rt_aot_modules` (synthetic XEX2/PE32 DLLs, `TESTDOUBLE_` host functions):
registry rejections; load of a module whose dependency is loaded first (DllMain
order and arguments, re-entrant lookup from DllMain), LDR fields and list,
imports (IAT slots, kernel variable, thunk call through `rcomp_module_import`,
indirect call through a slot), exports (code, data, absent, hole, by name,
ordinal zero), header field and resources, reference counting by path and name,
DETACH order on unload, decommit, reload, missing-file status, existing XEX
without descriptor, fixed base over the heap, the encrypted-file path (no root,
missing image, header mismatch, success), both accepted PE `.edata` forms and the
rejected one, DllMain returning FALSE; `rcomp::add_functions` is the real one of
`cpu/runtime/func_table.cpp` (Agent 1). A local probe (under `build/`, not
committed: it reads disc content) loaded the four real Halo 3 DLLs through this
path from the encrypted disc files and `rcomp_xex_decode` images, with
TESTDOUBLE function tables: WaveShell-Xbox (19 exports); L360, which loaded
WavesLibDLL (145 exports) first, with 2046 exports and 81 module imports bound;
Q10 (80 module imports); `XexGetModuleSection(L360, "xcli1000")` = 0x89460000 /
0x21C1. Host only: **NOT TESTED** on the PS5 and with the modules' real
recompiled code.

## Reproducible validation

Command executed from the root with Cygwin Clang 22.1.8; exit code **0**:

```powershell
& build/prime-host-tools/cygwin/bin/bash.exe --noprofile --norc -c 'export PATH=/usr/bin:/bin; cmake -S runtime -B build/runtime-modules-20260929/host -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DRCOMP_XENONRECOMP="$PWD/build/cpu-xenonrecomp-cfg-src" -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fsanitize-trap=undefined -g" -DCMAKE_EXE_LINKER_FLAGS="" && cmake --build build/runtime-modules-20260929/host -j 4 && ctest --test-dir build/runtime-modules-20260929/host --output-on-failure --output-log build/runtime-modules-20260929/ctest.log'
```

**PASS: 20/20 runtime UBSan tests**, including `rt_modules` on an original XEX/PE
loaded by the real loader, and neighboring heap, XEX, thread, TLS, lifetime,
HLE, pool/Rtl, video and XAM suites. New cases check indirections, metadata and
permissions, four HeaderField forms, names and statuses, code/data exports,
incorrect wrappers or missing AOT, malformed headers, invalid pointers and
preserved output, binding removal at shutdown and subsequent creation. An empty
string cannot select an ordinal-only export.

`rt_heap_failures` also injects **22** host-exhaustion points during prepare and
**8** during finalize, each followed by an ownership check or successful
finalization after recovery. The allocation double uses the `TESTDOUBLE_`
prefix; the tested runtime is production code. Logs:
`build/runtime-modules-20260929/ctest.log` and
`build/runtime-modules-20260929/host/Testing/Temporary/LastTest.log`.

An initial UBSan configuration without trap failed: Cygwin does not provide
`libclang_rt.ubsan_standalone.dll.a`. The `-fsanitize-trap=undefined` mode above
is instrumented and does not depend on that absent library.

**Host PASS reported by PRIME:** original PPC-fixture pipeline, 29 checks, seven
negative packages and three PE variants (SizeOfImage larger than payload,
section outside the loaded image, divergent PointerToRawData), in
`build/prime-modules-20260928/host-rehearsal/test.log`. This is host AOT evidence
separate from the C++ tests, not proof of GTA IV boot.

**NOT TESTED by Agent 3:** PS5 execution, real partial OS-protection failure,
host exhaustion on PS5, active monitors, concurrency with non-quiesced
destruction, and full LDR metadata compatibility. Console evidence and the
final inventory belong to PRIME and are recorded separately.
