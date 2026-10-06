# Catalog-driven imports — 2 October 2026

Status: **PASS (HOST ONLY)**. PS5 execution: **NOT TESTED**. These exports were
chosen from the disc catalog (`docs/COMPATIBILITY.md`): batch 1 holds imports
that every catalogued title lacks; batch 3 holds well-specified kernel imports
of the Unreal Engine 3 title. Each states its Xbox 360 semantics and its
subset; everything outside the subset traps (`unimplemented`, `guest_access`
or `guest_trap`), never a fake success. No Xenia/ReXGlue code is copied: the
pinned Xenia sources (95a5c3e) were read for signatures, ordinals and layouts.

## Batch 1 (all three catalogued titles)

### NT waitable timers — `src/hle_xboxkrnl_threads.cpp`

| Export | Ordinal | Subset |
| --- | --- | --- |
| NtCreateTimer | 0x00D7 | `(PHANDLE, POBJECT_ATTRIBUTES, TIMER_TYPE)`; unnamed NotificationTimer (0) and SynchronizationTimer (1) |
| NtSetTimerEx | 0x00FA | `(HANDLE, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE, KPROCESSOR_MODE, PVOID ApcContext, BOOLEAN Resume, LONG Period ms, PBOOLEAN PreviousState)`; no APC routine, no resume, Period >= 0 |
| NtCancelTimer | 0x00CD | `(HANDLE, PBOOLEAN CurrentState)` |

A timer is a new handle kind (`HandleKind::Timer`) in the existing dispatcher;
`NtWaitForSingleObjectEx` and `NtWaitForMultipleObjectsEx` accept it. State
rules are NT's, observed on Windows ntdll by `runtime/tests/timer_windows_oracle.py`
(`runtime/tests/timer_windows_oracle.json`, an NT reference, not Xbox proof): a new
timer is not signalled; setting resets it to not signalled, arms it and
reports the previous state; an expiry signals it; a satisfied wait resets a
SynchronizationTimer only; a periodic timer re-arms, and expiries missed while
nobody looked coalesce into one signal; cancelling disarms without changing
the state and reports it. DueTime is negative-relative or absolute system time
(a past time expires at once), converted at the call like every other wait.

No host thread runs per timer. Whoever looks at a timer under the dispatcher
lock applies the expiry that is due; a wait that includes armed timers sleeps
no later than their earliest due time (`WakeHint` in `wait_until`), and
setting or cancelling a timer wakes its waiters so they recompute that time.
Waits without timers are unchanged.

Traps: APC routines (the runtime delivers no APC), Resume, negative Period and
timer types other than 0/1 — NT rejects the last two with
`STATUS_INVALID_PARAMETER_n`, whose index the Xbox signatures shift, so the
code is not guessed. Named timers trap like every named object here.

### Title loader — `src/hle_xam_loader.cpp`

| Export | Ordinal | Subset |
| --- | --- | --- |
| XamLoaderLaunchTitle | 0x01A4 | `(LPCSTR Name, DWORD Flags)`; Name NULL, the return to the dashboard |
| XamShowDirtyDiscErrorUI | 0x02D9 | `(DWORD UserIndex)` |

`XamLoaderLaunchTitle(NULL, …)` ends the title through the lifecycle of
`XamLoaderTerminateTitle` (termination callbacks, then the exit of the calling
guest thread) and records the new reason `XamLoaderLaunchDashboard`. Launching
another executable needs a second recompiled image in the title and traps
(`unimplemented`). `XamShowDirtyDiscErrorUI` is the console's disc-read error
screen, which never returns; it ends the title with a `guest_trap` diagnostic
naming that report, distinct from a missing import.

## Batch 3 (Unreal Engine 3 title)

| Export | Ordinal | File | Subset |
| --- | --- | --- | --- |
| _snprintf | 0x013A | `src/hle_xboxkrnl_format.cpp` | the formatter's domain; variable arguments from r6, then SP+0x50; `_vsnprintf`'s count and truncation rules |
| ExAllocatePool | 0x0009 | `src/hle_xboxkrnl_pool_rtl.cpp` | the default pool (selector 0) with the tag `None` |
| DbgBreakPoint | 0x0001 | `src/hle_xboxkrnl_format.cpp` | no debugger: an unhandled breakpoint ends the title (`guest_trap`) |
| InterlockedPopEntrySList | 0x002C | `src/hle_xboxkrnl_threads.cpp` | 8-aligned SLIST_HEADER; depth − 1, sequence kept |
| InterlockedFlushSList | 0x002B | `src/hle_xboxkrnl_threads.cpp` | header becomes zero; returns the detached chain |

SLIST_HEADER is `{u32 first, u16 depth, u16 sequence}` big-endian. Titles
inline the push with `ldarx/stdcx.` on the whole header, so both HLE calls use
a 64-bit compare-and-swap (or exchange) on the same 8 bytes and never take the
dispatcher lock. A misaligned header or an unreadable first entry is a
`guest_access` diagnostic.

## Host evidence

- `rt_timers`: every oracle row, the wake hint (a blocked wait returns at the
  due time, within a multiple wait and after a re-set), errors and traps;
  stable over 5 repetitions.
- `rt_process_lifecycle`: dashboard return (callbacks run, new reason), the
  launch of another executable and the dirty-disc screen as diagnostics.
- `rt_slist`: LIFO order, header fields, flush, 2,000 concurrent guest-style
  pushes against three popping threads with no loss or duplicate, traps.
- `rt_format`, `rt_pool_rtl`: `_snprintf` on every existing format case and
  the truncation capacities; `ExAllocatePool` tag and release.
- Whole runtime suite: 43/43 PASS, run serially (the parallel `ctest` of this
  Cygwin host stalls after tests that already printed PASS).

Not done here and still unresolved: `NtPulseEvent` (releasing only the threads
waiting at the time of the pulse needs a change of the dispatcher's wait
model), `KeEnableFpuExceptions` (the default FP exception state is not
established), and the content, session, profile, voice, task, Live QoS and
console-key services the catalog lists.
