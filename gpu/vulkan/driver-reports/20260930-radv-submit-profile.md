# PS5 RADV submission profile audit, 2026-09-30

**PASS** for the archived phase19 counter/link-map audit. **NOT TESTED** for a CPU/GPU time breakdown, new performance optimization, or driver defect. No driver/SDK/reference or active title source was modified by this audit.

Phase19 build 6163187d6108f2b8 uses the actual 3x draw scale. PRIME reports a complete 3840x2160 intro and final car/HUD/scene, with the earlier 256-pixel rendering defect absent. Its archived swap-pacing summary measures 1170 frames in 88.529 s, **13.216008 fps**, over a transition into gameplay; last five intervals are 15.7729, 15.6250, 15.6169, 15.6986 and 15.9659 fps. These values remain below the 30 fps objective and do not establish a stable same-scene benchmark against another phase.

Evidence is under build/prime-perf-4k30-20260929/19-resolution-3x. Read only pc-window.log for the same window as pc-symbols.txt: its GPU group totals 152052 interrupted-thread samples. The full rcomp_title.err contains a different, longer window and must not be substituted for that denominator.

| Stack heuristic address | Count | GPU group share | Actual interpretation |
| --- | ---: | ---: | --- |
| 800000000518fdb2 | 79514 | 52.29395% | BridgeMain library-call stack attribution; may include sleeping thread time |
| 80000000068bc7c8 | 12149 | 7.99003% | Address of queue.sync_lock in BSS, not executable code |
| 80000000051280e0 | 11305 | 7.43496% | UploadRanges function-entry candidate found on stack |
| 80000000009df220 | 10525 | 6.92197% | radv_ps5_submit function-entry candidate found on stack |

The current sampler in cpu/runtime/indirect.cpp:175-183 scans the first 128 stack words when system-library RIP is sampled. It accepts the first value in a broad fixed title-address interval and tags it with bit63. This is neither an unwind nor evidence that the accepted value was an executed return address: data addresses and stored function pointers can pass. Interrupted threads waiting in libraries continue to contribute wall-time samples, so these percentages are not CPU utilization.

The archived link map puts title .text at load-relative [0,0x4fb77e0), hence raw [0x400000,0x53b77e0), and radv_ps5_queue_storage at load-relative 0x6484738, size 0x380a0. The corresponding BSS range is raw [0x6884738,0x68bc7d8). The masked sample 0x68bc7c8 is 0x38090 bytes into that object, outside .text. Inspection of the same build's DWARF struct radv_ps5_queue identified sync_lock at byte 0x38090 and sync_cond at 0x38098, consistent with the pinned header. radv_ps5_winsys.c:25 declares the object static struct data. Optimizing a fictitious queue_storage function would target a profiling artifact.

radv_ps5_submit is load-relative [0x5df220,0x5df287), size 0x67. Every sample in this function's range in pc-window.log is the single tagged entry 0x80000000009df220. The same-build disassembly placed CPU cache flush instructions at 0x5df231..0x5df25c, the AGC submit call return at 0x5df277, and SuspendPoint call return at 0x5df281. Thus the cited 6.9% does not distinguish cache-flush CPU work from the system submit call, suspend point, or a stored function pointer. A PF_X/text-only filter removes BSS acceptance but cannot by itself make a stack function pointer into a return address.

## Submission and waiting contracts

Pinned sources are build/vulkan-gta-radv-20260928/mesa-source/src/amd/vulkan/winsys/ps5/{radv_ps5_winsys.c,radv_ps5_winsys.h,radv_ps5_cs.c,radv_ps5_sync.c,radv_ps5_platform.c}.

- radv_ps5_cs.c:500-654 checks command streams, waits for semaphore signals to be **submitted**, then takes the shared submit_lock, claims/copies command words, submits all parts in order, and releases the lock. Same-queue order implements submitted semaphore dependencies; a separate wait can precede a tessellation factor ring change.
- The shared command ring is 16 MiB, with 4096 in-flight records. Claim waits for GPU sequence completion on ring overlap or record exhaustion; large submissions can use dedicated storage. Those waits are real ownership constraints and cannot safely be removed on the basis of the misleading data-address category.
- radv_ps5_queue_wait_seq polls the completion marker, spins for at most 1.5 ms, then sleeps in 1 ms steps (plus a stall diagnostic bound). Therefore a sampled wait can be GPU latency or sleeping wall time rather than sustained CPU work.
- radv_ps5_sync.c:123-165 locks sync_lock, releases it before a submitted sequence wait, or uses a condition wait for signals not yet submitted. A BSS sync_lock value observed on a library stack is compatible with such calls but does not identify a precise call or duration.
- radv_ps5_platform.c:646-660 flushes command memory with CPU cache-line flushes/fences, calls sceAgcDriverSubmitDcb, then sceAgcSuspendPoint. Its public source comment documents a refresh-late submission without the suspend point. Preserve cache coherence and this submit-start contract until a minimal experiment establishes a replacement; this audit does not justify a driver patch.

## Generic next steps proposed to PRIME

First correct the sampler generically. PRIME can expose read-only relocated RX bounds from the title's linker script or validated ELF PF_X LOAD intervals; Agent1 can use them for candidate filtering instead of the broad fixed address window. Preserve actual system-library RIP separately from the stack candidate, and label candidates as heuristics. Reject BSS/data; do not present function-entry candidates as measured CPU cost. An interface under include/rcomp or a root linker change belongs to PRIME. No such cross-lot edit was made here.

Keep diagnostics-off 3x and actual kstuff state constant for comparable runs. If accurate samples still motivate submit analysis, isolate observed queue-wait time, command-copy/flush time, and API-call duration in a bounded native probe using public Vulkan integration; require end-to-end FPS and image evidence. No SDK/reference instrumentation or driver patch is proposed from current data.

A concrete integration candidate is available from Agent5: build/xenos-direct-full-coverage-20260930/direct-full-coverage-clear-elision.patch, SHA256 885f2f2019e08457710bdb2f56ead8d5c936e0667b06326ffc4997eebbe71e81. PresentGuestImage clears the full swapchain black before a blit. When dx==0, dy==0, dw==extent.width and dh==extent.height after aspect-fit rounding, the blit replaces every destination RGBA texel, including alpha, without blending. Omitting only that redundant clear and its clear-to-blit WAW barrier preserves the source/destination pre/post transitions, conversion, mailbox lease, completion fence, present semaphores and layout restoration. Letterbox borders retain black clear. Independent static review: PASS, no P1 found. It avoids one logical 3840x2160x4=33177600-byte target write per 4K full-coverage frame; compression/fast-clear may reduce actual traffic, so no FPS gain is claimed. PS5 pixels/performance for this candidate: NOT TESTED until PRIME runs it.

The native original depth probe phase18 is PS5 PASS for its exact binary float-depth/stencil/sample mapping case only; see depth-msaa-transfer/PS5_RESULTS.md. It does not establish the title's sloped-triangle behavior or a driver defect.

## Reproduction and delivered files

Run the host-only archive check with the pinned Cygwin environment:

```sh
export PATH=/usr/bin:/bin
python3 build/vulkan-radv-profile-audit-20260930/audit_profile.py
```

Return 0; output profile-audit.json and audit.log. Assertions verify the 152052-sample denominator, object and function bounds, data-address exclusion from .text, and the single submit-entry sample. JSON pins SHA256 of BUILD_ID.txt, pc-window.log, pc-symbols.txt, title.map and summary.json. This is archive analysis, not another console test.

Changed by this audit: this report; build/vulkan-radv-profile-audit-20260930/{audit_profile.py,audit.log,profile-audit.json}. Separate authorized helper preparation resides only under build/vulkan-kstuff-control-20260930 with original query/pause/resume source, RX/RW linker copy, ELF artifacts, public source-reference hash, build.log and README. SDK compile/link PASS0; helper console operation NOT TESTED by Agent4. PRIME owns console actions, source integration and commits.