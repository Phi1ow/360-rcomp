# Xbox process and title lifecycle

This tranche implements the early title-process imports needed by the current
R-comp boot path without adding a guest kernel, a host reboot path, or a
fabricated `KPROCESS`/`KTHREAD` memory layout.

## Pinned public ABI evidence

The implementation is pinned to public source revisions so later changes can
be reviewed against a concrete contract:

- `rexglue/rexglue-sdk` `include/rex/system/xexception.h`, commit
  `70483a42518312d3b66b154ee2678e8387da41c9`: title-termination registration is
  16 bytes: big-endian `notification_routine` at `+0`, `priority` at `+4`, then
  an 8-byte list entry.
- `cOzInABox/xkelib` `kernel/_kernelext.h`, commit
  `3e218d66f8886cae374e15655fed61115bfd8581`: public declaration
  `ExRegisterTitleTerminateNotification(reg, create)` and the same structure.
  Its notes identify `0x7C800000` as an early XAM priority and `0` as late.
- `cOzInABox/dashLaunch` `swap_dll/swap.cpp`, commit
  `62256313e04b6263009a0696ba0b533597bcad5f`, and `copeison/xbdm`
  `xbdm/dmbreak.c`, commit `0716a030207d24aca8a7735e1daf13244c192959`:
  deployed public callers use a no-argument `void` title-termination callback.
- `rexglue/rexglue-sdk` `include/rex/system/kernel_state.h`, commit
  `9b9cf9eb98537111d3a1034b47f4f18ecffaa2f8`, and
  `src/system/kernel_state.cpp`, commit
  `697399cdd4207c779610127c01e5285e580767de`: process types are idle `0`, user
  `1`, system `2`, and the title process is initialized as user. The same source
  demonstrates cooperative title-thread cancellation while guest memory is
  still alive.
- Xenia `src/xenia/kernel/xam/xam_info.cc`, public source: the
  `XamLoaderTerminateTitle` export has no arguments and is a non-returning title
  termination request.
- Public `FIRMWARE_REENTRY` declarations in Xenia/XeniOS and XexUtils enumerate
  `HalRebootRoutine` as value `1`. Existing emulator implementations only
  establish value `1` as the ordinary reboot/title-return path; R-comp maps
  that value to its virtual dashboard/title lifecycle. Values `0` and `2..8`
  are rejected as unsupported instead of controlling the PS5 host.

The source URLs are the normal GitHub paths under those repositories. No game,
SDK, firmware, or proprietary header bytes are used by this implementation.

## Process type

`KeGetCurrentProcessType` returns `1` only when called by a real R-comp
`GuestThread` created from the active `Runtime` title heap. A call without an
active title guest context is an internal diagnostic. R-comp does not pretend
that an arbitrary host thread is an Xbox system or idle process.

## Termination notifications

`ExRegisterTitleTerminateNotification` requires a 4-byte-aligned, 16-byte
read/write guest registration and an AOT callback present in `func_table`.
Registrations are keyed by the address of the embedded registration object.
Duplicate insertion and removal of an unregistered object are guest traps.

Callbacks are dispatched once, in descending unsigned priority and then stable
registration order. The embedded guest `LIST_ENTRY` is not populated: its
links would require invented guest kernel addresses. R-comp keeps list
bookkeeping host-side and validates the real guest routine/priority fields.

The callback is invoked with the terminating guest's `PPCContext` and guest
memory base through `lookup_function`. No lock or C++ iterator remains live
across callback execution. Each callback is marked consumed before dispatch so
a guest non-local exit cannot cause it to run twice.

## Two-phase title shutdown

Title termination must wake managed waits and stop new worker admission while
the runtime and guest memory still exist. The current frozen threading code has
one blocking `runtime_quiesce_threads()` call that deliberately rejects calls
from a running guest thread. Integration therefore needs this split:

```text
runtime_request_thread_quiesce()
    under dispatcher lock: stopping = true; signal every managed waiter
    returns immediately and is valid from a GuestThread

runtime_finish_thread_quiesce()
    host/owner only: wait until managed worker count reaches zero

runtime_quiesce_threads()
    request + finish (preserves current shutdown behavior)
```

Until the frozen threading file is released, the isolated lifecycle module
accepts a `TitleQuiesceRequestFn` host hook. PRIME should wire it directly to
`runtime_request_thread_quiesce`. `XamLoaderTerminateTitle` and supported
`HalReturnToFirmware` set the lifecycle state, invoke that non-blocking request,
run callbacks while memory is alive, return from the lifecycle helper so every
RAII object is gone, then call `exit_current_guest_thread`. The owner performs
the finish phase before GPU/runtime memory teardown. This request point is also
the intended future network-cancellation fan-out boundary.

State is scoped by `Runtime::generation`. A new runtime generation discards
registrations, callback-dispatch residue, critical-region counts, and the prior
terminal code before accepting title work.

## Critical regions

Public Xenia/rexglue thread behavior models `KeEnterCriticalRegion` by
decrementing a per-thread APC-disable count and `KeLeaveCriticalRegion` by
incrementing it toward zero. R-comp stores that count host-side per real
`GuestThread`; zero is enabled, nested enters produce `-1`, `-2`, and so on.
An unmatched leave and signed overflow are guest traps. R-comp currently has no
kernel APC queue, so returning to zero has no deferred APC delivery to perform.

## Explicit limits

`RtlRaiseException` and `__C_specific_handler` are outside this tranche and
remain unresolved. Firmware modes other than `HalRebootRoutine (1)` remain
explicitly unsupported. No lifecycle path invokes a host reboot, shutdown,
SMC action, or PS5 power API.
