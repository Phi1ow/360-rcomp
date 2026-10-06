# Thread object identity and references

Runtime tranche for GTA IV boot work, 28 September 2026. This file documents
only the object semantics actually published by R-comp. It does not claim a
complete Xbox 360 `OBJECT_HEADER`, `OBJECT_TYPE`, or `KTHREAD` layout.

## Evidence boundary

`docs/OBJECT_ABI_RESEARCH_20260928.md` pins the public Xenia/rexglue, nxdk,
XexUtils and Microsoft ownership references. The generated GTA IV inventory was
then inspected by the CPU worker. Its three actual `ExThreadObjectType` readers
load the imported BE32 pointer and pass that value unchanged as argument 2 to
`ObReferenceObjectByHandle`; no descriptor field dereference was found. This is
enough to implement **type identity**, and no more.

The title bootstrap registers `ExThreadObjectType` (`0x001B`) to a guest address
inside the fixed opaque runtime arena `0x70000000..0x70FFFFFF`. The arena is
reserved but never committed. Equality against that pointer works. Any
guest attempt to read descriptor fields faults, instead of observing a zeroed
or guessed callback/tag structure.

## Thread Body identity

Every `GuestThread` created from the process Runtime heap, including the main
entry thread and `ExCreateThread` workers, receives one `ThreadObjectIdentity`.
Its Body value is a unique 16-byte-aligned token minted monotonically from the
same inaccessible arena. It is a stable guest pointer token for this Runtime generation, not a readable
KTHREAD. Stack/PCR/TLS storage has its existing lifetime and can disappear when
execution ends without invalidating a referenced Body token.

Three ownership classes are kept separate: active `GuestThread`/worker
ownership, numeric handle ownership, and explicit guest Body references. Handle
close invalidates that numeric handle immediately. An already acquired Body
reference continues to own the identity until the matching dereference. Runtime
shutdown invalidates all Body tokens while guest memory is still alive; leaked
guest references cannot survive into the next title generation. Object teardown
is performed during `runtime_shutdown`, after managed-thread quiescence and after
the title owner has stopped external guest-code producers; quiescence alone does
not invalidate Body identities.

Because a Body is a raw guest pointer with no generation bits, tokens are never
reused during a Runtime lifetime. No token commits guest memory or consumes a
GuestHeap allocation. This monotonic-token policy is separate from
the handle table's own generation-based stale-handle detection.

## Supported exports

`ObReferenceObjectByHandle` (`0x0110`) supports thread handles and the corroborated
current-thread pseudo-handle `0xFFFFFFFE`. Output must be a writable aligned word
and is validated before reference mutation. A nonzero type argument must equal
the exact opaque `ExThreadObjectType` token. Success writes the Body and acquires
one explicit Body reference. Other handle kinds with no R-comp guest Body remain
unimplemented instead of receiving fabricated object storage.

`ObReferenceObject` (`0x010F`) acquires another reference to a registered thread
Body and leaves r3 unchanged, treating the operation as VOID as in the independent
NT ownership reference. GTA IV's observed call supplies a Body and does not
consume a return value. Unknown/stale pointers fault rather than becoming objects.

`ObDereferenceObject` (`0x0105`) releases one explicit Body reference and also
leaves r3 unchanged. Unknown/stale Body values and underflow are guest-access
fatal diagnostics; no silent success path exists.

`NtDuplicateObject` (`0x00DA`) supports the corroborated minimal subset:
ordinary live source handle, writable non-null target pointer, options `0`.
It creates a distinct generation-checked handle to the exact same host object.
Closing source and target is independent. Output validation precedes the table
transaction, and allocation failure publishes no target. `DUPLICATE_CLOSE_SOURCE`,
null output and pseudo-handle duplication remain explicitly unimplemented until
their Xbox 360 failure/side-effect ordering is independently bounded.

`ObLookupAnyThreadByThreadId` (`0x010A`) adds one Body reference to the thread
object with that id (also an exited thread whose object is still owned) and
returns its Body; an unknown id is `STATUS_INVALID_PARAMETER` (NT
`PsLookupThreadByThreadId`). `ObOpenObjectByPointer` (`0x010E`, two-argument
Xbox form) opens a new ordinary thread handle to a worker thread Body; a Body
without a handle object (bootstrap thread, interrupt/DPC contexts) and device
objects trap. `ExEventObjectType` (`0x000E`) is registered with
`ExThreadObjectType` by `register_thread_object_type_variable()` as an
identity-only token (`0x70000400`): as a type filter it makes a non-event
handle `STATUS_OBJECT_TYPE_MISMATCH` and traps for an event handle (events have
no guest KEVENT Body). Details: `KERNEL_IMPORTS_20261003.md`.

The process pseudo-handle `0xFFFFFFFF` is not treated as a thread. `ObIsTitleObject`
and `ObReferenceObjectByName` remain unresolved.

## Direct layout consumers

`PCR+0x100` publishes the Body (`guest_thread.h`). The published direct KTHREAD
fields are listed in `KERNEL_FIELDS.md` (+0x58, +0x5C, +0x60, +0x84, +0xD0,
+0x14C, +0x160, +0x164). There is no OBJECT_HEADER before Body, and no
suspension, priority or timestamp field. Services that take a thread Body
resolve the opaque identity through the host registry. A future direct guest
field requires its own corroborated ABI evidence and test; it must not be
inferred from the fact that the Body pointer exists.

## Guest fibers

Status: **PASS (HOST ONLY)** (`rt_fibers`, `rt_kernel_more`; the System V
switch routine also at -O2 on the host, `RCOMP_HOST_FIBER_FORCE_SYSV`). PS5
execution: **NOT TESTED**.

Xbox 360 fibers are not a kernel service: xapi's ConvertThreadToFiber /
CreateFiber / SwitchToFiber / DeleteFiber are linked into the title and become
ordinary recompiled functions. They keep the current fiber block at
`KTHREAD+0x164` and the stack fields at +0x5C/+0x60/+0xD0 (`KERNEL_FIELDS.md`).
SwitchToFiber saves r1, r14-r31, CR, LR, FPSCR, f14-f31 and v64-v127 into the
running fiber's block, loads the same set from the target's block, stores the
target at `KTHREAD+0x164` and ends with a tail call to the kernel export
`KeSetCurrentStackPointers(StackPointer, Thread, StackAllocBase, StackBase,
StackLimit)`, which sets r1 and returns to LR. CreateFiber writes an initial
block whose LR is xapi's start trampoline, r31 the start routine and r1 the top
of a stack from `MmCreateKernelStack`.

In recompiled C++, "return to LR" is a host `return`: it must run on the host
stack holding the target fiber's frames. R-comp therefore gives every guest
fiber a host context (`src/host_fiber.h`, `src/host_fiber.cpp`) and the switch
happens inside `KeSetCurrentStackPointers`, the one kernel entry every xapi
SwitchToFiber reaches. No title address, function signature or generated-code
change is involved; any title that links xapi's fibers is covered.

- `StackPointer == r1`: switch to the running fiber; the call returns normally.
- A suspended host context whose saved guest r1 equals StackPointer: it is
  resumed (its pending `KeSetCurrentStackPointers` call returns). The key is
  the r1 of the context when it suspended, which is the value SwitchToFiber
  saved in its block and the switch back passes in. Guest stacks are disjoint,
  so a key names one context.
- Otherwise LR must be a recompiled function entry (CreateFiber's
  trampoline): a new host context calls it with the loaded registers. Neither:
  `RCOMP_FATAL_UNIMPLEMENTED` with StackPointer, LR and r1. A fiber's first
  function returning (no caller exists; xapi bugchecks first) is also an
  explicit fatal.
- Each host context has its own PPCContext; the whole register file is copied
  into the target's at the switch. A fiber suspended by one guest thread may
  therefore be resumed by another (it then sees that thread's r13, KTHREAD and
  PCR). A thread's own original context cannot be resumed by another thread
  (explicit fatal).
- `MmDeleteKernelStack` releases the host contexts suspended on that stack
  (DeleteFiber of a suspended fiber); deleting the stack the caller runs on is
  a guest-access fatal.
- `ExTerminateThread` on a created fiber first switches back to the thread's
  own context, which releases the fiber's host context and ends the thread as
  usual. The fiber's guest stack stays allocated, as on the console.
- Runtime shutdown releases every remaining host context.

Host contexts: a minimal x86-64 switch (System V on the PS5: rbx, rbp,
r12-r15, MXCSR, x87 control word; Microsoft x64 for the Cygwin host tests: also
rdi, rsi, xmm6-xmm15). The PS5 SDK exports getcontext/swapcontext stubs, but
FreeBSD's swapcontext is a system call per switch with signal-mask semantics,
and the switch must stay inside the R-comp archive. Host stacks: anonymous
mappings of 8x the guest stack size (at least 512 KiB, at most 8 MiB, the host
stack of a guest thread) plus a 64 KiB `PROT_NONE` guard. The KTHREAD
stack/PCR fields, the thread identity, `current_guest_thread()` and
`exit_current_guest_thread()` keep working, because a fiber runs on the host
thread of the guest thread that switched to it.

Limits (explicit fatals or documented): a switch from a nested guest callback
whose register file is not the running fiber's (an APC routine calling
SwitchToFiber) traps; the diagnostic import attribution of the PC sampler
(`t_current_import`) can be restored on the wrong host thread after a fiber
moved between threads.
