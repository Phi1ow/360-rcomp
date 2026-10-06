# Graphics boundary for the first integrated slice (M5)

Owner: Agent 5. Decision owner: PRIME. Statuses: PASS / FAIL / BLOCKED / NOT
TESTED. Constraints that shape everything below (AGENTS.md): no global Xenia
port, no second Vulkan driver, no D3D12, no home-made AGC/PM4 *backend* on the
PS5 side, rendering only through PS5_Vulkan, no game content in the repo or
in this environment.

## Background facts (with evidence)

- On Xbox 360 the Direct3D runtime is a **static library linked into each
  title**; the kernel only exposes the video/ring-buffer layer (`Vd*`
  exports). rexglue-sdk@c94f5eb registers exactly these: `VdInitializeRingBuffer`,
  `VdEnableRingBufferRPtrWriteBack`, `VdGetSystemCommandBuffer`, `VdSwap`, ...
  (`src/kernel/xboxkrnl/xboxkrnl_video.cpp:328-360,440-584`). Guest D3D code
  writes PM4 packets into the ring buffer; `VdSwap` appends the swap packet.
- The packet vocabulary a D3D title emits is small and known: rexglue's (Xenia-
  derived) processor dispatches `ME_INIT, NOP, INTERRUPT, XE_SWAP,
  INDIRECT_BUFFER(_PFD), WAIT_REG_MEM, REG_RMW, REG_TO_MEM, MEM_WRITE,
  COND_WRITE, EVENT_WRITE(_SHD/_EXT/_ZPD), DRAW_INDX(_2), SET_CONSTANT(2),
  LOAD_ALU_CONSTANT, SET_SHADER_CONSTANTS, IM_LOAD(_IMMEDIATE),
  INVALIDATE_STATE, VIZ_QUERY, SET_BIN_*, CONTEXT_UPDATE, WAIT_FOR_IDLE`
  (`src/graphics/command_processor.cpp:749-873`). Reference only: we do not
  port that processor (26 kLOC of Xenia GPU in `src/graphics/`).
- XenosRecomp was built for the **opposite** boundary: UnleashedRecomp hooks
  the title's D3D functions ("implements a translation layer for the renderer
  rather than emulating the Xbox 360 GPU", XenosRecomp README) and hands the
  shader *container* (with D3DX constant-table reflection) to a hash lookup.
  See XENOSRECOMP_AUDIT.md sections 1-4.

## Option A -- HLE/replacement of identified guest D3D calls

**What the first slice needs**
1. A per-title list of D3D function addresses (Create*Shader, SetVertexShader/
   PixelShader, SetVertexDeclaration, SetStreamSource, SetIndices, SetTexture,
   SetSamplerState, SetRenderState, Set*ShaderConstantF/B, DrawIndexedVertices,
   Clear, Resolve, Swap/Present) and their guest struct layouts
   (`D3DDevice`, `D3DVertexShader`, `D3DTexture`, `D3DVertexDeclaration`).
2. XenonRecomp config to redirect those functions to host implementations
   (Agent 1: function replacement hooks; Agent 3: runtime side).
3. Shader cache from the title's files (XenosRecomp directory mode) keyed by
   container hash; our PS5 profile patch for the SPIR-V.
4. Host D3D-state -> Vulkan translation (Agent 4 owns the Vulkan calls).

**Verifiable without game content**: only the *plumbing* -- a PPC test
program we write that calls functions of *our own* invented API at known
addresses, which get replaced; shader side via synthetic containers
(`tools/synth_xenos.py`). Nothing about real D3D struct layouts, calling
patterns or the shader hash contract can be checked: those are the XDK's and
the title's -> BLOCKED.

**Risks**: the boundary is title- and XDK-version-specific (function
identification needs the title binary -- BLOCKED here; inlined/partially
inlined D3D helpers bypass hooks; the guest D3D library also writes PM4
itself on some paths, e.g. recorded/replayed command buffers, so a missed
path draws nothing -- not verified here, NOT TESTED); every title repeats the reverse-engineering; guest struct
layouts are XDK-private. Upside: best performance, no eDRAM emulation,
proven by UnleashedRecomp for one title, and the XenosRecomp contract fits
without changes to its input side.

## Option B -- documented subset of Xenos PM4 interpreted by our runtime

**What the first slice needs**
1. Agent 3: `Vd*` exports (`VdInitializeRingBuffer`,
   `VdEnableRingBufferRPtrWriteBack`, `VdSwap`, `VdGetSystemCommandBuffer`,
   ...) with real semantics (ring base/size, read-pointer write-back, swap).
2. Ours: a PM4 **whitelist** interpreter on the host CPU (not a GPU backend):
   type-0 register writes, type-3 `SET_CONSTANT(2)`, `LOAD_ALU_CONSTANT`,
   `IM_LOAD_IMMEDIATE`/`IM_LOAD`, `DRAW_INDX_2` (auto/immediate indices) and
   `DRAW_INDX` (index buffer), `EVENT_WRITE*`, `WAIT_REG_MEM`, `MEM_WRITE`,
   `INDIRECT_BUFFER`, `XE_SWAP`, `NOP`/`ME_INIT`. **Unknown packet or
   register write in a used range = hard FAIL with its opcode, never ignored.**
3. Register subset for one draw: shader program control, VGT primitive type,
   PA viewport/scissor/`PA_SU_VTX_CNTL` (pixel center), RB surface/colour/
   depth info, blend/depth control; the 512 x float4 ALU constant file and the
   fetch-constant file (texture + vertex fetch constants).
4. Shaders arrive as **raw microcode** via `IM_LOAD*` without the container
   and without D3DX reflection. XenosRecomp needs a generator change:
   translate microcode + register-derived stage info, name constants
   `c<N>` (our PS5 profile already maps every constant to the register-file
   UBO, so names are cosmetic), and derive vertex inputs from each `vfetch`
   (fetch-constant slot, offset, stride, format are in the instruction) instead
   of a vertex declaration. Offline cache keyed by microcode hash; runtime
   miss = hard FAIL listing the hash (no JIT, no runtime shader compiler
   beyond PS5_Vulkan's own SPIR-V compile).
5. Minimum eDRAM model for the slice: one colour surface + optional depth,
   resolve-to-texture/front buffer via the copy registers + `EVENT_WRITE`,
   swap. Textures: linear and tiled 2D 8888 only, endian per fetch constant.

**Verifiable without game content**: yes, end to end on the host. We write a
PPC test program (Agent 6 fixture) that: initialises the ring buffer via
`VdInitializeRingBuffer`; writes PM4 to set a render target, viewport and
constants; `IM_LOAD_IMMEDIATE` of **synthetic microcode produced by our own
encoder** (the same encoder as `tools/synth_xenos.py`, emitting raw microcode
instead of a container); a vertex fetch constant pointing at a big-endian
vertex buffer in guest memory; `DRAW_INDX_2` of one triangle; a resolve;
`VdSwap`. Oracle: exact pixels of the resolved surface (host Vulkan readback
first, then PS5_Vulkan console run by PRIME). Every packet and register is
our own documented input, so the test is deterministic and license-clean.

**Risks**: scope creep toward a Xenia GPU port (forbidden) -- mitigated by the
whitelist + FAIL-on-unknown rule, a written register subset, and clean-room
code with Xenia/rexglue used only as behavioural reference (BSD-3 attribution
if any table is taken); eDRAM/resolve/tiling semantics are the hard part and
grow with each title; per-draw CPU cost of register-state -> Vulkan pipeline
hashing; synchronisation with guest (`WAIT_REG_MEM`, read-pointer write-back,
fences in guest memory) must be exact or titles hang. The synthetic test
proves our reading of the packets, not that real D3D emits exactly that
subset (real-title coverage stays BLOCKED until an authorised title exists).

## Comparison

| Criterion | A: D3D HLE | B: PM4 subset |
| --- | --- | --- |
| Title-agnostic | no (per title/XDK) | yes (hardware interface) |
| First slice verifiable here | plumbing only; D3D contract BLOCKED | fully, with self-authored PPC + microcode |
| Needs game data to even start | yes (function identification) | no |
| Shader path | XenosRecomp as is (+ PS5 profile) | XenosRecomp + microcode/vfetch generator changes |
| eDRAM / resolve / tiling work | small (D3D-level resources) | required (minimal model first) |
| Performance ceiling | highest | lower (state reconstruction) |
| Forbidden-scope risk | low | medium (must not become a Xenia port) |

## Recommendation for PRIME

**Adopt B (documented PM4 subset) as the M5 boundary**, with A kept as an
optional per-title fast path later. Reasons:
1. It is the only option whose first slice can be *proven* in this project
   today: all inputs (PPC program, PM4 stream, microcode, vertex data) are
   ours, so PASS/FAIL is meaningful without game content; A's contract is
   BLOCKED until a title binary is available.
2. The interface is fixed by the hardware and the kernel (`Vd*`), so it is
   shared by every title; A must be redone per title/XDK version.
3. Our shader adaptation already fits it: the PS5 profile moves all constants
   into register-file UBOs and uses fixed fetch-constant slots, which is
   exactly how PM4 delivers state (constant file + fetch constants), and it
   removes BDA/Int64/bindless that PS5_Vulkan cannot run
   (XENOSRECOMP_AUDIT.md section 6).
4. The forbidden-scope risk is controllable by rule (whitelist, FAIL on
   unknown, written register subset, clean-room), whereas A's risk (silent
   missed D3D paths) is not visible without the title.

Proposed first-slice acceptance (for PRIME to schedule): PPC fixture draws one
flat-coloured then one textured triangle through `Vd*` + PM4 whitelist ->
XenosRecomp(PS5 profile) SPIR-V -> PS5_Vulkan; host readback exact; then one
console run by PRIME. Interfaces to propose in `include/rcomp/` (PRIME):
guest-memory accessor for the ring buffer, `Vd*` -> GPU-runtime callback
(Agent 3 -> Agent 5), and the per-draw state struct handed to Agent 4's Vulkan
layer. Current status of B slice: NOT TESTED (no code yet).
