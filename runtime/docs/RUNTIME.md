# rcomp guest runtime (Agent 3)

2026-09-28 update: XEX static TLS is parsed, snapshotted and initialized per
guest thread; `KeTls*` remains separate. Runtime shutdown quiesces managed
workers before memory destruction; external entry/callback producers must be
joined by the owner. IRQ caches carry a runtime lifetime identity.

Library `rcomp_runtime` (C++17, namespace `rcomp::rt`), headers in `runtime/include/rcomp/runtime/`.
Links against the platform implementation of `include/rcomp/{diag,guest_memory}.h` (Agent 2).

| Header | Service |
| --- | --- |
| `status.h` | `rt::Status` + NTSTATUS mapping (`to_ntstatus`) |
| `import_registry.h` | (module, ordinal) → `PPCFunc`; `dispatch_import`; `rcomp::hle_missing_import` |
| `handle_table.h` | typed, generation-checked handles |
| `guest_heap.h` | allocator over `GuestMemory` |
| `guest_thread.h` | guest stack / r1 / r13 / lr init, `run_guest_thread`, thread exit |
| `vfs.h` | sandboxed guest path → host file mapping, read-only files |
| `clock_sync.h` | monotonic clock, FILETIME, host `Mutex` / `Event` |
| `runtime.h` | process-wide `Runtime` (heap + handles + VFS), `register_xboxkrnl_hle()` |
| `guest_context.h` | includes XenonRecomp `ppc_context.h` the way generated code does |

## Start-up order (integration)

1. `GuestMemory::reserve()`; commit/`reserve_runtime_range()` the image and the function table.
2. `rt::runtime_init(&mem)` (heap excludes runtime ranges present at this point).
3. `rt::register_xboxkrnl_hle()`; `runtime()->vfs.mount("game", <host dir>)` (+ `"d"` if used).
   With the GPU (titles that render): `xenos::gpu_start(mem, cfg, rt::dispatch_graphics_interrupt,
   backend)` then `rt::register_xboxkrnl_video_hle()` (library `rcomp_runtime_video`,
   `include/rcomp/runtime/video.h`; needs an implementation of `include/rcomp/xenos_gpu.h`).
4. `rt::create_guest_thread(heap, {stack_size, entry, r3}, &ctx, &thread)`;
   `rt::run_guest_thread(thread, ctx, mem.base(), <PPCFunc of entry>, &exit_code)`.

## Import binding

XenonRecomp declares `PPC_EXTERN_FUNC(__imp__<Name>)` (C++ linkage) for every import it could name from
`XenonUtils/xbox/{xboxkrnl,xam}_table.inc`. `src/import_thunks.cpp` defines all 2620 function exports of both
tables from the same `.inc` files; each forwards to the registry. Unregistered → one line on stderr and exit 70:

```
RCOMP-FATAL kind=missing_import module=xboxkrnl.exe ordinal=0x00D2 name=NtCreateFile lr=0x82001234 r3=0x00000001 r4=0x00000002 r5=0x00000003 r6=0x00000004
```

Parameter combinations an implemented export does not support end in `kind=unimplemented` (never a fake status).

## Guest thread conventions

See `guest_thread.h`: stack from the guest heap with a 64 KiB PROT_NONE guard below it;
`r1 = stack_base - 0x50` (16-byte aligned, back chain 0); `r13` → zeroed KPCR-like block (tls_ptr@0x0,
self@0x30, stack_base@0x70, stack_end@0x74), 4 KiB TLS slot array; `lr = 0xFEEDFACC` sentinel.
Stack arguments of HLE calls are read at `r1 + 0x54 + 8*(n-8)` (caller frame).

## HLE: Xbox 360 semantics vs implemented (xboxkrnl.exe)

| Ordinal | Export | Implemented | Traps (`unimplemented`) |
| --- | --- | --- | --- |
| 0x0019 | ExTerminateThread | unwinds to `run_guest_thread`, exit code = arg | – (no thread object/callbacks yet) |
| 0x0084 | KeQuerySystemTime | FILETIME from CLOCK_REALTIME | – |
| 0x00CC | NtAllocateVirtualMemory | base 0, MEM_COMMIT[+RESERVE/TOP_DOWN/NOZERO/LARGE_PAGES], PAGE_READWRITE[+NOCACHE/WRITECOMBINE]; 64 KiB rounding/alignment; zeroed; address from 0x40000000-0x7F000000 | fixed base, reserve-only, MEM_RESET, other protections, debug memory |
| 0x00CF | NtClose | any runtime handle; bad → STATUS_INVALID_HANDLE | – |
| 0x00D2 | NtCreateFile | FILE_OPEN of existing regular file, read-only VFS, sync | other dispositions, FILE_DIRECTORY_FILE, non-DOS RootDirectory |
| 0x00DC | NtFreeVirtualMemory | MEM_RELEASE, size 0, base from NtAllocateVirtualMemory | MEM_DECOMMIT, debug memory |
| 0x00DF | NtOpenFile | as NtCreateFile with FILE_OPEN | as NtCreateFile |
| 0x00F0 | NtReadFile | synchronous, current or absolute offset, EOF → STATUS_END_OF_FILE | Event, APC |

Threads, synchronisation and TLS live in `src/hle_xboxkrnl_threads.cpp`.
The synchronization extension adds eight exports for semaphores, handle mutants
and atomic WaitAny/WaitAll; see [SYNC_OBJECTS.md](SYNC_OBJECTS.md) for its exact
supported subset, references and host validation.
Calendar conversion lives in `src/hle_xboxkrnl_time.cpp`; see
[OBJECTS_TIME.md](OBJECTS_TIME.md) for the TIME_FIELDS ABI, supported FILETIME
domain, native Windows reference oracle and explicit unsupported cases.

Pool and RTL memory/string operations add seven exports, with runtime-scoped
pool/VA/physical ownership, checked guest rights, big-endian patterns and counted
strings. See [POOL_RTL.md](POOL_RTL.md) for the ABI, pinned references, allocator
failure semantics and the still-unimplemented pool selectors/conversions.

Physical memory (`src/hle_xboxkrnl_memory.cpp`, registered by `register_xboxkrnl_hle`). Guest memory has
no aliasing: every physical allocation lives in the GPU window 0xA0000000-0xBFFFFFFF, physical address
P = guest address 0xA0000000 + P, whatever page size is requested.

| Ordinal | Export | Implemented | Traps (`unimplemented`) |
| --- | --- | --- | --- |
| 0x00B9 | MmAllocatePhysicalMemory | = Ex(flags, size, protect, 0, MAX, 0) | as Ex |
| 0x00BA | MmAllocatePhysicalMemoryEx | flags 0, PAGE_READWRITE[+NOCACHE/WRITECOMBINE], 4 KiB / 64 KiB / 16 MiB page rounding, physical range limits, alignment, top-down, zeroed; NULL when nothing fits | flags ≠ 0, read-only / other protections |
| 0x00BD | MmFreePhysicalMemory | blocks from the two above | any other address (`guest_access`) |
| 0x00BE | MmGetPhysicalAddress | addresses in the physical window | any other address (not GPU-visible here) |
| 0x00C5 | MmQueryAllocationSize | physical and heap allocations; 0 otherwise | – |

Video (`src/hle_xboxkrnl_video.cpp`, library `rcomp_runtime_video`): `VdQueryVideoMode`,
`VdQueryVideoFlags`, `VdGetCurrentDisplayInformation`, `VdGetCurrentDisplayGamma`, `VdSetDisplayMode`,
`VdInitializeEngines`/`VdShutdownEngines`, `VdGetGraphicsAsicID`, `VdSetGraphicsInterruptCallback`,
`VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`, `VdInitializeScalerCommandBuffer` (NOPs:
no display scaler in the emulated GPU), `VdSwap` (fetch constant + `XE_SWAP` packet), clock gating /
HSIO / EDRAM training (no emulated counterpart) and the variables `VdGlobalDevice`,
`VdGlobalXamDevice`, `VdGpuClockInMHz`, `VdHSIOCalibrationLock`. Graphics interrupts run the title's
callback as guest code on the raising host thread (`dispatch_graphics_interrupt`). Not registered
(unknown semantics, trap as missing imports): `VdGetSystemCommandBuffer`,
`VdSetSystemCommandBufferGpuIdentifierAddress`, `VdCallGraphicsNotificationRoutines`, `VdPersistDisplay`,
`VdInitializeEDRAM`, HDCP / closed caption / WSS / CGMS.

`KeQueryPerformanceFrequency` currently returns 50 MHz, while generated `mftb`
still uses host `__rdtsc()`; that consistency gap remains a generator decision
described in `REXGLUE_AUDIT.md` §4. Everything else not listed above traps as a
missing import. Differences from hardware that do not trap: allocation addresses/placement,
freed pages stay committed, files are case-sensitive on the host, writes are refused with
STATUS_ACCESS_DENIED (read-only VFS), every I/O completes synchronously.

## Build and test

`RCOMP_RUNTIME_DIAGNOSTICS` defaults to `OFF`. Set the CMake option to `ON`
for bring-up traces of file I/O, dispatcher waits/semaphores, contended spin
locks, XMA registers, input polling, memory queries and graphics callbacks.
The corresponding counters, clock reads, stack walks and diagnostic TLS are
compiled out when disabled. Direct compilation can select the same behavior
with `-DRCOMP_RUNTIME_DIAGNOSTICS=1` (default `0`). Fatal diagnostics,
unsupported-feature reports and `RCOMP-VD` swap timestamps remain enabled.
Wait deadlines, atomic lock semantics and callback execution are unchanged.

Host:

```
cmake -S runtime -B build/runtime-host -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build/runtime-host && (cd build/runtime-host && ctest --output-on-failure)
```

PS5 cross build (compile + archive + link of test binaries; nothing is run):

```
$RCOMP_DEPS/sdk/ps5-payload-sdk/bin/prospero-cmake -S runtime -B build/runtime-ps5 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/runtime-ps5
```
