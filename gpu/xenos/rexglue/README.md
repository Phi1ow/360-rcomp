# Xenos GPU integration

R-comp adapts only the GPU subsystem of rexglue-sdk, pinned to
`c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`. The selected subsystem contains
Xbox 360 PM4 command processing, registers, Xenos-to-SPIR-V translation and a
Vulkan backend. It runs through R-comp's PS5_Vulkan integration. CPU execution
remains statically recompiled AOT code; rexglue CPU/JIT/kernel/system code is
not linked. BSD notices are retained in `LICENSE.rexglue` and prepared sources.

The repository contains adaptations, patches and boundary shims. Public
upstream source and public build dependencies are prepared into a new `build/`
directory. Reference checkouts and the shared SDK remain read-only. No game
content, proprietary SDK files or keys belong in this repository.

## Prepare the current source chain

Use POSIX Python/Git, or FULL Cygwin Python/Git on Windows, from the checkout
root. The destination must not exist; preparation never removes an existing
tree. `deps/deps.lock` supplies the public immutable pins. The profile checks
archive and patch SHA256 values before application and verifies every selected
C/C++ source/header afterward. An interrupted or failed output remains
available for diagnosis; choose a different fresh destination for another run.

```sh
python3 gpu/xenos/rexglue/prepare-latest-opt-in.py \
  --out build/xenos-latest-src --cache build/xenos-source-cache \
  --git /usr/bin/git
```

Descriptor reuse defaults OFF. To reproduce the measured source chain of
phase15 and later, explicitly add `--descriptor-reuse`. Add `--offline` when
the verified public archives are already cached. The output records the ordered
patches and normalized source hashes in `.rcomp-latest-source-manifest.json`.
Compile using `REXGLUE_SRC` pointing to that output directory. The preparation
script does not set application/runtime options or launch a console.

The default build options are `RCOMP_XENOS_DIAGNOSTICS=OFF`,
`RCOMP_XENOS_PROFILE_TIMINGS=OFF` and `RCOMP_XENOS_RENDER_DIAGNOSTICS=OFF`.
`VulkanOutput::Create` explicitly selects synchronous shader compilation to
preserve every guest draw. Its draw-resolution axes default to 1 and accept
1..7; application wiring must request 3 on each axis for 3840x2160 output from
a logical 1280x720 video mode. Actual capture dimensions, rather than a requested
setting, are the resolution witness.

`prepare.sh` and `prepare-archive.py` remain the historical base0011
preparations, with0012 explicitly optional. They do not apply the later quiet,
barrier, diagnostics, retirement, direct presentation and publication-serial
chain. `adapt-perf-instrumentation.py` migrates the older exploratory build
layout; it cannot by itself prepare that layout from untouched public source.
The latest profile includes an exact quiet delta and retains the measured
legacy resolve trace. A historical comment calling one trace a build-tree edit
is preserved to match the archived source exactly.

## Runtime and rendering contracts

| Component | Contract |
| --- | --- |
| `rcomp/xenos_host.*` | R-comp `GuestMemory`, MMIO register bridge, ring write-pointer changes, vblank and graphics callbacks; no rexglue kernel |
| `shim/rex/system/xmemory.h` | Tracks watched physical 4 KiB runs and invalidates each watched run exactly before a new primary command buffer; CPU publication before the kick remains required |
| `rcomp/vulkan_output.*` | Vulkan provider/CP creation, effective draw-scale validation, real CPU captures and synchronous source-image visitation |
| `rcomp/vulkan_display.*` | Native swapchain blit/present, per-image completion fences, restored source layouts and completed GPU work before releasing the image lease |
| `rcomp/guest_output_presentation_cursor.h` | Strong output epoch plus 64-bit publication serial; repeated output is skipped, and a new output is committed only after successful presentation |
| `shim_src/` | R-comp implementations of the utility APIs needed by the selected GPU sources |

The command worker and bridge retire their weak profiler registrations before
thread-local destruction. Direct presentation retains the consumer lock and
image reference, uses the same queue 0, restores the source layout and completes
the blit before returning. Post-submit failure is terminal while that lease is
still held. Presentation resources have explicit completion dependencies.
Capture remains available for independent resolution/pixel evidence.

The repository's GPU glue is already integrated and is verified by the latest
manifest; do not reapply its historical patches. The private presenter/device/
instance patches are applied to the fresh source output. App wiring remains
PRIME-owned. See `perf-patches/README.md` and `render-patches/README.md` for the
active and inactive artifacts.

## Evidence and remaining work

Status date: 2026-09-30, through completed phase24. Host/SDK checks: PASS.
Pinned fresh source reproduction: PASS,692 exact normalized C/C++ files for
both the default descriptor-OFF and explicit descriptor-ON profiles. These are
source checks, not a new PS5 execution.

PS5 phase19 and later captures show actual3840x2160 output and a complete
world in gameplay. The horizontal missing-geometry defect seen at 1x is absent
in those 3x captures. Its underlying cause at 1x remains NOT TESTED. Phase18's
independent native depth/MSAA-transfer probe was PASS; no driver defect was
established by that probe.

Phase21 reference:12.6881fps; phase22 full-coverage clear elision:12.7680fps;
phase23 present-once: 12.8545 fps; phase24 raw-RX profiler: 12.7542 fps
over 89.382 s with 1140 guest swaps. Real 3840x2160 gameplay captures: PASS.
Early kstuff pause was verified during phase24 at 30 s and 300 s.
These small differences do not establish a significant performance gain.
The 30 fps objective remains FAIL. Phase24 timebase ratio 1.00000709693
and vblank 60.00370188477 Hz: PASS. Simulation speed and audibility: NOT TESTED. Physical-display pixel comparison and graceful full shutdown
remain NOT TESTED. Upload shadow/scratch, atomic-counter and GPU-checkpoint
candidates remain inactive and are excluded from the latest preparation.

Historical host checks of synthetic shaders, PM4 processing and Vulkan pixels
remain host evidence. No source-preparation, shader-policy test or TESTDOUBLE
result may be reported as proof of PS5 pixels or speed.

## Later work: the measured private copy (phases 25-142, October 2026)

The status above ends at phase 24. The performance phases after it were measured on a private copy of the prepared
source that has moved past the reviewed profile; `perf-patches/README.md` lists what waves 3 and 5 changed there (four ordered patches,
`perf-patches/wave3-*.patch`, then three, `perf-patches/wave5-*.patch`) and `docs/PERF_4K30_20260929.md` holds the measurements. Pointers for whoever builds the title:

- **Backend flags from the title**: `-DRCOMP_M6_XENOS_CVARS=NAME=VALUE,...` (cvars set before the GPU starts, logged as
  `RCOMP-XENOS-CVAR`). Current names: `execute_unclipped_draw_vs_on_cpu` (default true), `rcomp_elide_overwritten_transfers` (true),
  `rcomp_elide_overwritten_color_transfers` (false), `rcomp_async_submit` (false; **recommended on, together with the thread placement below**), `rcomp_async_chunk_kib`, `rcomp_async_cpu`,
  `rcomp_force_msaa_1x` (measurement only, never a default). Wave 5 (phases 101-142): `rcomp_dump_sample_mask` (true), `rcomp_early_rt_barrier` (true), `rcomp_direct_depth_load` (true),
  `rcomp_coalesced_load` (true), `rcomp_skip_identical_uploads` (true), `rcomp_upload_prefetch` (false, measured neutral), `rcomp_shadow_skip_every` (0 = off; **a quality option**: the shadow map pass is rendered on one frame
  out of N), and the measurement-only `rcomp_skip_gpu_work` and `rcomp_diag_skip_upload_barriers`. `anisotropic_override` is set to -1 (the guest's own anisotropy) by `rcomp/vulkan_output.cpp`.
- **Thread placement**: `RCOMP_M6_TUNING_ENV=RCOMP_CPU_MASK_<ROLE>=0x..` with the roles `GUEST` (every guest thread), `CP` (the command processor; it defaults to CPU 10), `BRIDGE` and `REPLAY` (the replay thread of
  `rcomp_async_submit`); the title sees logical CPUs 0-12, numbered in pairs that are the two threads of one core. Measured best (scale 3, phases 141 and 142): `RCOMP_CPU_MASK_GUEST=0xFF,RCOMP_CPU_MASK_REPLAY=0x100,RCOMP_CPU_MASK_BRIDGE=0x1000`
  with `rcomp_async_submit=true`; the asynchronous replay without the placement is a loss.
- **Pass accounting** (`-DRCOMP_XENOS_PROFILE_TIMINGS=ON`): `RCOMP-PROFT`/`RCOMP-GPUT`/`RCOMP-GPUN` (CP and GPU time per frame),
  `RCOMP-PASS-TABLE`/`RCOMP-PASS` (GPU time per pass class every 20 s) and `RCOMP-EV` (a bounded timeline of draw runs, transfers and
  resolves); read them with `tools/xenos_pass_analysis.py` (`passes`, `budget`, `timeline`, `overwrites`).
- The rectangle-list estimator (`DrawExtentEstimator::EstimateRectangleListBounds`) mirrors the host's rectangle-list geometry
  shader: the fourth vertex is the one reflected across the longest edge (`pipeline_cache.cpp`, `kRectangleList`).
