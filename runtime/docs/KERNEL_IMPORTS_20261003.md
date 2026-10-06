# Catalog kernel imports — 3 October 2026

Agent 3 workstream. Status: **PASS (HOST ONLY)** for every row marked PASS;
PS5 execution: **NOT TESTED**. xboxkrnl.exe imports of the checked titles
(Gears of War 2, Halo 3, Fallout: New Vegas, Skate 3, NARUTO STORM 3) that were
not registered. Each export states its Xbox 360 contract and subset in its
source; outside the subset it ends in `RCOMP_FATAL_UNIMPLEMENTED` (or
`guest_access` for guest misuse), never in a fake success. Signatures:
XenonRecomp's copy of Xenia's export table (ordinals), Xenia / rexglue-sdk
(BSD-3, read for signatures and layouts only, no code copied), community xbdm
reverse engineering (`copeison/xbdm` `KernelExports.h`) and the NT routines of
the same name.

| Export | Ordinal | File | Status |
| --- | --- | --- | --- |
| RtlCaptureContext | 0x0119 | `src/hle_xboxkrnl_seh.cpp` | PASS (`rt_kernel_seh`), see `SEH.md` |
| RtlUnwind | 0x0147 | `src/hle_xboxkrnl_seh.cpp` | PASS for the no-frame subset; explicit fatal otherwise (`SEH.md`) |
| __C_specific_handler | 0x01A5 | `src/hle_xboxkrnl_seh.cpp` | explicit fatal: needs a dispatcher (`SEH.md`) |
| IoCompleteRequest | 0x0035 | `src/hle_xboxkrnl_more.cpp` | explicit fatal: R-comp dispatches no IRP to guest drivers |
| IoInvalidDeviceRequest | 0x0041 | `src/hle_xboxkrnl_more.cpp` | explicit fatal: same |
| NtWriteFileGather | 0x0100 | `src/hle_xboxkrnl_devices.cpp` | PASS (`rt_kernel_more`) |
| ObLookupAnyThreadByThreadId | 0x010A | `src/hle_xboxkrnl_objects.cpp` | PASS (`rt_kernel_more`) |
| ObOpenObjectByPointer | 0x010E | `src/hle_xboxkrnl_objects.cpp` | PASS for worker threads; explicit fatal for objects without a handle class |
| KeInitializeDpc | 0x006F | `src/hle_xboxkrnl_dpc.cpp` | PASS (`rt_kernel_dpc`) |
| KeInsertQueueDpc | 0x007B | `src/hle_xboxkrnl_dpc.cpp` | PASS (`rt_kernel_dpc`) |
| KeRemoveQueueDpc | 0x008E | `src/hle_xboxkrnl_dpc.cpp` | PASS (`rt_kernel_dpc`; the pair of KeInsertQueueDpc) |
| KeQueryBasePriorityThread | 0x0081 | `src/hle_xboxkrnl_more.cpp` | PASS (`rt_kernel_more`) |
| KeSetCurrentProcessType | 0x009A | `src/hle_xboxkrnl_process.cpp` | PASS for type 1 (user); explicit fatal for 0/2 |
| KeSetCurrentStackPointers | 0x009B | `src/hle_xboxkrnl_more.cpp`, `src/host_fiber.cpp` | PASS for the calling thread, including xapi fiber switches (`rt_fibers`, `THREAD_OBJECTS.md` "Guest fibers"); explicit fatal for another thread or an unknown continuation |
| MmCreateKernelStack | 0x00BB | `src/hle_xboxkrnl_more.cpp` | PASS (type 0) |
| MmDeleteKernelStack | 0x00BC | `src/hle_xboxkrnl_more.cpp` | PASS (also releases the host contexts of fibers suspended on the stack) |
| MmIsAddressValid | 0x00BF | `src/hle_xboxkrnl_more.cpp` | PASS |
| MmMapIoSpace | 0x00C2 | `src/hle_xboxkrnl_more.cpp` | PASS for committed physical memory; explicit fatal for MMIO |
| MmLockAndMapSegmentArray | 0x00C0 | `src/hle_xboxkrnl_more.cpp` | explicit fatal: no established contract |
| MmUnlockAndUnmapSegmentArray | 0x00C9 | `src/hle_xboxkrnl_more.cpp` | explicit fatal: no established contract |
| NtCreateIoCompletion | 0x00D3 | `src/hle_xboxkrnl_threads.cpp` | PASS (`rt_kernel_dispatch`) |
| NtSetIoCompletion | 0x00F8 | `src/hle_xboxkrnl_threads.cpp` | PASS (the producer NtRemoveIoCompletion needs) |
| NtRemoveIoCompletion | 0x00F4 | `src/hle_xboxkrnl_threads.cpp` | PASS (`rt_kernel_dispatch`) |
| NtQueueApcThread | 0x00E3 | `src/hle_xboxkrnl_threads.cpp` | PASS (`rt_kernel_dispatch`) |
| NtSignalAndWaitForSingleObjectEx | 0x00FB | `src/hle_xboxkrnl_threads.cpp` | PASS (`rt_kernel_dispatch`) |
| RtlImageNtHeader | 0x01A9 | `src/hle_xboxkrnl_more.cpp` | PASS (`rt_kernel_xex`) |
| XexLoadImage | 0x0199 | `src/hle_xboxkrnl_modules.cpp`, `src/aot_modules.cpp` | PASS (host) for loaded modules, missing files and AOT secondary modules (`MODULES.md`); explicit fatal for an image not compiled into the title |
| XexUnloadImage | 0x01A1 | `src/hle_xboxkrnl_modules.cpp` | PASS; explicit fatal for the bootstrap reference |
| VdSetDisplayModeOverride | 0x01D4 | `src/hle_xboxkrnl_more.cpp` | PASS for "no override"; explicit fatal for a real override |
| ExEventObjectType (variable) | 0x000E | `src/thread_object.cpp` | PASS (identity token, `rt_kernel_more`, `rt_kernel_fields`) |

## User APCs and alertable waits

`NtQueueApcThread(HANDLE Thread, ApcRoutine, ApcRoutineContext, Argument1,
Argument2)` queues `ApcRoutine(ApcRoutineContext, Argument1, Argument2)` for the
target guest thread (a thread handle or the current-thread pseudo-handle). The
dispatcher now delivers user APCs with NT's rules, in every wait that has the
NT `WaitMode`/`Alertable` pair: `KeWaitForSingleObject`,
`KeWaitForMultipleObjects`, `NtWaitForSingleObjectEx`,
`NtWaitForMultipleObjectsEx`, `KeDelayExecutionThread` and
`NtSignalAndWaitForSingleObjectEx`:

- only a **user-mode** (`WaitMode` 1) **alertable** wait delivers;
- an object that is already signalled wins (the wait is satisfied normally,
  the APC stays queued);
- otherwise a pending APC, or one queued while the thread waits, ends the wait
  with `STATUS_USER_APC` (0xC0) after every queued APC ran, oldest first, on the
  waiting thread's own guest context (r1/r13/lr preserved, as for a call);
- an exited thread cannot be queued to (`STATUS_UNSUCCESSFUL`), and the APCs of
  a thread that exits are dropped;
- the routine must be in the AOT function table, checked when queued
  (`indirect_target`).

Waits without APCs are unchanged (one relaxed atomic load).

## Signal and wait

`NtSignalAndWaitForSingleObjectEx(SignalHandle, WaitHandle, WaitMode,
Alertable, Timeout)`: the NT routine plus the `WaitMode` of every Xbox "Ex"
wait (the timeout in r7 agrees with Xenia; Xenia names r5 "alertable" and r6
unknown, this follows the `NtWaitForSingleObjectEx` order). Event: set;
semaphore: release 1 (`STATUS_SEMAPHORE_LIMIT_EXCEEDED` at the limit); mutant:
release one level if owned (`STATUS_MUTANT_NOT_OWNED` otherwise). The signal and
the wait happen under one dispatcher hold, so no thread observes the signal
before the caller waits. A failed signal waits for nothing.

## I/O completion ports

A port is a new handle kind (`HandleKind::IoCompletion`), NT's KQUEUE: a FIFO of
`{Key, ApcContext, Status, Information}` packets. `NtSetIoCompletion` appends;
`NtRemoveIoCompletion(Port, *Key, *ApcContext, IO_STATUS_BLOCK*, Timeout)`
waits (not alertable) for the oldest packet, `STATUS_TIMEOUT` with untouched
outputs on timeout. A port is waitable with the other waits: signalled while it
holds a packet, without consuming it. NULL outputs are skipped (Xenia; NT would
fault). `NumberOfConcurrentThreads` is recorded, not enforced (the host
scheduler decides). Named ports trap like every named object. Packets come from
`NtSetIoCompletion` and from file I/O on associated files (next section).

## File completion ports (NtSetInformationFile class 30)

Requested by PRIME after the Halo 3 diagnostic build on the PS5 reported
`RCOMP-IO NtSetInformationFile class=30 not served: 0xC0000003 lr=0x8259FAE0`.

`FileCompletionInformation` (30) is `{HANDLE Port; PVOID Key}`, 8 bytes on the
Xbox 360 (NT `FILE_COMPLETION_INFORMATION` with 32-bit fields). Rules, from the
NT I/O manager (`IopSetCompletion` / Win32 `CreateIoCompletionPort` contract):

- the port handle must be an I/O completion port (`STATUS_OBJECT_TYPE_MISMATCH`
  for another kind, `STATUS_INVALID_HANDLE` for an unknown one); Length < 8 is
  `STATUS_INFO_LENGTH_MISMATCH`;
- the association is per **file object** (every duplicate handle shares it)
  and permanent: a second association is `STATUS_INVALID_PARAMETER`;
- a synchronous-I/O file object (opened with `FILE_SYNCHRONOUS_IO_NONALERT`,
  now recorded by NtCreateFile/NtOpenFile as `OpenRequest::synchronous_io`) is
  refused with `STATUS_INVALID_PARAMETER` (Win32: CreateIoCompletionPort on a
  handle not opened with FILE_FLAG_OVERLAPPED fails), so synchronous handles
  never queue packets;
- success writes IOSB `{STATUS_SUCCESS, 0}`; the file object keeps a reference
  to the port, which therefore outlives its handle.

`NtQueryInformationFile` class 30 answers `STATUS_INVALID_INFO_CLASS`: NT has no
query length for this set-only class.

Completion: `NtReadFile`, `NtWriteFile`, `NtReadFileScatter` and
`NtWriteFileGather` queue one packet `{Key, ApcContext, Status, Information}` to
the file's port when the call passed a non-NULL `ApcContext` (r6, the OVERLAPPED
pointer Win32 passes unless the event handle has its low bit set) and the
request succeeded. R-comp completes every request at the call, which on an
overlapped handle NT treats as a completed asynchronous request: the IOSB is
written, the event set and the packet queued (the Xbox 360 has no
`FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`). A request that fails returns its error
and queues nothing (Win32: no packet when ReadFile fails). An APC routine on a
port-associated file is `STATUS_INVALID_PARAMETER` (NT); elsewhere it still
traps (APC routines on file I/O are not implemented).

Halo 3 (PPSA88371, generated code under
`rcomp-installer/build/work/PPSA88371-20261003-162704/inventory/ppc`):

- `sub_8259FA68` is XAPI `CreateIoCompletionPort(File, ExistingPort, Key,
  Concurrency)`: `NtCreateIoCompletion(&port, 0x1F0003, NULL, Concurrency)` when
  no port is given, then `NtSetInformationFile(File, &iosb, {port, key} at
  r1+88, 8, 30)` (the reported lr 0x8259FAE0). It is called with File = -1 (port
  only) and, from `sub_8221A6xx` (ppc_recomp.25.cpp), with the handle of
  `CreateFile(name, GENERIC_READ, share 3, OPEN_EXISTING, 0x70000000 =
  OVERLAPPED|NO_BUFFERING|RANDOM_ACCESS)`, so an overlapped handle: the
  association is accepted.
- `sub_82084EF0` is `GetQueuedCompletionStatus`: `NtRemoveIoCompletion(port,
  &Key, &ApcContext(-> *lpOverlapped), &iosb{Status, Information(->
  *lpBytes)}, Timeout)`, the argument layout implemented here.
- `sub_82101C28` is XAPI `ReadFile(File, Buffer, Length, &Read, lpOverlapped)`.
  With an OVERLAPPED it sets `Internal = STATUS_PENDING` and calls the read
  routine through a function table (`[[0x827207CC]+16]`, a `bctrl`, so no
  direct `bl` to the import thunk) with NtReadFile's arguments: File, Event =
  `hEvent`, ApcRoutine 0, **ApcContext = the OVERLAPPED** (0 when `hEvent` has
  its low bit set), IoStatusBlock = the OVERLAPPED (`Internal`/`InternalHigh`),
  Buffer, Length, ByteOffset from `Offset`/`OffsetHigh`; it treats
  STATUS_PENDING and success alike and STATUS_END_OF_FILE as an error. Without
  an OVERLAPPED it passes ApcContext 0 and waits on the file handle when the
  status is STATUS_PENDING. That table entry is the only read path that can
  carry an ApcContext: the title's three direct `NtReadFile` calls pass 0 (the
  synchronous helper `sub_82584710` and two configuration reads), and
  `NtReadFileScatter` is imported without a call site. Assumption (NOT TESTED on
  the PS5): the table entry is the `NtReadFile` thunk; the argument layout above
  is exactly what this change consumes.
- Halo 3 also contains a guest file-system driver (`IoCreateDevice`,
  `IoCompleteRequest` in ppc_recomp.90/91): R-comp does not route files to guest
  drivers, so those paths stay unreachable (`IoCompleteRequest` is an explicit
  fatal if reached).

## DPCs

KDPC (0x1C bytes, rexglue-sdk XDPC layout): `+0 u16 Type (19)`, `+2/+3`
processor bytes, `+4 LIST_ENTRY`, `+0xC DeferredRoutine`, `+0x10
DeferredContext`, `+0x14/+0x18 SystemArgument1/2`. `KeInitializeDpc` writes
type, processor bytes, routine and context. `KeInsertQueueDpc` returns FALSE
for a DPC already queued (arguments unchanged), else stores the arguments and
queues it. One runtime DPC worker (a host thread with its own guest stack, PCR
and thread identity, started on the first insertion, joined by
`runtime_shutdown`) runs `DeferredRoutine(Dpc, DeferredContext, Arg1, Arg2)`
in FIFO order at `DISPATCH_LEVEL`. All DPCs run on that one "processor", so
routines never run concurrently (NT's per-processor guarantee). A DPC is
dequeued before its routine runs, so it may queue itself again.
`KeRemoveQueueDpc` cancels a queued DPC. The queued state is runtime-side; the
guest `LIST_ENTRY` is not linked into a kernel list (none exists in R-comp, as
in Xenia). A routine that terminates its thread or returns at another IRQL is a
`guest_trap`.

## Object manager

- `ObLookupAnyThreadByThreadId(ThreadId, PKTHREAD*)`: the thread object with
  that id gains a reference (released by `ObDereferenceObject`), including an
  exited thread whose object is still owned (NT finds a thread object until it
  is deleted). Unknown id: `STATUS_INVALID_PARAMETER`, the NT
  `PsLookupThreadByThreadId` code (rexglue's `ObLookupThreadByThreadId` uses
  `STATUS_NOT_FOUND`; the console value is not independently established).
- `ObOpenObjectByPointer(Object, PHANDLE)` (two-argument Xbox form: xbdm,
  Xenia): a new ordinary thread handle to a worker thread Body. The bootstrap
  thread and interrupt/DPC contexts have no handle object, device objects no
  handle class: explicit fatal. Unknown pointer: `guest_access`.
- `ExEventObjectType` (variable 0x000E) is registered with `ExThreadObjectType`
  by `register_thread_object_type_variable()` (no bootstrap change): an
  identity-only token at `0x70000400` in the kernel token page. As a type filter
  of `ObReferenceObjectByHandle`, a non-event handle is
  `STATUS_OBJECT_TYPE_MISMATCH`; an event handle traps, because R-comp's events
  are host dispatcher objects without a guest KEVENT Body (giving them one is
  the next step if a title needs it).

## Memory and threads

- `MmCreateKernelStack(Size, Type)`: zeroed read/write stack of Size rounded to
  4 KiB from the runtime heap; returns the high end (stack base). NULL when the
  heap is exhausted. Type other than 0 traps; size 0 or above 16 MiB is guest
  misuse. `MmDeleteKernelStack(StackBase, StackLimit)` frees exactly such a
  stack (any other pair is a guest fault, a bugcheck on the console).
- `MmIsAddressValid`: TRUE for readable committed memory and for the kernel
  objects R-comp serves virtually (live thread Bodies, registered
  variable/type tokens).
- `MmMapIoSpace(Unknown, PhysicalAddress, Size, Protect)`: an address already
  in a physical window is returned as is (Xenia's observed XMA use, arguments 2
  / 0x40 / 0x404), a physical address becomes its 0xA0000000-window address,
  when committed. Device registers (MMIO) are not mapped: explicit fatal.
- `KeSetCurrentStackPointers(StackPointer, Thread, StackAllocBase, StackBase,
  StackLimit)`: for the calling thread, the PCR publishes StackBase (+0x70) /
  StackLimit (+0x74) and the KTHREAD StackAllocBase/StackBase/StackLimit
  (+0xD0/+0x5C/+0x60). The range must be committed read/write memory
  containing StackPointer. The guest then continues at LR with r1 =
  StackPointer: with StackPointer == r1 that is the caller's own return;
  otherwise it is a guest fiber switch, served by switching host contexts
  (`THREAD_OBJECTS.md` "Guest fibers"). A continuation that is neither a
  suspended fiber nor a recompiled function entry traps.
- `KeQueryBasePriorityThread`: the increment recorded by
  `KeSetBasePriorityThread` and the `ExCreateThread` hints.
- `KeSetCurrentProcessType(1)`: the title already runs in the user process;
  0 (idle) / 2 (system) trap, other values are guest misuse.

## Modules

`XexLoadImage(Name, Flags, MinimumVersion, PHMODULE)`: R-comp's images are the
main module and the HLE modules `xboxkrnl.exe` / `xam.xex`. Their names or
paths add one LDR LoadCount reference and return the real HMODULE. A device
path that does not exist returns the open's NTSTATUS
(`STATUS_OBJECT_NAME_NOT_FOUND`, `STATUS_OBJECT_PATH_NOT_FOUND`); a file that
exists is loaded when the title registered an AOT descriptor for it (title
DLLs, `MODULES.md` "AOT secondary modules", added the same day) and otherwise
traps (its code would have to be recompiled into the title); a bare name
that is no loaded module returns `STATUS_NO_SUCH_FILE` (Xenia). A non-zero
minimum version is accepted for a system module only up to the presented kernel
version; other cases trap. `XexUnloadImage` releases one reference
(`STATUS_INVALID_HANDLE` for an unknown handle); releasing the bootstrap
reference traps.

`RtlImageNtHeader`: NT's `RtlImageNtHeaderEx` rules (NULL base, no `MZ`,
`e_lfanew` >= 256 MiB, no `PE\0\0` -> NULL); an unreadable base is a guest
fault.

## Video

`VdSetDisplayModeOverride(ULONG, ULONG, double RefreshRate, ULONG, ULONG)`
(Xenia/rexglue; the integers after the double read from their positional
slots r6/r7): R-comp never overrides the scan-out mode, so the all-zero request
(no override) describes the state that holds and returns 0; a real override
traps with its values.

## Kernel version policy

See `KERNEL_FIELDS.md`: a title whose import `version` is newer than the final
retail kernel 2.0.17559 is accepted when its declared `minimum` is a retail
kernel (2.0.1888 to 2.0.17559) and is presented 2.0.17559.0.

## Reproduction

```
MSYS_NO_PATHCONV=1 /c/path/to/rcomp/build/vulkan-gta-radv-20260928/host-cygwin-full/bin/bash.exe -lc \
  "cmake -S /cygdrive/c/path/to/rcomp/runtime -B /cygdrive/c/path/to/rcomp/build/rt-agentB -G Ninja \
   -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DRCOMP_ROOT=/cygdrive/c/path/to/rcomp && \
   cd /cygdrive/c/path/to/rcomp/build/rt-agentB && ninja && ctest -j1 --output-on-failure -R rt_kernel"
```

Tests: `rt_kernel_seh`, `rt_kernel_xex`, `rt_kernel_dispatch`, `rt_kernel_dpc`,
`rt_kernel_more`, `rt_kernel_iocp` (file completion ports), and the updated
`rt_kernel_fields`, `rt_kernel_devices`.
