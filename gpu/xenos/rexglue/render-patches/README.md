# Render correctness, diagnostics and lifecycle patches

These are generic Xenos/backend contracts and bounded observations. They do not
change guest zfunc, clip planes, formats or shaders to fit one title. Source
preparation and host tests are not proof of GPU fragments.

## Shared-memory compute writes

`shared-memory-compute-write-access.patch` corrects GetUsageMasks(kComputeWrite)
from SHADER_READ alone to SHADER_READ|SHADER_WRITE. Compute resolves actually
write the shared buffer, so subsequent barriers must include writes in their
access scopes. Existing stages, ranges and order remain. Previous policy:
FAIL; corrected actual-function policy, SDK syntax and application checks:
PASS. It has been active since phase8. It did not correct the y255/256 missing
geometry observed in1x intro/game captures; no pixel or FPS attribution is made.

`adapt-render-correctness.py` provides checked/idempotent migration in copied
R-comp build trees only, preserving LF/CRLF and recording before/after hashes.
The latest source recipe applies the exact reviewed patch to a fresh pinned tree.

## Quiet source preparation

`quiet-cp-instrumentation.patch` reproduces the measured gated CP/pipeline
instrumentation and weak CP profiler registration directly from public base0011.
It was reconstructed against the recorded phase2 source hashes. This avoids
requiring the earlier manual exploratory build edits as an untracked prerequisite.
`retained-resolve-trace.patch` preserves the existing legacy resolve counter and
first 80/every 2000 trace, matching the measured source. It is not compiled out by
the bounded-diagnostics option; the latest profile does not silently remove it.

Diagnostic and timing options default OFF. Compilation OFF removes their hot
packet/draw clocks, signature mutex and gated per-packet logs. The historical
`adapt-perf-instrumentation.py` checks the old exploratory layout; the latest
recipe uses the complete exact delta instead.

## Bounded structural state diagnostics

`bounded-render-diagnostics.patch` adds state observations to three private GPU
units. `structural-render-diagnostics.patch` is the subsequent delta and must
follow it exactly. The current `shim/rcomp_xenos/render_diagnostics.h` is already
integrated in the repository. Historical helper/fast-gate patches document
intermediate layouts and are not additional latest-profile application steps.

`RCOMP_XENOS_RENDER_DIAGNOSTICS` defaults OFF. When built ON, configure these
requires-restart cvars before GPU startup:

```cpp
rex::cvar::SetFlagByName("rcomp_render_diagnostics", "true");
rex::cvar::SetFlagByName("rcomp_render_diag_start_frame", "7000");
rex::cvar::SetFlagByName("rcomp_render_diag_frames", "8");
```

The frame is an example chosen from observed command-stream progress, not a
per-title workaround. The window is bounded to 32 CP frames. Fixed exact-state
tables cap each category, and explicit LIMIT lines disclose truncation. Source
settings are cached before the window; normal diagnostics-OFF code has no calls.

DRAW/DEPTH/PLANE report guest registers and normalized viewport/scissor/depth,
formats, clip state, offsets and structural shader modes. TARGETS report actual
framebuffer extent, attachment keys, sample counts and ownership work.
RESOLVE/DUMP/TRANSFER preserve exact EDRAM spans, formats and rectangles;
SHARED-BARRIER reports access scopes and ranges. A logged command or rectangle
is not proof that fragments reached an attachment.

Phase9 and13 observations at 1x contained the three 256/256/208-line resolve
tiles and still showed missing geometry above y256. These did not establish a
cause or justify arbitrary depth/clip changes. Phase18's native R-comp depth/
MSAA transfer probe was PASS for all ten checkpoints; no RADV defect was
established. Real 3x gameplay captures from phase19 onward were complete 4K and
PASS. Underlying1x causality and inactive real-draw GPU checkpoints remain
NOT TESTED. The latest preparation includes diagnostics code but keeps the
compile/runtime options OFF by default.

## Host profiler retirement and vblank observation

`profile-retirement-cp.patch` adds weak unregister plus a local RAII guard at
WorkerThreadMain entry before SetupContext. It covers normal and early return
before pthread/emutls destruction. BridgeMain already has the same repository
contract. The CPU registry and runner are owned by CPU/PRIME. SDK, exact
application and lifecycle-policy checks: PASS. Do not apply the guard twice.

`vblank-observer.patch` records the repository bridge hook already integrated:

```cpp
extern "C" void rcomp_observe_vblank(uint32_t count, uint32_t nominal_hz,
                                   uint64_t monotonic_ns, int64_t lateness_ns)
    __attribute__((weak));
```

It runs after the actual increment and before IRQ 0, reusing the already sampled
clock. It does not recalibrate or change cadence. A CPU-owned observer can report
MFTB/monotonic/count deltas at 1 Hz. Phase13 gameplay timebase ratio 1.00000758 and
vblank 60.0004 Hz were PASS; these are recorded-run measurements, not a universal
performance assumption.

`counter-atomic.patch` is a separate inactive synchronization candidate. The
public upstream contract increments the counter at both XE_SWAP and vblank;
removing either arbitrarily would change that contract. Relaxed atomic operations
would remove the C++ data race without becoming a guest-memory publication
barrier. Host concurrent/wrap policy and SDK: PASS; PS5 cause/effect: NOT TESTED.
It is not applied by the latest source recipe.
