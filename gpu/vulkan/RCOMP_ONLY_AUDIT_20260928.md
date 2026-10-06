# Lot 4 audit — R-comp-only graphics path — 2026-09-28

Overall status: **BLOCKED** for a complete PS5 graphics-title link;
**PASS** for the local object compilations described below; console
execution **NOT TESTED** in this slice.

This audit applies the 28 September 2026 decision in `AGENTS.md`: R-comp
is the sole working base. File
`build/prime-calendar-resume-20260928/gpu-bundle/PS5_Vulkan.bundle` is
excluded and was neither extracted, compiled, linked nor used as an oracle.
No Emu-3 content, source or configuration is an input to this slice.

## Actual graphics path in R-comp

The game shell links `rcomp_app`, which links `rcomp_runtime_video` and
`rcomp_xenos_vulkan` (`app/CMakeLists.txt`). Xenos Vulkan backend is the
rexglue copy/adaptation under `gpu/xenos/rexglue/`; on PS5 it links no
system loader and expects the title to supply `vkGetInstanceProcAddr`.

The current PS5 path is therefore:

`Title/rcomp_app -> rcomp_xenos_vulkan -> vkGetInstanceProcAddr -> static Vulkan driver`.

`gpu/vulkan/common/rcvk_loader.c` follows the same contract for the probe
and draw test: `vkGetInstanceProcAddr` is the only Vulkan symbol linked
by name; other PFNs resolved through `vkGetInstanceProcAddr` then
`vkGetDeviceProcAddr`.

`gpu/vulkan/ps5/build-ps5.sh` currently knows how to link the historical
`retroarch` contract for public PS5_Vulkan `9639c418`, provided a coherent
driver snapshot exists. It also contains the `ps5vk-driver` contract,
title shims and `PS5VK_EXPERIMENTAL` profile, all from the Emu-3 period.
Xenos, graphics XEX and M6 targets are currently blocked by the name of
this historical contract (`RCOMP_VK_LINK_CONTRACT=ps5vk-driver`).
This condition must not simply be removed: it encoded graphics capabilities
the Xenos backend needed. The next configuration must measure these
capabilities with the selected driver, then express a capability gate
independent of the old profile name.

## Inputs actually present and usable without Emu-3

Locally present and verified in R-comp:

- ps5-payload-sdk v0.42 under
  `build/platform-ps5-deps/sdk/ps5-payload-sdk`, local archive pinned by
  `deps/deps.lock` SHA-256;
- public PS5_RetroArch `18dc1059...`, local archive and working copy under
  `build/platform-ps5-deps/`, with `tooling/native`, `tooling/ps5-stubs`
  and `tooling/prospero-clang18`;
- valid RetroArch link snapshot under
  `build/vulkan-m4-20260928/retroarch-link/` (valid `MANIFEST.json`,20
  files, hashes retained);
- Vulkan-Headers `e3b1eec...` under `build/prime-vulkan-headers/`;
- host zlib1.3.2 under `build/platform-ps5-deps/zlib/`;
- host ELF/SELF tool `build/vulkan-m4-20260928/ps5-native-tool.exe`;
- `libSceAgc.so` and `libSceAgcDriver.so` import stubs built from the
  public RetroArch snapshot;
- PS5 objects already compiled in `build/vulkan-m4-20260928/`: loader,
  probe, draw test and wrappers. Previous M4 compilation **PASS**.

This slice also compiled `gpu/vulkan/ps5/radv_icd_bridge.c` with the
PS5 toolchain and R-comp headers: **PASS**, output
`build/vulkan-calendar-audit-20260928/radv_icd_bridge.o`.
`llvm-nm` gives exactly:

```text
T vkGetInstanceProcAddr
U vk_icdGetInstanceProcAddr
```

The bridge is therefore bounded correctly: it supplies R-comp's expected
entry point and requires the real RADV backend to supply
`vk_icdGetInstanceProcAddr`.

Not present: no `build/vulkan-ps5-artifacts/driver/MANIFEST.json` or
compatible driver snapshot defined in R-comp, none of the four
PS5_Vulkan/Mesa archives expected by the historical contract, and no
coherent public RADV PS5_Vulkan/PS5_Mesa/SDK set ready for linking.
Old `tests-xenos-cp-ps5` / `tests-xex-gfx-ps5` folders are not present
in the current local state; historical existence must not be used as
evidence for this slice.

## Current blocker

The first blocker is no longer “missing Emu-3 bundle.” That bundle is
explicitly excluded. The blocker is **absence of a compatible driver
snapshot defined and reproducible from R-comp, with its full link contract**.

Two paths documented in R-comp, neither currently complete:

1. Old public PS5_Vulkan `9639c418` has a snapshot/link recipe in
   `LINK_CONTRACT.md`, but the local driver snapshot is missing.
   This backend exposed a limited Vulkan profile and the current
   Xenos gate does not accept it.
2. Newer public backend observed during the Mihawk audit is RADV/ACO.
   R-comp has only the `radv_icd_bridge.c` adapter. The public contract
   described in `docs/HOST_INTEGRATION_20260928.md` and
   `docs/GTA_IV_BOOT_AUDIT.md` requires together the RADV archive under
   `--whole-archive`, `libps5platform`, heap/stdio/pthread wrappers,
   a2 MiB stack, host TLS `__emutls_get_address`, C++/unwind runtimes
   and associated link script. `build-ps5.sh` does not yet implement it.

The TLS issue is concrete with the local toolchain: compiler-rt archive
`libclang_rt.builtins-x86_64.a` searched through Cygwin clang22 is absent.
`platform/ps5/README.md` also documents that the current SDK forces
`-femulated-tls` and no SDK stub supplies `__emutls_get_address`.
A RADV link must therefore supply and verify this runtime as a whole;
the bridge must not invent this symbol.

Public commits mentioned in R-comp must also be distinguished before
selecting a pin: `71026e7` is the public snapshot consulted for the audit,
while the documented release cites PS5_Vulkan `5d8f37d`, PS5_Mesa `cedb774`
and SDK `95c08f2`. Announced Vulkan version alone does not select a
compatible set.

## Recommended next R-comp step

Create a dedicated slice, initially without console, defining **one**
public driver snapshot from R-comp-recorded pins and reproducing its full
contract. For the already-documented public RADV candidate, the slice must:

1. Explicitly select the PS5_Vulkan/PS5_Mesa/SDK set matching the actual
   public binary targeted, then record pins and hashes in R-comp.
2. Produce a manifested snapshot of archives, headers, `libps5platform`,
   wrappers and TLS/unwind runtime under `build/vulkan-*`, without
   modifying reference checkouts.
3. Then add an explicit RADV link contract to the R-comp recipe that
   compiles `radv_icd_bridge.c` with `RCOMP_VK_RADV` and links **only** RADV.
4. First pass the Vulkan probe and draw test, then measure capabilities
   actually required by `rcomp_xenos_vulkan` (particularly formats, depth,
   3D/cube textures, color attachment count, independent blend, resolved
   functions/versions and presentation).
5. Only after these measurements replace the historical
   `ps5vk-driver/PS5VK_EXPERIMENTAL` gate with an explicit capability gate.
   A missing capability remains **BLOCKED**; no fallback may hide failure.

This is a new explicit R-comp contract, not an implicit migration and
not fallback to a second driver.

## Historical references to annotate before a future graphics resumption

The following files still contain instructions or statuses that could
suggest Emu-3 is an active dependency. They must be treated as historical
during a future editing slice:

- `gpu/vulkan/LINK_CONTRACT.md`: Emu-3 fork section and `ps5vk-driver` selection;
- `gpu/vulkan/ps5/build-emu3-fork.sh`: entire recipe now excluded;
- `gpu/vulkan/ps5/build-ps5.sh`: comments, experimental variant and
  Xenos/M6 gates by contract name;
- `gpu/vulkan/ps5/ps5vk_title_shims.c` and
  `gpu/vulkan/common/rcomp_title_wrap.c`: fork-specific
  `PS5VK_EXPERIMENTAL`/shim behavior;
- `tools/ps5_test_kit.sh` and `tools/m6_build_title.sh`:
  `vulkan-emu3-*` paths, fork build and `ps5vk-driver` contract;
- Emu-3 fork lines in `deps/deps.lock`;
- `docs/STATUS.md`, `docs/REPO_SURVEY.md`, `docs/PS5_TEST_KIT.md`,
  `docs/SESSION_RUNTIME_CPU_20260928.md`, `docs/SESSION_PS5_GTAIV_20260928.md`
  and historical passages in `docs/GTA_IV_BOOT_AUDIT.md`.

`AGENTS.md` already contains the owner decision and governs this resumption.

## Validation of this slice

- Integration path, contract and script reading: **PASS**.
- Bounded local R-comp input inventory: **PASS**.
- PS5 object compilation of RADV bridge: **PASS**.
- Compatible R-comp driver snapshot: **BLOCKED** (absent/undefined).
- Complete Vulkan title link: **BLOCKED** by driver snapshot/contract,
  before any execution.
- Console execution: **NOT TESTED**.

No code, pin, SDK, shared checkout or dependency modified and no driver
downloaded in this slice.
