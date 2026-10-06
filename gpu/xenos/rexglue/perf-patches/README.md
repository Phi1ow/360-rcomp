# Performance patches and activation state

> **ERRATUM (2 October 2026, wave 7): every figure measured with the shadow governor before its scope was fixed is invalid** (phases 131, 142, 160, 174-177: "+5 %", "50.43", "59.27", "60.0 fps"). A black-frame probe on the presented frames found that the governor presented a black frame every other frame in the E, F and H scenes (8,582 of 22,800 presented frames in phase 177; 0 after the intro in the control without the governor): its skip scope swallowed the HDR scene pass, which has the shadow pass's surface signature. The 60 fps was not drawing half the frames, and the owner's black flashes (reported since phase 160) were real. See the journal section 173-182 and `docs/evidence/wave7-20261002/`.
> The wave 7 changes of the private copy (the plan histogram, the governor diagnostics and scope fix, sc7-sc13) are kept as change sets in `build/prime-perf-4k30-20260929/tools-20261001/w6-session-tools/` and are not yet regenerated as `wave7-*.patch`.

Status date: 2026-09-30, through completed phase24. All changes are generic and
preserve guest commands/draws. PRIME applies and measures console steps;
reference checkouts and the shared SDK are not modified. Host/SDK validation
is separate from PS5 pixel and performance evidence.

## Active presentation chain

The direct path was activated in phase14 as six coordinated patches:

1. `direct-guest-image-presenter.patch`: synchronous image visitor under the
   existing consumer lock with a retained image reference.
2. `direct-present-fence-device.patch`: device support/completion features.
3. `direct-present-instance-dependencies.patch`: instance extension dependencies.
4. `direct-guest-image-output.patch`: GPU blit and swapchain output glue.
5. `direct-guest-image-completion.patch`: explicit completion/lifetime handling,
   including terminal post-submit errors while the lease remains alive.
6. PRIME's application wiring: direct output selection and ordinary CPU capture.

The same queue 0 orders producer and consumer GPU work; source layout is restored.
Per-swapchain-image presentation completion fences drain before destruction.
A successful visitor returns only after its blit completes. SDK/lifetime and
extension-dependency checks: PASS. Phase14 console activation/capture: PASS,
without a demonstrated FPS gain. Physical-display pixel comparison and graceful
full shutdown: NOT TESTED.

`direct-full-coverage-clear-elision.patch` is active since phase22. It omits
only the clear and intermediate write-after-write barrier when integer
`dx=dy=0` and `dw/dh` equal the full destination extent. Letterboxing retains
the black clear. Transitions, blit, source restoration, fences and lease are
unchanged. 142500 host coverage cases and SDK syntax: PASS. PS5 real4K capture:
PASS.12.7680fps versus12.6881fps reference does not establish a significant gain.

`present-new-output-presenter.patch` and `present-new-output-glue.patch` are
active since phase23. The private visitor supplies the existing 64-bit
publication serial under the consumer lock. A Display cursor retains a strong
CPU epoch for each Output, preventing allocator/handle ABA. It skips only an
already successfully presented output from that epoch. Failed/empty visitors
remain retryable; presentation commits only on success. The ordinary app boolean
contract is unchanged, so skipped output does not inflate shown-frame counts.
Producer refresh, guest swaps, shaders, draws and GPU submission lifetime are
unchanged. Display reset/destruction uses the owner's existing screen-thread
serialization. Independent 10021-assertion policy review and SDK: PASS.
PS5 phase23 capture: PASS,12.8545fps; no significant gain established.

The current repository already contains the GPU glue. Historical glue patches
are retained for review, not applied twice by source preparation. The latest
script applies the private presenter/device/instance chain and serial delta.

## Resolution and watched invalidation

`resolution-scale-api-direct.patch` is the current direct-compatible API,
active since phase19. Axes1..7 are checked before CP startup, effective backend
values are verified, and async shader compilation is forced false. Default axes
remain 1. The application explicitly requests 3x for4K; logical guest video mode
stays1280x720. Real3840x2160 gameplay captures: PASS. The earlier
`resolution-scale-api.patch` is a historical pre-direct variant and must not be
stacked on the current API.

`exact-watched-invalidation.patch` is active since phase16 in the repository
memory shim. It passes `exact_range=true` for every already watched 4 KiB run.
The upstream 64-host-page expansion amortizes fault handling, which this shim
does not use. Callback/watch/rearm policy and SDK: PASS. Console capture: PASS;
phase16 approximately17.10fps versus17.0824fps in phase15 shows no meaningful gain.

Descriptor reuse 0012 is active since phase15 in the measured private source.
It preserves texture requests/sampler lifetime and invalidates snapshots with
submission/layout changes. Preparation still requires explicit
`--descriptor-reuse`; it is never implicitly enabled by the base/default recipe.

## Inactive exact upload shadow and CPU scratch

`exact-upload-shadow.patch` remains inactive. It uses the existing
`shim/rcomp_xenos/page_upload_shadow.h`; cvar
`rcomp_shared_memory_shadow_mib=0` disables it by default. Payload capacity is
bounded to 128 MiB, metadata to physical 512 MiB page count and bounded LRU slots.
Payload allocation is lazy. Missing, evicted, reset, GPU-written or failed
snapshots force ordinary uploads. Optional cache failure cannot acknowledge a
required upload; required staging allocation failure retains the existing false.

The separate reviewed CPU-scratch delta is still inactive. Scratch defaults 0,
requires an enabled shadow, is bounded to 4 MiB and rounds down to whole host
pages. It captures guest memory once into ordinary CPU heap bytes, compares that
immutable snapshot exactly and stages only changed runs. Failed scratch
allocation falls back to staged comparison. The same captured bytes feed
staging and Remember, so concurrent guest publication cannot cause a second
read to disagree with submitted bytes. This does not create a globally atomic
guest snapshot or complete CPU dirty tracking.

GPU memexport and texture resolves invalidate all touched snapshot pages.
Backing reset/cache/shutdown clears snapshots. MakeRangeValid rearms watches
before capture. Empty changed-run lists queue no transfer/barrier; nonempty
runs retain the original conservative usage transition and barrier ordering.
Upload-pool tagging, FlushWrites, completed-submission reclamation and shutdown
waits are preserved. No CPU scratch pointer is referenced by GPU commands.

Independent static review and 25804 assertions over 1200 byte-oracle frames:
PASS. Sequential fresh-copy application and existing SDK syntax: PASS.
Actual PS5 heap/staging cache attributes, memory pressure, pixels and FPS gain:
NOT TESTED. Staging allocation, a complete guest snapshot and CPU comparisons
remain; reduced transfer/staging work does not guarantee a speedup. The latest
source recipe excludes these candidates entirely.

## Applying isolated candidates

After PRIME authorization outside a console run, first archive exact inputs and
hashes. Use FULL Cygwin Git on Windows and check forward application, then reverse
application without mutating again. Use fresh private copies for tests. Neither
this directory nor the preparation script deploys a payload, changes a console,
commits sources or automatically activates a candidate.

## Wave 3: the measured private copy after phase 24 (1 October 2026)

The copy the performance phases were measured on (`build/prime-radv-resume-20260929/xenos-source-v3`) has moved
past the reviewed profile (`../latest-source-profile.json`, phase 24): phases 25-78 edited it by hand, with backups under
`build/prime-perf-4k30-20260929/tools-20261001/before-*`. Wave 3 of the optimization audit (phases 79-100, journal section 79-100
of `docs/PERF_4K30_20260929.md`) is the first set that is also recorded as patches. They apply in order, with `patch -p1` at the
source root, to the copy as it was before phase 80, and reproduce the measured copy byte for byte (checked by applying the
four patches to the backups of that state and comparing every file). Host/SDK syntax checks of both
`RCOMP_XENOS_PROFILE_TIMINGS` settings: PASS. PS5 evidence: `docs/evidence/wave3-20261001/`.

| Patch | Content | State |
| --- | --- | --- |
| `wave3-1-pass-accounting-dynamic-ubo-estimator.patch` | `execute_unclipped_draw_vs_on_cpu` defaults to `true` (as in Xenia; +16 % in the final 90 s); the guest constants as dynamic uniform buffers over a persistent descriptor set per uniform-pool page (bindings -25..-29 % of CP time); the pass accounting of profile builds (`RCOMP-PASS-TABLE`, `RCOMP-EV`, CP stage scopes, slots 32-56 of `RCOMP-PROFT`), compiled only with `RCOMP_XENOS_PROFILE_TIMINGS=ON` | estimator and constants: on; accounting: opt-in build |
| `wave3-2-overwritten-depth-transfer-elision.patch` | `DrawExtentEstimator::EstimateRectangleListBounds`, `Transfer::overwrite_cutout`, `RenderTargetCache::Update(..., overwrite_candidate)`: no ownership transfer into the depth and stencil area a clearing rectangle draw replaces; cvar `rcomp_elide_overwritten_transfers` | on (default set by patch 4; +3 % per scene, kstuff-paused PS5 runs) |
| `wave3-3-overlapped-replay.patch` | `rcomp_async_submit`, `rcomp_async_chunk_kib`: the deferred commands are replayed and submitted by a helper thread (`DeferredCommandBuffer::SizeBytes/SwapStream`, `AwaitSubmitted`) | off: correct, no gain while the GPU is the limit |
| `wave3-4-color-transfer-elision-and-experiment-knobs.patch` | the mask form of `Update`'s argument and `OverwrittenColorRenderTargets` (`rcomp_elide_overwritten_color_transfers`, off: +0.4 %, inside the noise); the default of `rcomp_elide_overwritten_transfers` becomes `true`; a forced hand-off of the commands at the swap and `rcomp_async_cpu` (pin of the replay thread) for the replay option; `rcomp_force_msaa_1x` (MEASUREMENT ONLY: the guest's MSAA mode replaced by 1x, owner decision, never a default) | knobs: off |

Backend cvars are set from the title with `-DRCOMP_M6_XENOS_CVARS=NAME=VALUE,NAME=VALUE` (`app/m6`), logged as
`RCOMP-XENOS-CVAR`; a name the backend does not know stops the title.

## Wave 5: the resolve path, the uploads, the replay (1 October 2026)

The copy as it stands after wave 5 (phases 101-142, journal section 101-142 of `docs/PERF_4K30_20260929.md`, evidence `docs/evidence/wave5-20261001/`). Three patches apply in order, with `patch -p1` at the source root, **to the copy as it was
at the end of wave 3 (phase 100, build `92b86916db8564e2`)**, i.e. after the four wave 3 patches, and reproduce the measured copy byte for byte. The base of each file is verified against the sha256 that phase 100 recorded (`private-gpu-inputs.sha256` of
its run directory: `make_base_set.py` and `make_wave5_patches.py` in `docs/evidence/wave5-20261001/tools/`); the patches were generated from that verified set and checked by applying them to a fresh copy of it and comparing every file. Host/SDK syntax checks of both
`RCOMP_XENOS_PROFILE_TIMINGS` settings: PASS. The three shader headers' sources are in `../rcomp/shaders` (`texture_load_scaled_image.comp`, `build.sh`).

| Patch | Content | State |
| --- | --- | --- |
| `wave5-1-command-processor-uploads-and-accounting.patch` | **the skip of identical uploads** (`rcomp_skip_identical_uploads`: XXH3 hash of each requested 4 KiB page, `SharedMemory::RcompPagesWrittenByGpu`), the optional prefetching copy (`rcomp_upload_prefetch`), the measurement-only `rcomp_diag_skip_upload_barriers`; **CP-1** (`CopyFloatConstants`: float constants copied by runs); **the shadow governor** (`rcomp_shadow_skip_every`); **the texture lookup memo** (`TextureCache::FindOrCreateTexture`); `rcomp_skip_gpu_work` (measurement only); the CP's host thread roles (`HostThread::Start(..., "CP")`) and its default CPU 10 yielding to `RCOMP_CPU_MASK_CP`; the accounting of profile builds (draw classes, fine CP slots, upload and register statistics, replayed command statistics; opt-in `RCOMP_XENOS_PROFILE_TIMINGS`) and the timing line of the skip in lean builds | skip, CP-1, memo: on; prefetch, governor, measurement knobs: off |
| `wave5-2-render-target-cache-resolve-path.patch` | the dump sample mask (`rcomp_dump_sample_mask`: the resolve copy's sample select decides which guest samples the dump writes) and the early render target barrier (`rcomp_early_rt_barrier`: the dumped targets return to their drawing layout before the copy is recorded) | on |
| `wave5-3-texture-cache-direct-depth-load.patch` | direct load of depth textures (`rcomp_direct_depth_load`: a conversion mode of the scaled-load shader, two new pipelines and shader headers) and the **coalesced** direct load shaders (`rcomp_coalesced_load`: 32 lanes store 32 consecutive texels; the previous layout is kept as `_v0` and selectable); the last-view memo of `VulkanTexture::GetView` | on |

Flags are set from the title as in wave 3 (`-DRCOMP_M6_XENOS_CVARS=NAME=VALUE,...`). Changes of wave 5 that live outside the private copy are ordinary (uncommitted) edits of this repository: `rcomp_xenos/diagnostics.h` and `host_thread.h`
(thread roles and placement masks), `rcomp/xenos_host.cpp`, `app/m6` (topology probe, tuning environment), `runtime/src/guest_thread.cpp` (`RCOMP_CPU_MASK_GUEST`), the shaders under `rcomp/shaders`.

## Wave 6: the revalidation that was the hash, vertex slots, the fused resolve (1-2 October 2026)

The copy as it stands after wave 6 (phases 143-169, journal section 143- of `docs/PERF_4K30_20260929.md`, evidence `docs/evidence/wave6-20261001/`). Three patches apply in order, with `patch -p1` at the
source root, **to the copy as it stood at the end of wave 5** (after the four wave 3 and the three wave 5 patches); they were generated from the backups the arms were applied with
(`build/prime-perf-4k30-20260929/tools-20261001/before-{storecmp,cp234,v1,fused,counters}`, the earliest backup of each file is its wave 5 state) and checked by applying them to a fresh
copy of that state and comparing every file with the copy the console ran. They are grouped by file, not by topic:

| Patch | Content |
| --- | --- |
| `wave6-1-shared-memory-write-tracking-rolling-revalidation-audit.patch` | the shared memory side of the write tracking without the per-frame revalidation: the GPU-written flags kept by `MakeRangeValid` and the invalidation callback, `RcompGetValidCpuPages`, `SetSystemPageBlocksValidWithGpuDataWrittenSlice` (rolling revalidation, returns the byte range it covered), the stale-page audit (`rcomp_stale_audit`, `RcompStaleAudit`, `RcompAuditUsedRange`), `Memory::NoteCpWrite` calls for the writes of the base command processor (read pointer, scratch registers, `REG_TO_MEM`, `MEM_WRITE`, `COND_WRITE`, `EVENT_WRITE_SHD`/`EXT`/`ZPD`), CP-3 (the texture lookup memo flushed only when a scaled resolve changed something) |
| `wave6-2-command-processor-constants-vertex-slots-counters.patch` | the Vulkan command processor: CP-2 (`rcomp_skip_unchanged_constants`: a shader constant write that carries the register's values is ignored), CP-4 (the occlusion and memory export / resolve read-back writes marked), CP-5 (`rcomp_revalidate_frames`, used at frame close, with the slice's vertex slots forgotten), **V-1** (the vertex buffer slots whose pages the CPU invalidated are forgotten: a global watch on the shared memory), the vertex shortcut audit hook, and the **async placeholder counters** (`RCOMP-ASYNC-PLACEHOLDER` lines: draws dropped and frames not presented while pipelines are created; the title forces `async_shader_compilation=false`, so they stay at zero) with `VulkanPipelineCache::RcompPendingCreations`, and the **hitch source counters** (`RCOMP-HITCH-SOURCES` lines: shader translations and pipeline creations done synchronously on the CP thread, counted and timed on their slow paths) |
| `wave6-3-render-target-cache-fused-resolve.patch` | `rcomp_fused_resolve` (the fused render target dump + 32-bit fast resolve copy: `PlanFusedResolve`, `GetFusedResolvePipeline`, `RecordFusedResolve`, the GLSL compiled at run time), `rcomp_fused_resolve_verify` (the in-title word-by-word verification against the two-pass path), `rcomp_diag_log_dump_spirv`, and the measurement-only `rcomp_diag_skip_resolve_dump` / `rcomp_diag_skip_resolve_copy` |

Flags are set from the title as before (`-DRCOMP_M6_XENOS_CVARS=NAME=VALUE,...`). **Recommended after wave 6** (lossless, 4K, phase 159: 50.556 fps): `rcomp_async_submit=true,rcomp_revalidate_frames=8` (the SDK's `clear_memory_page_state` stays at its default, true);
`clear_memory_page_state=false` is within the noise at 4K (phases 152-154) and removes the staleness bound, it needs the write tracking to be complete (see the stale-page audit). Quality option, off by default, the owner's decision:
`rcomp_shadow_skip_every=2` (phase 160: 59.269 fps). Off (bit-identical in the title on the scripted run, phase 171: 434,176 verified resolves; its rate is not yet measured on a clean scripted run): `rcomp_fused_resolve`. Changes of wave 6 that live outside the private copy are ordinary (uncommitted) edits of this repository:
`include/rcomp/guest_write_tracking.h`, `include/rcomp/ppc_prelude.h` (the store comparison, `-DRCOMP_M6_STORE_COMPARE`, off: no measured gain), `cpu/patches/xenonrecomp/0017-guest-vector-store-macro.patch`,
`gpu/xenos/rexglue/shim/rex/system/xmemory.h` (`NoteCpWrite`, `RCOMP-INVALIDATE` statistics), `runtime/src/xma_decoder.cpp`, `runtime/src/hle_xboxkrnl_{threads,sched}.cpp` (the writers the tracking has to be told about),
`runtime/tests/test_store_compare.cpp`, `tools/stale_page_report.py`, `tools/host_gpu_bench/fused_resolve/`.

## Wave 8 (2 October 2026, Episodes from Liberty City)

- `wave8-1-inline-bit-scans-constants-set-memo.patch`: `rex::lzcnt/tzcnt/bit_scan_forward` for 32 and 64 bits become inline on non-Windows GNU/clang builds (they were out-of-line calls in the draw path's bitmap loops), and the constants descriptor set of the last uniform buffer page is remembered. Console arms 43/44: neutral on frame rate (56.46 / 56.50 fps against 56.46 for the reference).
- `wave8-2-render-target-cache-fused-full-copy-resolve.patch`: the fused dump + resolve copy also covers the "full" 32- and 64-bit copies (conversion, exponent bias, red/blue swap, averaging of MSAA samples; `tools/host_gpu_bench/fused_resolve/fused_full.comp`). **Off by default**: `rcomp_fused_resolve_full_mask` (1 = 8:8:8:8, 2 = 32-bit float, 4 = float 2:10:10:10 to 32 bits, 8 = to 64 bits). PS5 verification mode (arm 45): 413,899 fused resolves compared with the two-pass destination, 0 words differ; arm 46 with mask 15: 56.57 fps, no measurable gain.
- Tried and **not kept**: a texture lookup memo robust to scaled-resolve toggles (no effect) and a per-submission cache of texture descriptor sets by payload (about 1.6 fps slower on the PS5, arm 44 against 38).

## Wave 9 (3 October 2026, Halo 3 overexposure)

- `wave9-1-resolve-readback-size-limit-and-trace.patch` (applies with `patch -p1` at the source root to the private copy `build/prime-radv-resume-20260929/xenos-source-v3` as it stood on 3 October 2026; base sha256 in `build/xenos-h3-readback-20261003/base.sha256`): the cvar `rcomp_readback_resolve_max_bytes` (default 0 = every resolve, the SDK's behavior) limits the SDK's `readback_resolve` (default `none`) to the resolves that write at most N bytes; `RCOMP-READBACK-RESOLVE` (mode and limit at start), `RCOMP-READBACK` and `RCOMP-READBACK-DATA` lines (the first 32 read-backs and every 4096th: address, length, destination format and size, first guest words); the `RCOMP-RESOLVE` trace line gains the surface's EDRAM format (`rf`), depth flag (`dz`), exponent bias (`eb`), sample select (`ss`), MSAA (`ms`), number format (`dn`) and swap (`sw`). Nothing changes while `readback_resolve` stays `none`.
- Why: Halo 3 reads GPU-resolved data on the CPU for its HDR adaptation (Xenia's game-compatibility label `gpu-readback` on 4D5307E6). Without a read-back the guest reads stale guest memory instead of the resolved luminance. Candidate title configuration: `-DRCOMP_M6_XENOS_CVARS=readback_resolve=full,rcomp_readback_resolve_max_bytes=65536` (or `readback_resolve=fast`, one frame of delay, no wait for the GPU).
- Status: PS5 compiler build of both files with `RCOMP_XENOS_PROFILE_TIMINGS` 0 and 1: PASS; forward and reverse application: PASS. PS5 pixels and frame rate: NOT TESTED.
- `wave9-2-readback-downscale-fix-and-data-statistics.patch` (applies with `patch -p1` on top of wave9-1; checked against the private copy as it stood after wave9-1 was applied, 3 October 2026): PS5 arm B (`readback_resolve=full`, 3x) read back only zero words. The SDK's read-back of a resolution-scaled resolve binds the whole scaled resolve buffer (512 MiB times the scale area: 4.5 GiB at 3x, beyond `maxStorageBufferRange`) at offset 0, and its shader assumes that the host pixels of one guest pixel are consecutive, which is not the layout the resolve shaders write (16-byte groups of guest data, each stored as `scale_x * scale_y` consecutive 16-byte blocks of host rows; see the fused resolve shaders). `rcomp_readback_downscale` (default **on**; it only matters when `readback_resolve` is not `none`) replaces that pipeline with a GLSL shader compiled at start that follows the resolve layout, and binds only the range it reads (`RCOMP-READBACK-DOWNSCALE rcomp_shader=1` at start). The trace logs the first 32 read-backs and then every 64th, with `nonzero_words` over the whole range and, for 16-bit float destinations, `RCOMP-READBACK-HALF` (finite nonzero count, min, max, infinities/NaNs as the big-endian guest reads them, and the other byte order's maximum). Status: PS5 compiler build of both files with `RCOMP_XENOS_PROFILE_TIMINGS` 0 and 1: PASS; forward and reverse application: PASS. Downscale correctness on the PS5: NOT TESTED.
- `wave9-3-rt-2-10-10-10-exact-host-format.patch` (applies with `patch -p1` on top of wave9-2; base checked against the private copy after wave9-2 was applied): PS5 arm H (fragment shader interlock) renders Halo 3's campaign correctly and the host render target path does not, with the same resolves. Halo 3 writes its HDR "dark" buffer (scene / DARK_COLOR_MULTIPLIER, render target 1, `k_2_10_10_10_AS_10_10_10_10`, EDRAM base 1216) next to the LDR buffer (`k_8_8_8_8`, base 608), and its bloom is taken from max(LDR, dark * multiplier); the host path stores `k_2_10_10_10` targets as A8B8G8R8_UNORM (8-bit color, 8-bit alpha) where the EDRAM and the interlock path store 10:10:10:2. `rcomp_rt_2_10_10_10_exact` (default **off**, A/B) hosts them as A2B10G10R10_UNORM_PACK32 (`RCOMP-RT-2-10-10-10 requested=1 host=...` at start; falls back when the format lacks attachment, blending or sampling). Dumps, resolves, clears and ownership transfers convert through floats and are unchanged. The read-back trace also prints `RCOMP-READBACK-UNORM` (component means and 2-bit alpha counts of 10:10:10:2 and 8:8:8:8 destinations) to compare a resolve between the two paths. Status: PS5 compiler build of both files with `RCOMP_XENOS_PROFILE_TIMINGS` 0 and 1: PASS; forward and reverse application: PASS. PS5 pixels: NOT TESTED.
- `wave9-4-readback-frame-trace.patch` (applies with `patch -p1` on top of wave9-3; base checked against the private copy after wave9-3): diagnostics only, no behavior change. `rcomp_readback_trace_first_frame` (default -1, off) and `rcomp_readback_trace_frames` (default 2) log every resolve read-back of those frames with `RCOMP-READBACK-SOURCE` (copied surface: source select, EDRAM base, format, pitch, MSAA, sample select, exponent bias, clear flags, draws since the previous traced resolve) next to the existing statistics; `RCOMP-READBACK-UNORM` decodes destinations without an endian swap in host order. Arm L (wave9-3 on) did not fix Halo 3, so the 8-bit storage of 10:10:10:2 targets is not the cause. Status: PS5 compiler build of both files with `RCOMP_XENOS_PROFILE_TIMINGS` 0 and 1: PASS; forward and reverse application: PASS.

## Wave 10 (3 October 2026, Halo 3 at 4K)

- `wave10-1-async-memexport-readback-sync-points-wait-reg-mem.patch` (applies with `patch -p1` on top of wave9-4; base sha256 in `build/xenos-h3-memexport-20261003/base.sha256`; changes `include/rex/graphics/command_processor.h`, `src/graphics/command_processor.cpp`, `include/rex/graphics/vulkan/command_processor.h`, `src/graphics/vulkan/command_processor.cpp`; the base header gains a virtual, so every file including it is rebuilt).
- Why: Halo 3's profile at scale 3 (90-150 s, 1,554 draws a frame, about 21 fps) has one fence site, 12.6 blocking fence waits and 20.4 ms a frame on the command processor and a GPU idle gap of 23.7 ms a frame: the memexport draws (Halo 3's particles) go through the SDK's read-back (`readback_memexport`, on by default), whose fast path falls back to `AwaitAllQueueOperationsCompletion` whenever no older copy of the same range has completed; each wait ends the submission (13.6 submissions a frame) and drains the GPU, so the command processor and the GPU take turns. At scale 1 the GPU part of each turn is small, which is why 720p holds 30 fps and 4K does not.
- `rcomp_memexport_readback_async` (default **0** = the SDK paths, unchanged): 1 or 2 record each memexport draw's copy into a host-visible ring (4 MiB chunks, at most 32) and go on; the copies reach the guest memory in draw order when their submission is seen completed (any fence poll, the command processor's idle and wait loops, the frames-in-flight wait). The copy is not marked as a CPU write: the GPU already holds those bytes, and a re-upload could replace newer memexport results. Mode 1 also waits for the pending copies before a packet through which the guest can observe the GPU's progress (INTERRUPT, EVENT_WRITE_SHD/EXT/ZPD, MEM_WRITE, REG_TO_MEM, COND_WRITE, a scratch register write-back, a WAIT_REG_MEM that does not match at once); mode 2 does not (the data lags by at most the frames in flight, as the SDK fast path's previous-copy data does). Both modes wait when CPU writes invalidate a page a pending copy covers (before the GPU re-reads it from guest memory). Recommended for the generic profile: 1 (exact with respect to the guest's synchronization); 2 only if the counters show that mode 1 still blocks often.
- `rcomp_wait_reg_mem_spin_us` (default **0** = the SDK's sleep of wait/0x100 ms between polls): poll, yielding, for up to N microseconds before that sleep. Timing only.
- Counters, every build: `RCOMP-MEMEXPORT-RB config` at start; `RCOMP-MEMEXPORT-RB` every 300 frames (draws, bytes, copies retired by polling or by a wait, average lag in frames, pending, chunks, and in mode 0 the SDK's fast-path hits, fast-to-full fallbacks, full-path waits and their time); `RCOMP-MEMEXPORT-RB-FLUSH` (modes 1 and 2: per trigger, flushes / blocking waits / ms). Profile builds: `RCOMP-WAITREGMEM` every 300 swaps (per polled register or address: function, reference, mask, calls, calls that did not match at once, sleeps, spins, time, last value).
- Status: PS5 compiler build (Halo 3 archives' commands) of both changed sources and of render_target_cache, texture_cache and shared_memory against the new headers, with `RCOMP_XENOS_PROFILE_TIMINGS` 0 and 1: PASS; forward and reverse application: PASS. PS5 rate, pixels and particles: NOT TESTED.
- `wave9-5-host-rt-dynamic-color-writes.patch` (applies with `patch -p1` on top of wave9-4, independently of `wave10-1`: checked against the private copy with wave10-1 applied, and the five files of that combination compile): the arm T traces (same scene, every resolve of two frames, host render targets vs fragment shader interlock) agree up to the albedo, normal, depth and shadow resolves and first differ at the output of the lighting pass (render target 0, k_8_8_8_8 at EDRAM 608: 32 % of the pixels with zero color against 1-2 % with interlock, color about 2.5 times lower). The SDK's comments name that pass: 4D5307E6 binds two render targets at the same EDRAM base and picks the one it writes with a constant condition; the host path keeps only the lower one (`RenderTargetCache::Update`), drops writes to the other, and applies the alpha test with render target 0's alpha even when that output was not written, while the interlock path tracks the outputs written on the execution path. `rcomp_host_rt_dynamic_color_writes` (default **off**, A/B; `RCOMP-HOST-RT-DYNAMIC-WRITES 1` at start) gives the host path the same tracking: alpha test and alpha to coverage only when render target 0 is written, and a written output of a render target that shares the EDRAM base of a lower-index one replaces the bound one's output (the command processor passes the bases of the used targets in `edram_rt_base_dwords_scaled`). Status: PS5 compiler build of the five files with `RCOMP_XENOS_PROFILE_TIMINGS` 0 and 1, alone and with wave10-1: PASS; forward and reverse application: PASS. SPIR-V validation on the host: NOT TESTED (no host build of the translator in this environment); PS5 pixels: NOT TESTED.
- PS5 arms of wave10-1 (correctness set of the HDR investigation, scale 3, lean): R1 (`rcomp_memexport_readback_async=1`) 14-18 fps, no gain: every EVENT_WRITE_SHD (3.9 a frame) found copies pending and waited (about 39 ms a frame), and the per-draw copies moved 54 MB a frame (97 memexport draws, 562 KB each: a range is the stream constant's whole capacity). R2 (`=2`) 17-20.5 fps. R3 (`readback_memexport=false`) 27.2-29.0 fps: the read-back is the main 4K cost.
- `wave10-2-merged-memexport-copies-deferred-guest-sync-no-readback-mode.patch` (applies with `patch -p1` on top of wave10-1; also applies, with line offsets, on top of wave10-1 + wave9-5; hashes in `build/xenos-h3-memexport-20261003/base2.sha256` and `base2-private-with-wave9-5.sha256`). Modes 1 and 2: a memexport draw only notes its ranges; the end of each submission copies their merged union once (one barrier pair instead of one per draw; 8 MiB chunks, at most 32). Mode 1: an EVENT_WRITE_SHD value, a scratch write-back or an interrupt that follows pending copies is queued behind them and becomes visible when the GPU work before it has completed (the console's end-of-pipe behaviour) instead of making the command processor wait; the idle loop and unmatched WAIT_REG_MEM polls submit what is waited for and wait at most 1 ms at a time; the other guest-visible packets (EVENT_WRITE_EXT/ZPD, MEM_WRITE, REG_TO_MEM, COND_WRITE) still wait. New mode 3: no read-back (the speed of `readback_memexport=false`), the memexported pages kept in a bitmap to count CPU writes into them (`RCOMP-MEMEXPORT-CPU-WRITE`, `cpu_write_events`). Counters: `RCOMP-MEMEXPORT-RB` adds `copies`, `copied_bytes`, `deferred_writes`, `deferred_irqs`, `avg_visible_delay_us`, `cpu_write_*`.
- Not done, BLOCKED outside this directory: copies on demand when the guest CPU reads a memexported page. Loads of the lean build are plain dereferences (`include/rcomp/ppc_prelude.h`, unchecked `PPC_LOAD_*`) and R-comp has no fault-based watches (patch 0009); detecting reads needs page protection of the guest physical views with a SIGSEGV handler that waits for the command processor (platform and runtime workstreams, PS5 granularity 16 KiB, every host reader of guest memory must unprotect first), or load instrumentation in the generator. The written range cannot be narrowed on the CPU: the export index is computed by the shader.
- Status: PS5 compiler build of both changed sources, with the other sources against the new headers, `RCOMP_XENOS_PROFILE_TIMINGS` 0 and 1, on wave10-1 and on wave10-1 + wave9-5: PASS; forward and reverse application on wave10-1: PASS; forward application on the private copy with wave9-5: PASS (offsets only). PS5: NOT TESTED.
- `wave9-6-host-rt-dynamic-color-write-parts.patch` (applies with `patch -p1` on top of the private copy with wave10-1, wave9-5 and wave10-2; touches only the translator header, `spirv_translator_rb.cpp` and `vulkan/pipeline_cache.cpp`): PS5 arm U1 (wave9-5 on) was brighter than U2 (off). The host path cannot leave an unwritten render target untouched per pixel as the interlock path does, so the alpha-test gating of wave9-5 lets pixels whose render target 0 output was not written store an undefined value into render target 0 where the old behavior killed them. `rcomp_host_rt_dynamic_color_writes_parts` (default 3, the wave9-5 behavior; `RCOMP-HOST-RT-DYNAMIC-WRITES parts=N` at start) splits the two parts for A/B: 1 = only the merge of render targets sharing an EDRAM base, 2 = only the alpha-test gating. Status: PS5 compiler build with the private copy's patch set, `RCOMP_XENOS_PROFILE_TIMINGS` 0 and 1: PASS; forward and reverse application: PASS. PS5 pixels: NOT TESTED.
- PS5 arm R5 (wave10-2, mode 1): about 20 fps, no gain, and the image darker (luma 20 against 40): the deferred EVENT_WRITE_SHD values became visible 35 ms late on average (51,548 deferred), the command processor idled waiting for them (`idle` 38 s), and Halo 3's exposure, which its CPU computes from memexport output, lagged. Mode 1 is not recommended. With memexport read-back off the image is brighter (luma 67): the CPU does consume some memexport output.
- `wave10-3-memexport-readback-size-limit-and-histogram.patch` (applies with `patch -p1` on top of wave10-2, also on the private copy with wave10-2 + wave9-5; hashes in `build/xenos-h3-memexport-20261003/base3.sha256`, `base3-private.sha256`): `rcomp_memexport_readback_max_bytes` (default 0 = every draw, the SDK's behavior) limits the SDK read-back (mode 0) to draws whose memexport ranges total at most N bytes; larger ones are not read back (as `readback_memexport=false`), smaller ones keep the SDK paths (fast path with fallback to the waiting full path). Every 300 frames `RCOMP-MEMEXPORT-SIZES` (per size bucket <=256 B .. >1 MiB: draws and KB a frame, distinct first base addresses, CPU-written pages of unread exports) and `RCOMP-MEMEXPORT-SMALL` (the 16 most frequent exports of at most 4 KiB: base, bytes, draws a frame); `RCOMP-MEMEXPORT-CPU-WRITE` (first 32, then every 1024th: a CPU write into pages of an export not read back, with the export's size bucket); `RCOMP-MEMEXPORT-RB` adds `skipped_draws`, `skipped_bytes`, `unread_pages`. Status: PS5 compiler build (both settings of `RCOMP_XENOS_PROFILE_TIMINGS`) on wave10-2 and on the private copy: PASS; forward and reverse application: PASS. PS5: NOT TESTED.
