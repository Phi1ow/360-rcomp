# Synchronization objects — 2026-09-29

Status: **PASS (HOST ONLY)**. PS5 execution: **NOT TESTED** by this runtime lot.
The implementation is local C++ over the existing dispatcher mutex, condition
variable, checked guest memory and handle table. No external kernel implementation
or generated PPC source was copied or changed.

## Supported contract

Eight Xbox exports are added in `src/hle_xboxkrnl_threads.cpp`:

| Export | Ordinal | Behavior |
|---|---|---|
| KeInitializeSemaphore | 0x74 | Guest 20-byte object, type 5, signed count at +4 and limit at +16 |
| KeReleaseSemaphore | 0x88 | Positive adjustment, previous count, no change on invalid/overflowing release |
| NtCreateSemaphore | 0xD5 | Unnamed handle, initial count >= 0, limit > 0, initial <= limit |
| NtReleaseSemaphore | 0xF3 | Positive release, optional previous count, limit checked without overflow |
| NtCreateMutant | 0xD4 | Unnamed recursive mutant, optional initial ownership |
| NtReleaseMutant | 0xF2 | Owner-only release; second Xbox argument must be zero |
| KeWaitForMultipleObjects | 0xAF | Guest events and semaphores, WaitAll=0 / WaitAny=1 |
| NtWaitForMultipleObjectsEx | 0xFE | Handle events, semaphores, mutants and exited threads |

The single-object waits use the same dispatcher. Multiple waits accept 1–64
distinct objects; invalid count/type/duplicates return INVALID_PARAMETER.
All handles are retained and all input pointers validated before state changes.
Wrong handle types and stale handles return errors. A bad semaphore output
pointer cannot create an object, release tokens or overwrite the previous count.
Access checks use `GuestMemory::is_accessible`: outputs require ReadWrite, while
object attributes, wait arrays and timeouts require Read. Read-only and guard
pages remain committed and translatable, so commitment alone is insufficient.
Ke waits on manual-reset events require only Read; state-consuming event and
semaphore waits, initialization and release require ReadWrite. Rejected NT
pointers return ACCESS_VIOLATION; rejected Ke accesses diagnose `guest_access`
before acquiring the dispatcher lock. Mapping changes must remain serialized
by their owner: an access query does not pin pages against concurrent remapping.

Readiness and consumption happen under one dispatcher lock. WaitAll first tests
the complete set, then consumes it; timeout or shutdown consumes nothing.
WaitAny consumes only the lowest ready index. Auto-reset events clear once,
manual-reset events and exited threads remain signaled, and each acquired
semaphore loses one token. Guest semaphore signal changes use big-endian atomics.

Mutant ownership follows `GuestThread::thread_id`, never a recycled stack/PCR
address or native thread id. Recursive waits increase depth; only the owner can
release. Normal guest return and ExTerminateThread abandon all still-owned
mutants. The next acquisition obtains ownership and reports 0x80 + index for
WaitAny, 0x80 for WaitAll; later recursive waits no longer report abandonment.
Weak registry entries are pruned and cleared between runtime lifetimes.

Relative/absolute deadlines and INT64_MIN use the existing checked conversion
and bounded native waits. Shutdown wakes both single and multiple waits with
STATUS_THREAD_IS_TERMINATING and waits for workers before freeing memory.
Alertable waits had an empty APC/alert queue when this was written; since
3 October 2026 `NtQueueApcThread` produces user APCs, delivered by user-mode
alertable waits only (see Later additions). No alert producer exists, so
`STATUS_ALERTED` is never returned.

## Later additions

User APC delivery in alertable user-mode waits (`NtQueueApcThread`),
`NtSignalAndWaitForSingleObjectEx` and I/O completion ports (a new waitable
handle kind) are described in `KERNEL_IMPORTS_20261003.md`.

## Explicit limits

- Named objects, kernel-mode APCs, guest Ke mutant layouts, timers, queues and file
  waits are outside this tranche. Unsupported imports/types remain errors or
  fatal diagnostics; they do not report completion.
- Guest exception dispatch is absent. Invalid Ke semaphore parameters, limit
  overflow and extreme mutant recursion end in `unimplemented` without signal
  mutation, rather than pretending to raise and handle an exception.
- Scheduler priority and IRQL are not emulated. KeReleaseSemaphore's Increment
  and Wait values are scheduling hints here, as for existing KeSetEvent.
- Ke wait blocks and dispatcher wait lists are not guest-visible scheduler
  structures. The host stack owns bounded wait bookkeeping; callers must not
  inspect a synthetic guest wait queue.
- As before, absolute deadlines are converted at entry; host wall-clock changes
  during a pending wait are not tracked. Fairness and priority ordering are not
  promised. This is not evidence that GTA IV or its full import set runs.

## References consulted

- Xbox signatures, ordinals and semaphore layout:
  [Xenia xboxkrnl_threading.cc at 95a5c3e](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc).
- Wait type and abandoned result mapping:
  [Xenia xobject.cc at the same pin](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/kernel/xobject.cc).
- Dispatcher atomicity, return values and semaphore limits:
  [Microsoft WaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects),
  [KeWaitForMultipleObjects](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kewaitformultipleobjects),
  [KeReleaseSemaphore](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kereleasesemaphore).

## Reproduction

From repository root in the existing local Cygwin shell, `PATH=/usr/bin:/bin`:

```sh
cmake -S runtime -B build/runtime-sync2-20260929 -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DRCOMP_XENONRECOMP="$PWD/build/cpu-xenonrecomp-host-src" \
  '-DCMAKE_CXX_FLAGS=-fsanitize=undefined -fsanitize-trap=undefined'
cmake --build build/runtime-sync2-20260929 -j8
ctest --test-dir build/runtime-sync2-20260929 --output-on-failure
ctest --test-dir build/runtime-sync2-20260929 \
  -R '^rt_(sync_objects|lifetime|video_lifetime)$' \
  --repeat until-fail:10 --output-on-failure --timeout 30
```

Clang 22.1.8 / CMake 4.4.3 / Ninja 1.13.2. All commands return 0. The baseline
plus new suite passes 17/17; concurrency/lifetime suites pass ten repetitions
each. The new tests exercise production thunks, without replacing HLE behavior.
`TESTDOUBLE_` functions stand in only for guest entrypoints in this unit harness.

Logs live under `build/runtime-sync2-20260929/{config,build,test,repeat}.log`.
Independent compiled mutations under its `mutations/` directory lose semaphore
consumption, consume WaitAll partially, or lose mutant abandonment. Each mutant
build succeeds and the real test exits 1; `mutations/RESULTS.json` records exact
commands and returns. No mutation edits the production source or dependency.

## Protected memory correction after independent review

Agent 6 reproduced SIGSEGV (exit 99) for protected outputs to NtCreateSemaphore,
NtCreateMutant and NtReleaseSemaphore, plus a guard-page creation. The first
implementation used commitment-only `translate`; it could insert an object or
hold the dispatcher mutex before faulting. The correction uses PRIME/Agent 2's
access query, with the checks described above; no OS protection query is used
inside the runtime.

The extended `rt_sync_objects` suite maps genuine Read/None pages and verifies
NT statuses, unchanged output bytes, no leaked handles, no released/consumed
tokens on failure, readable inputs, page-crossing rejection, and usable locks
after Ke diagnostics. New clean UBSan build:
`build/runtime-sync-protection-20260929`, configured with the same command and
flags above after substituting this output directory. Configuration/build and
17/17 tests return 0; 30 concurrency/lifetime executions return 0. Logs:
`config.log`, `build.log`, `test.log`, `repeat.log` in that directory.

The independent original `build/tests-sync-review-20260929/repro_bad_output.cpp`
was recompiled without editing it. All four modes (`create`, `release`,
`mutant`, `guard`) return ACCESS_VIOLATION and exit 0. Commands/return codes
are preserved in `build/runtime-sync-protection-20260929/REPRO_RESULTS.json`;
individual output is in `repro-*.log`.

The earlier packaged PS5 boot result (27 checks, binary hash prefix `8a0b`)
belongs to its recorded binary. The 19:51 post-freeze source change was checked
by Agent 2 as producing identical machine code. The protection fix is a later
behavioral change and requires a new build/console receipt; that earlier boot
result is not validation of this correction.
