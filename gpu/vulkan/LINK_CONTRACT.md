# PS5_Vulkan link contract (lot 4)

Written by PRIME from `gpu/vulkan/ps5/build-ps5.sh` (Agent 4) after Agent 4
was interrupted by an API limit before writing this file. Every statement
below is taken from the script and the snapshot manifests, and was
re-checked on 2026-09-25.

## Sources (pinned)

- PS5_Vulkan @9639c4188393 (GPL-3.0). Built from a **copy** in
  `build/vulkan-ps5-driver/PS5_Vulkan`; the shared checkout in `$RCOMP_DEPS`
  is never touched.
- PS5_RetroArch @18dc1059c497 (GPL-3.0): only `tooling/native` (CRT,
  `ps5-pie.ld`, `app-symbols.map`, `app_cpp_runtime.cpp`, ELF→SELF tool) and
  `tooling/ps5-stubs` (`libSceAgc`, `libSceAgcDriver` link stubs).
- ps5-payload-sdk v0.42 (sha256 8cfbc7cd…02da): sysroot, prospero-lld,
  `libc++.a`, `libc++abi.a`, `libunwind.a`, `.so` link stubs.

Snapshots: `build/vulkan-ps5-artifacts/{driver,retroarch-link}` with
`MANIFEST.json` (source commit, per-file size/sha256/mode) and `SHA256SUMS`.
The build verifies the hashes before linking and again after linking (detects
a modification during the build).

## Link line (same for the probe and the draw test)

```
prospero-lld -T ps5-pie.ld --eh-frame-hdr --no-dynamic-linker \
  --version-script app-symbols.map --exclude-libs=ALL -e _start \
  app_crt.o app_cpp_runtime.o <test>.o rcvk_loader.o libSceAgc.so libSceAgcDriver.so \
  --whole-archive libps5vk.ps5.a libvk_runtime.ps5.a libpsbc_driver.ps5.a libpsbc_support.ps5.a \
  --no-whole-archive u_thread.o anon_file.o os_file.o \
  --start-group libc++.a libc++abi.a libunwind.a [libclang_rt.builtins] --end-group \
  --as-needed $SDK/target/lib/*.so
```

| Artifact | Role | sha256 (snapshot) |
| --- | --- | --- |
| `build/driver/ps5/libps5vk.ps5.a` | Vulkan driver | 1bdcfc1a…82a2c |
| `.deps/native/vulkan-runtime/lib/libvk_runtime.ps5.a` | Mesa Vulkan runtime | c6a1d1f2…baa4d |
| `build/driver/ps5/libpsbc_driver.ps5.a` | shader compiler driver side | ea7fa08e…2a2c |
| `.deps/native/psbc/lib/libpsbc_support.ps5.a` | shader compiler support | feaae465…e65e |
| `rcomp-mesa-util/{u_thread,anon_file,os_file}.o` | Mesa util objects (build-mesa-util.sh) | in SHA256SUMS |

No permissive linker option hides a missing symbol (`--error-limit=0`, no
`--unresolved-symbols=ignore-all`). The loader shim `common/rcvk_loader.c`
resolves entry points through `vkGetInstanceProcAddr` of the linked driver.

## What the driver exposes (from its sources and the host model)

Device API 1.0.0, driverVersion 0x2000, single device extension
`VK_KHR_swapchain`; features robustBufferAccess, samplerAnisotropy,
dualSrcBlend (Agent 5, `ps5vk_physical_device.c:51-72`). Presentation path
per the probe: `VK_KHR_surface` + `VK_KHR_display` → display-plane surface →
swapchain.

## Results (2026-09-25)

| Check | Status | Evidence |
| --- | --- | --- |
| Probe + draw test, host, Mesa lavapipe | PASS (HOST ONLY) | 946 inside + 2718 outside pixels exact, 432 edge pixels excluded (1.5 px) |
| Driver built for host with the repo's AGC host model | PASS (build) | `libvulkan_ps5vk.so` |
| Draw test through the host-model driver | recording/submission accepted; **pixels NOT TESTED** | the host model builds and replays AGC streams but executes no GPU work: readback is all zero (expected), so it proves nothing about pixels |
| Same, without `PS5_HOST_REPLAY` | FAIL (expected) | `vkEndCommandBuffer` → -13, "AGC's context defaults lack a colour target register": the model needs a console-captured replay |
| PS5 link of probe and draw test (eboot.bin) | PASS | probe 0aee7b05…f45a, draw f5e92ecc…b872 (build/vulkan-ps5-out/SHA256SUMS) |
| PS5 execution | NOT TESTED | no console in this environment |

## Still to do (lot 4)

- Run both eboots on the console (PRIME, serialised), collect the probe JSON
  and the draw test's verdict.
- Compile Agent 5's PS5-profile SPIR-V (`build/xenos-synth/spv-ps5/`) with
  the driver's shader compiler on the host model; probe Texture3D, MRT, SV_Depth.

## Historical Emu-3 PS5_Vulkan fork (@17350536) — 2026-09-25

This section retains historical measurements. The owner's current R-comp-only
decision in AGENTS.md excludes this fork as a dependency or fallback; these
results do not authorize its reuse.

Script: `gpu/vulkan/ps5/build-emu3-fork.sh` (bundle `PS5_Vulkan.bundle`
sha256 4401390f…7e22, tree identical to Emu-3's `PS5_Vulkan/` folder
@cfb691a2; history distinct from public, which is not an ancestor).
Built through the fork's `make driver` target (6 min 30), without writing
anything in Emu-3.

**Different link contract**: this revision defines `vk_nir_convert_ycbcr`
(`nir_convert_ycbcr_to_rgb`, `nir_vk_lower_ycbcr_tex`) in both
`libvk_runtime.ps5.a` and `libpsbc_driver.ps5.a`. Linking everything with
`--whole-archive` (PS5_RetroArch contract) produces “duplicate symbol.”
The driver's own proven contract (`tools/build-driver.sh:306-313`):
`--whole-archive libps5vk libvk_runtime --no-whole-archive libpsbc_driver
libpsbc_support`. Selection: `RCOMP_VK_LINK_CONTRACT=ps5vk-driver`;
default (`retroarch`) remains the public-driver contract and still links.

| Artifact (fork) | sha256 |
| --- | --- |
| libps5vk.ps5.a | 3480016e…a2e4 |
| libpsbc_driver.ps5.a | 089ea632…95e8 |
| libpsbc_support.ps5.a | 674c347c…2f88 |
| libvulkan_ps5vk.so (host) | c77302ba…0cd7 |
| rcomp_vk_probe/eboot.bin | e4839874…f72b (identical on rebuild) |
| rcomp_vk_draw_test/eboot.bin | 79b7411e…3af1 (identical on rebuild) |

Probe through fork's host driver (AGC host model; **not** PS5 proof):

| | public 9639c418 | fork, default | fork, `PS5VK_EXPERIMENTAL=1` |
| --- | --- | --- | --- |
| device API | 1.0 | 1.0 | **1.1** |
| device extensions | 1 | 1 | 22 (multiview, timeline semaphore, sync2, push descriptor, draw parameters, demote…) |
| independentBlend / imageCubeArray | no / no | no / no | **yes / yes** |
| samplerAnisotropy / dualSrcBlend | yes / yes | no / no | yes / no |
| maxColorAttachments | 4 (but > 1 rejected at draw) | 4 | 8 |
| maxImageDimension2D | 16384 | 4096 | 4096 |
| maxImageDimension3D | 256 | 256 | 256 |

The fork's default profile is **more conservative** than public (features
removed for lack of console proof); improvements useful to Xenos are in
the experimental profile. Draw test through host model: recording and
submission accepted in both profiles, zero pixels (model does not execute
GPU) ⇒ pixels **NOT TESTED**. PS5 execution: **NOT TESTED**.
