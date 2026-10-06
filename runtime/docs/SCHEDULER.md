# Scheduler controls, IRQL and spin locks

Registered by `runtime/src/hle_xboxkrnl_sched.cpp` (plus `KeResumeThread` in
`hle_xboxkrnl_threads.cpp`). Found by the actual GTA IV host entry trace, which
stopped first on `KeSetBasePriorityThread` (caller `0x829A0A4C`, Body of the
current thread, increment 2).

## Thread scheduling hints

`KeSetBasePriorityThread` (0x99), `KeSetAffinityThread` (0x97) and
`KeSetDisableBoostThread` (0x9C) take a thread Body token (only real, live
Bodies; unknown/stale pointers stop with a guest-access diagnostic) and store
the value in that thread's `ThreadObjectIdentity`, returning the previous
value. Defaults: increment 0, boost enabled, affinity `0x3F` (the six Xbox 360
hardware threads). An affinity of zero or with bits outside `0x3F` is rejected.

**Scope:** the values are per-thread state observable through these APIs.
R-comp does not reconfigure host thread priority or CPU affinity from them;
this is a scheduling hint, not a timing guarantee.

`KeResumeThread` (0x92) decrements a worker's suspend count and returns the
previous count (0 for the main thread, which is never suspended).

## IRQL and spin locks

`GuestThread::irql` is real per-thread state (0 = PASSIVE_LEVEL).
`KfAcquireSpinLock` raises to at least DISPATCH_LEVEL (2), acquires and returns
the old level; `KfReleaseSpinLock(lock, old)` releases and lowers;
`KeRaiseIrqlToDpcLevel`, `KfLowerIrql`, `KeAcquireSpinLockAtRaisedIrql`,
`KeReleaseSpinLockFromRaisedIrql` and `KeTryToAcquireSpinLockAtRaisedIrql`
follow the same model. The lock word (guest big-endian u32; 0 = free) holds
the owner's thread Body while held and is acquired with an atomic CAS.

There is no interrupt or DPC delivery, so IRQL raises have no scheduling
effect. Misuse stops with a diagnostic rather than continuing: lowering to a
level above the current one, releasing a lock the caller does not hold,
re-acquiring a lock the caller already holds (self-deadlock), unwritable or
misaligned lock words.

## ExCreateThread creation flags

The actual GTA IV entry called `ExCreateThread` with flags `0x10000001`.
Accepted: bit 0 (suspended), the priority hint bits `0x60` (recorded as
increment 1 when `0x20` is set, else 0) and the top byte as a hardware-thread
mask (bits 0..5; recorded as the thread's affinity; layout per the public
rexglue-sdk c94f5eb `xthread.cpp`). Any other bit still stops with an explicit
unimplemented diagnostic. The values are hints; the host scheduler is not
reconfigured.

## RtlRaiseException (0x136)

Structured exception dispatch (unwind tables, `__C_specific_handler`,
`RtlUnwind`, `RtlCaptureContext`) is **not implemented**. Two exception classes
have a defined effect without a debugger:

* code `0x406D1388` (thread naming): the name is logged
  (`RCOMP-THREADNAME`) and the call returns;
* floating-point codes `0xC000008D..0xC0000093`, raised by the C runtime's math
  routines: reported once per occurrence (first 16, `RCOMP-EXCEPTION float ...`)
  and ignored, so the raising routine continues with its own default result.
  This matches the effective behaviour of the public rexglue-sdk/Xenia kernels
  (`RtlRaiseException` has no effect there); on a console a handler could
  alter the result.

Any other code stops with the full exception record, parameter words and the
guest call chain.
