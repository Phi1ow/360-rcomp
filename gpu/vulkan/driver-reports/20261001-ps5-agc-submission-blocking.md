# PS5 RADV: AGC suspend point and DCB submission block the submitting thread

Date: 2026-09-30/10-01. Owner of the change: PRIME (acting for Agent 4).
Local build copy only: `build/vulkan-gta-radv-20260928/mesa-source` (pin
`cedb774b27d089fa81f46add28d0a8c13ff0f7d2`). The reference checkouts under
`$RCOMP_DEPS` and the SDK are unchanged. Patch:
`gpu/vulkan/patches/mesa/0002-ps5-async-agc-submission-and-submit-counters.patch`
(final form: asynchronous submission helper plus counters).

## Observation (PS5, GTA IV at 3840x2160, phases 27-30)

TSC counters inside `radv_ps5_submit` (winsys `radv_ps5_platform.c`), final
90 s of each 420 s run, TSC 1.596 GHz:

| Phase | Change | Share of wall time in `sceAgcSuspendPoint` | In `sceAgcDriverSubmitDcb` | clflush + copy |
| --- | --- | ---: | ---: | ---: |
| 27 | none (suspend point on the submitting thread) | ~48% (summed over CP and display threads) | <1% | <1% |
| 28 | suspend point on a helper thread | ~51% (helper) | ~28% (callers) | <1% |
| 30 | + one CP submission per guest frame | ~98% (helper) | ~19% (CP) | <1% |

A suspend point costs milliseconds while the GPU is busy (PS5_Vulkan's idle
probe measured ~130 us). With the suspend point after `SubmitDcb` on the caller,
the display thread held the shared Vulkan queue mutex ~20 ms per frame inside
`vkQueueSubmit`/`vkQueuePresentKHR`, and the Xenos command processor waited for
that mutex ~9-11 ms per frame. Moving the suspend point to a helper removed the
queue-mutex wait (display queue hold 19.9 -> 2.7 ms/frame) and raised the
comparable instrumented rate from 19.03 to 21.56 fps; the callers then blocked in
`SubmitDcb` while a suspend point ran. At 1x (phase 29, 32.48 fps) neither call
blocks: the stall scales with GPU load.

The cache flush and the copy into the submission ring are negligible; the
earlier sampler attribution to `radv_ps5_submit` was the suspend point.

## Change

1. Diagnostic counters `rcomp_radv_submit_stats[8]` (calls, words, flush, DCB,
   suspend, claim, copy cycles), printed by the Xenos bridge only when
   `RCOMP_XENOS_PROFILE_TIMINGS` is on. No behavior.
2. Asynchronous AGC submission: `radv_ps5_submit` flushes the words on the
   caller and queues them; one helper thread submits in queue order and issues a
   suspend point each time the queue drains, so every submission is followed by
   a suspend point that starts after it (the R68 prompt-start contract). Words
   remain valid because ring space is reclaimed only after the GPU completed the
   sequence. An AGC error is returned by the next submission (device lost).
   Completion, fences and waits still use the GPU-written marker, unchanged.
   Without the helper (thread creation failure) the original synchronous path
   runs.

## Contract risks and status

- Submission order across threads is preserved: jobs are queued under the
  winsys submit lock, in sequence order. PASS by construction; PS5 NOT TESTED
  until phase 32 completes.
- Whether several DCBs followed by one suspend point start as promptly as
  individually suspended ones: NOT TESTED directly; measured end to end by fps
  and frame captures.
- No minimal standalone reproducer yet beyond the in-title counters; a native
  probe timing `sceAgcSuspendPoint` against GPU load is the next step for an
  upstream report to PS5_Vulkan.
