# XenosRecomp audit and PS5_Vulkan capability gap (Agent 5)

Status date: 2026-09-25. Owner: Agent 5 (`gpu/xenos/`). Statuses use only
PASS / FAIL / BLOCKED / NOT TESTED. Nothing here is a PS5 proof: every run below
is on the Linux host.

References (read-only, never modified):

| Ref | Pin | Licence |
| --- | --- | --- |
| XenosRecomp | `990d03b28a27b50277ee5d8d942e1c5f873869d1` | MIT |
| its submodules (fetched into `build/` only) | dxc-bin `737ac9f5`, fmt `873670ba`, smol-v `9dd54c37`, xxHash `2bf8313b`, zstd `f7a8bb12` (gitlinks of the pin) | dxc-bin: no LICENSE file in repo (Microsoft DXC binaries, NCSA/Apache-2.0 upstream) -- host tool only, never shipped |
| PS5_Vulkan | `9639c4188393dfe406303fc997ab5ea88a9689d6` | GPL-3.0 |
| rexglue-sdk | `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518` | BSD-3-style + Xenia portions |

Paths below are relative to each checkout; `XR/` = `XenosRecomp/XenosRecomp/`,
`PV/` = PS5_Vulkan root.

---

## 1. What XenosRecomp consumes

- **Input = D3D9-style Xbox 360 shader containers**, big-endian, recognised by
  a magic: `(flags & 0xFFFFFF00) == 0x102A1100`, `field1C == 0`,
  `field20 == 0` (`XR/main.cpp:91-94`). `flags & 1` = vertex shader
  (`XR/shader_recompiler.cpp` `isPixelShader = (flags & 1) == 0`).
- **Container layout** (`XR/shader.h`): header `ShaderContainer` (9 BE words:
  flags, virtualSize, physicalSize, constantTableOffset, definitionTableOffset,
  shaderOffset, ...); a *virtual* part holding the D3DX constant table
  (`XR/constant_table.h`, `D3DXSHADER_CONSTANTTABLE` + `CONSTANTINFO`:
  name, register set Bool/Int4/Float4/Sampler, index, count), an optional
  definition table (literal `def` float4 / int4 constants), the `Shader`
  struct (microcode offset/size, svPos register in `fieldC`, interpolator
  count in `interpolatorInfo`), vertex elements (usage/usageIndex keyed by
  *vfetch instruction address*) and interpolators; then a *physical* part
  = raw Xenos microcode (CF triples + 96-bit ALU/fetch instructions,
  `XR/shader_code.h`).
- **Reflection is mandatory**: `assert(constantTableOffset != NULL)`
  (`XR/shader_recompiler.cpp:1106`); README: "If this data is missing, the
  recompiler will not function."
- **Discovery in a game**: directory mode walks every file recursively and
  scans every 4-byte offset for the magic (`XR/main.cpp:74-117`), i.e. it
  finds containers embedded in the XEX or in uncompressed archives. It needs
  the extracted game files on disk -> **BLOCKED here** (no authorised
  content). Compressed/encrypted archives are not handled.
- **Identity**: XXH3-64 of the whole container (`XR/main.cpp:96`); the
  runtime is expected to hash the container the guest hands to D3D
  (`CreateVertexShader/CreatePixelShader`) and look it up.

## 2. What XenosRecomp produces

- **HLSL** text: prelude = the "shader common header" given on the command
  line (upstream `XR/shader_common.h`) + generated body (single-file mode
  `XenosRecomp in.bin out.hlsl shader_common.h`).
- **Directory mode**: compiles each HLSL with DXC in-process and emits a
  `.cpp` shader cache: `g_shaderCacheEntries[] {hash, dxilOff, dxilSize,
  spirvOff, spirvSize, specConstantsMask}` + zstd(max level) blobs; SPIR-V is
  smol-v encoded before zstd (`XR/main.cpp:139,180-205`). DXIL only on
  Windows (`XENOS_RECOMP_DXIL`).
- **DXC options** (`XR/dxc_compiler.cpp:24-58`), SPIR-V path:
  `-T vs_6_0|ps_6_0 -HV 2021 -all-resources-bound -spirv -fvk-use-dx-layout
  [-fvk-invert-y for VS] -Qstrip_debug [-DUNLEASHED_RECOMP]`. No
  `-fspv-target-env`, so DXC's default applies: **SPIR-V 1.0 / vulkan1.0**
  (measured: `; Version: 1.0` in every module, section 5).
- DXC used: dxc-bin@737ac9f5 `bin/x64/dxc-linux`, reports
  `libdxcompiler.so: 1.8(dev;4662-416fab6b)`, sha256
  `e48ba27c7e989726d1219b0b900b15eb1b99b669faa3905da3ffba46c52306a9`.

## 3. Runtime contract assumed by the generated SPIR-V (upstream)

| Area | Contract | Evidence |
| --- | --- | --- |
| Constant buffers | Push-constant block of **three `uint64_t` GPU addresses** (VS consts, PS consts, shared); every constant read is `vk::RawBufferLoad` (BDA, `PhysicalStorageBuffer64`). VS file 256 x float4 (4096 B), PS file 224 x float4 (3584 B), little-endian (runtime swaps) | `XR/shader_common.h:20-32`, `XR/shader_recompiler.cpp:1157,1162`; README "Constants" |
| Dynamic constant index | `NAME(INDEX)` macro: `select(INDEX < tail, load(base + min(INDEX, tail-1)), 0)` -- clamps to the end of the register file, OOB -> 0 | `XR/shader_recompiler.cpp:1152-1158` |
| Bool constants | `g_Booleans` u32 in shared buffer @256: VS bits 0-15, PS bits 16-31 (only 16 each; hw has 128) | `XR/shader_common.h:29`, `XR/shader_recompiler.cpp:1257` |
| Int constants | **not implemented** as runtime constants; only literal `def` int4 (loop counts) from the definition table | README; `XR/shader_recompiler.cpp:1390-1413` |
| Textures/samplers | **Bindless**: unbounded arrays `Texture2D[] (space0)`, `Texture3D[] (space1)`, `TextureCube[] (space2)`, `SamplerState[] (space3)`, indexed by per-sampler descriptor indices read from the shared buffer (offset `dim*64 + reg*4`, samplers at `192 + reg*4`) | `XR/shader_common.h:50-53`, `XR/shader_recompiler.cpp:1172-1182` |
| Texture features | 2D/3D/Cube `Sample()` only; pixel offset supported; **no LOD/grad/bias, filter overrides, border, 1D, `tfetch` with computed LOD, weights only for 2D**; cube via local direction array (`cube` instr stores direction) | README "Textures & Samplers"; `XR/shader_recompiler.cpp:226-361` |
| Vertex fetch | **No fetch emulation**: each `vfetch` becomes a read of a *native vertex input* chosen from the container's vertex element table; vfetch `format/stride/offset/expAdjust/isMiniFetch` fields are **ignored** (0 references). Runtime must build Vulkan vertex input state from the D3D vertex declaration and **endian-swap vertex buffers as 32-bit words**; 16-bit TEXCOORDs then come out YXWZ, fixed by `g_SwappedTexcoords` bits; `NORMAL/TANGENT/BINORMAL` forced to `uint4` and decoded as R11G11B10 under spec bit 0 | `XR/shader_recompiler.cpp:161-224`, `:16-31` (USAGE_TYPES) |
| Vertex locations | Fixed Unleashed table (`POSITION0->0 ... TEXCOORD7->15, POSITION1->15`); other usages get no explicit location | `XR/shader_recompiler.cpp:77-95` |
| Interpolators | VS always writes all 16 TEXCOORD + 2 COLOR outputs; PS reads all 18 | `XR/shader_recompiler.cpp:98-118,1277-1356` |
| Spec constants | `[[vk::constant_id(0)]] g_SpecConstants`: bit0 R11G11B10 normals, bit1 alpha test (Unleashed adds bicubic GI, A2C, reverse-Z) | `XR/shader_common.h:4-11,34` |
| Alpha test | Only one comparison: `clip(oC0.w - g_AlphaThreshold)` (== GREATEREQUAL) on `ExecEnd`; other D3D alpha funcs unimplemented | `XR/shader_recompiler.cpp:1799-1809` |
| Depth / blend / stencil / cull | **Not in the shader** -- fixed-function state the runtime must translate to Vulkan pipeline state. `oDepth` (SV_Depth) export supported | `XR/shader_recompiler.cpp:1299-1300` |
| Half-pixel offset | VS: `oPos.xy += g_HalfPixelOffset * oPos.w` at every VS exit (runtime supplies the D3D9 -> Vulkan correction); `-fvk-invert-y` flips Y in the VS | `XR/shader_recompiler.cpp:1835,1884`; `XR/dxc_compiler.cpp:44-45` |
| VPOS / face | svPos register = `(iPos.xy - 0.5) * (face ? 1 : -1, 1)` (D3D9 VPOS convention) | `XR/shader_recompiler.cpp:1465-1468` |
| Control flow | flattened when only forward conditional jumps; else `while(true) switch(pc)`; loops only via `def` int constants; predicates `p0`; `a0`/`aL` | README; `XR/shader_recompiler.cpp:1508-1760` |

**Sonic Unleashed-specific assumptions** (under `#ifdef UNLEASHED_RECOMP`,
off in our builds but they shape the design): constant names
`g_MtxProjection` (reverse-Z double pass), `g_InstanceTypes`, `g_IndexCount`
(instancing via index buffer as vertex stream + `SV_VertexID/InstanceID`),
`g_MtxPrevInvViewProjection` + `sampZBuffer` (1 - depth), sampler register 10
= GI bicubic, texture register 0 = pixel coord for A2C
(`XR/shader_recompiler.cpp:251-293,1118-1146,1340-1371,1752-1780`). Generic
but Unleashed-shaped even without the macro: vertex location table, forced
`uint4` usages, `g_SwappedTexcoords` TEXCOORD-only fix, R11G11B10 only for
normal/tangent/binormal, 16-bit bool masks.

**Defects noticed (upstream, not fixed there)**: `CondExecPredCleanEnd` is not
treated as an end (`XR/shader_recompiler.cpp:1633` compares `CondExecEnd`
twice); generic (non-Unleashed) VS never zero-initialises `oPos`
(`:1448-1451` is under `UNLEASHED_RECOMP`); D3D-path array macro indexes past
the declared array (`g_Mtx[min(INDEX, 251)]` on a 4-element array,
`:1214`). Memory export, point size, mini-fetch, vfetch bindings,
dynamic register indexing (`vectorDestRelative` has 0 uses) are unimplemented
(README "Other Unimplemented Features").

## 4. What XenosRecomp does NOT provide

Command submission / ring buffer / PM4; any D3D or Vd* HLE; resource creation
and memory management; texture **untiling, endian swap and format conversion**
(Xenos textures are tiled, big-endian; DXT/DXN/CTX1 need decoding: PS5_Vulkan
leaves every optional feature off, so no `textureCompressionBC`,
`PV/driver/ps5vk_physical_device.c:51-67`); vertex-buffer endian swap and
vertex declaration -> Vulkan input state; resolves (eDRAM -> texture copies),
**eDRAM semantics** (tile layout, MSAA, formats like 7e3/2_10_10_10_FLOAT,
gamma PWL), render-target management and aliasing; clears; fixed-function
state (blend, depth, stencil, cull, scissor, viewport, alpha func other than
>=, alpha-to-coverage); present/VdSwap; shader discovery at runtime (only the
hash table). In short: **it is a shader translator plus a cache format; the
whole GPU runtime is ours.**

## 5. Measured SPIR-V requirements (host, self-authored inputs only)

Because there is no Xbox 360 shader in this environment, I authored
**synthetic containers** from XenosRecomp's own struct definitions
(`gpu/xenos/tools/synth_xenos.py`; not derived from any game/SDK binary): a
textured PS (tfetch2D + float4 const), a PS with a bool-constant branch, a VS
with vfetch POSITION0 + 4x dp4 with a 4-register matrix + interpolator export.
They were translated by the real XenosRecomp build, compiled with the real
DXC and XenosRecomp's exact flags, disassembled and validated
(`spirv-val` = SPIRV-Tools v2025.1, Ubuntu `2025.1~rc1-1~ubuntu0.24.04.2`,
already installed).

Upstream (generic build, upstream `shader_common.h`) -- all 3 modules:

```
; Version: 1.0
OpCapability Shader | Int64 | RuntimeDescriptorArray | PhysicalStorageBufferAddresses (+ ImageQuery in the textured PS)
OpExtension "SPV_EXT_descriptor_indexing" | "SPV_KHR_physical_storage_buffer"
OpMemoryModel PhysicalStorageBuffer64 GLSL450
PushConstant block (3 x u64); SpecId 0; bindless heaps at (set 0,b0) and (set 3,b0) with non-constant OpAccessChain
spirv-val --target-env vulkan1.0 -> rc 0   (!)
```

`spirv-val` accepts them for vulkan1.0 because the extensions are *declared*;
it cannot know the device does not expose them. **spirv-val alone is not a
sufficient gate**; `tools/shader_manifest.py` adds the device gate.

## 6. Capability gap table vs PS5_Vulkan@9639c41

PS5_Vulkan facts: device API **1.0** (`PV/driver/ps5vk_private.h:62`); features
only `robustBufferAccess`, `samplerAnisotropy`, `dualSrcBlend`
(`PV/driver/ps5vk_physical_device.c:51-67`); device extensions only
`VK_KHR_swapchain` (`:69-72`); README "Buffer device address is not
advertised ... refused by name" (`PV/README.md:140`); pipeline creation
**refuses** addressing model `PhysicalStorageBuffer64` and capability
`PhysicalStorageBufferAddressesEXT` before compiling
(`PV/driver/ps5vk_pipeline.c:606-619,674-697`); probe shaders are built
`glslang --target-env vulkan1.0` (`PV/tools/build-probe-shaders.sh:811`) and
the SPIR-V 1.5 -> 1.0 regeneration is recorded in `PV/docs/M5_PHASE_C.md:1148-1190`.
Limits: push constants 128 B, UBO range 16384, 4 sets, 16 sampled images /
16 samplers / 12 UBOs / 4 SSBOs per stage, 16 vertex attributes, 4 colour
attachments advertised (`PV/driver/ps5vk_physical_device.c:99-125,171`).

| Requirement (upstream output) | Source | PS5_Vulkan | Adaptation (implemented = I, planned = P) |
| --- | --- | --- | --- |
| SPIR-V 1.0, vulkan1.0 | DXC default | **OK** (device 1.0) | keep; gate with `spirv-val --target-env vulkan1.0` (I) |
| `OpMemoryModel PhysicalStorageBuffer64` + `PhysicalStorageBufferAddresses` + `SPV_KHR_physical_storage_buffer` (vk::RawBufferLoad) | `shader_common.h:29-32`, generator `:1157-1180` | **NO** -- refused at pipeline creation | constants become **UBOs at fixed bindings**: set 0 b0 = VS register file (256 x float4 = 4096 B), b1 = PS file (224 x float4 = 3584 B), b2 = shared (32 B); names map to `g_*ShaderConstants[N]` with the same clamp/zero rule (I) |
| `Int64` (u64 push-constant addresses, `1ull` literal) | `shader_common.h:22-24,180` | **NO** (`shaderInt64` off) | no push-constant addresses; `1ull` -> `1u` in our prelude (I) |
| `RuntimeDescriptorArray` + `SPV_EXT_descriptor_indexing` (unbounded heaps) | `shader_common.h:50-53` | **NO** (no descriptor indexing) | **fixed slots**: per (fetch constant, dimension) one `Texture*` + one `SamplerState`; PS -> set 1, VS -> set 2, binding `2*slot` / `2*slot+1`; generator aborts if a slot is fetched with two dimensions (I) |
| Non-constant index into descriptor arrays | heap indexing | **NO** (`shaderSampledImageArrayDynamicIndexing` off) | removed by fixed slots; detected structurally by the gate because DXC declares no capability for it (I) |
| Push constants | 24 B block | OK in general (<=128 B; `v0-push-constant` proof) | not needed any more (I) |
| Spec constant id 0 | `shader_common.h:34` | **OK** (R9 / `v0-r9` proof per README) | keep (I) |
| `ImageQuery` (GetDimensions for offsets/weights) | `tfetch2D` | **OK** -- listed as lowered, `PV/driver/ps5vk_pipeline.c:594-598` | keep (I) |
| UBO arrays indexed at run time (`c[a0+N]`) | const macros | **OK** after R62 (`PV/jobs/r62-uniform-index/README.md`) | keep (I) |
| Separate `Texture` + `SamplerState` | common | **OK** (SAMPLER / SAMPLED_IMAGE descriptor types, R2, `PV/driver/ps5vk_descriptor_set_layout.c:49-61`) | keep (I); combined image samplers are the fallback |
| Texture3D | `tfetch3D` | **refused at draw** (`PV/driver/ps5vk_draw.c:1341-1350`) | gate flags it (I); needs a driver probe (Agent 4) or runtime fallback (P) |
| Texture1D | unimplemented upstream | refused at draw (same lines) | map to 2D Nx1 in generator (P) |
| >1 colour output (`oC1..oC3`) | PS outputs | **refused at draw**: v0-mrt writes do not land (`PV/driver/ps5vk_draw.c:947-955`) | gate flags it (I); MRT titles BLOCKED on Agent 4/driver; interim: one pass per target (P) |
| `SV_Depth` export | `oDepth` | NOT TESTED on PS5_Vulkan | Agent 4 probe (P) |
| `clip` / OpKill, `SV_IsFrontFace`, `SV_Position` input | many | core 1.0; FragCoord probe `PV/jobs/r59-fragcoord` exists | keep; NOT TESTED end-to-end |
| <= 16 vertex inputs, explicit locations | location table | OK (`maxVertexInputAttributes = 16`) | keep table for now; P: locations from vfetch slots (see BOUNDARY_OPTIONS.md) |
| VS writes 18 interpolators (72 components) | signature | **limit risk**: `maxVertexOutputComponents = 64` (`PV/driver/ps5vk_physical_device.c:129`) | P: emit only interpolators the pair uses (generator change) -- NOT TESTED against the PS5 compiler |
| BC/DXT textures, float16/int16, geometry/tess, clip distance | not generated by XenosRecomp shaders | off | runtime decodes DXT to RGBA8 (P) |

**Adaptation implemented (prototype):** `gpu/xenos/patches/0001-xenosrecomp-ps5-profile.patch`
(generator, compile-time `XENOS_RECOMP_PS5`, 163 lines, sha256
`b8a632f1...a22e`) + `gpu/xenos/prelude/shader_common_ps5.h` (prelude,
sha256 `fa76f969...c1a6`). No SPIR-V is edited, no capability is removed by
hand: the generator emits a different, fully legal program. Result on the same
synthetic containers: `; Version: 1.0`, `OpMemoryModel Logical GLSL450`,
capabilities `Shader` (+ `ImageQuery` for the textured PS), **no extensions,
no push constants**, bindings as designed; `spirv-val --target-env vulkan1.0`
rc 0; PS5 driver-refusal gate 0; feature gate 0.

**Not yet known (NOT TESTED):** whether PS5_Vulkan's own compiler
(opengnm-psbc NIR/ACO fork) accepts these modules. That needs Agent 4's host
driver/compiler build; the `.spv` files in `build/xenos-synth/spv-ps5/` are
ready to hand over.

## 7. Tooling (`gpu/xenos/tools/`)

- `synth_xenos.py OUT` -- writes 3 SYNTHETIC containers (parser fixtures only).
- `shader_manifest.py --corpus DIR --out DIR --dxc ... [--check-expectations]`
  -- per shader: source sha256, stage, exact DXC args, SPIR-V sha256/version,
  capabilities, extensions, memory model, bindings, push constants, spec ids,
  image dims, colour outputs, `spirv-val --target-env vulkan1.0` result, and
  three gates (driver refusal, unexposed feature/extension, draw-time
  refusal). `--corpus-kind xenos-real` exits 2 with BLOCKED by design.
- `selftest/*.hlsl` -- 6 self-authored shaders with `// rcomp-expect:`.
- `run_xenos_checks.sh` -- end-to-end reproduction (clone into `build/`,
  submodules at gitlink SHAs, upstream + PS5-profile builds, synth, translate,
  gate, self-test). Last run: rc 0 (upstream gate rc 1 as expected; PS5 profile
  rc 0; self-test 6/6 match).

Real Xenos shader corpus: **BLOCKED** (no authorised content; nothing
downloaded).
