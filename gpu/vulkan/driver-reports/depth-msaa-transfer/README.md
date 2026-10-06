# Original MSAA depth/stencil transfer probe

Status: PASS build/shaders/host oracle and PASS PS5 isolated execution on
2026-09-30 (phase18, launched by PRIME after phase17). The ten GPU checkpoints
checked 26,214,400 sample records with zero mismatches; the actual color composite
matched red256/green256/blue208 exactly. See [PS5_RESULTS.md](PS5_RESULTS.md) for
provenance and hashes. Correctness of the title rendering chain and the cause of
its visual defect remain NOT TESTED by this isolated probe. No RADV defect is
established. It has no game asset or game shader. It uses the
current R-comp public RADV static archive, native platform runtime and title
shell. Only PRIME launches it, serially with every other console title.

The exact observed contract comes from
`build/xenos-render-audit-20260930/stage13-depth-reproducer.md` and
`stage13-structural-analysis.json`. Source D32_SFLOAT_S8_UINT is 640x1024/4x;
destination same format is 1280x1024/2x. F24 is the guest format only; this path
copies backing float32 without a conversion. For each destination sample:
`sourceX=x>>1; sourceY=y; sourceSample=(x&1)|((sample^1)<<1)`.

The probe requires standardSampleLocations, sampleRateShading, MSAA4/2 for
depth/stencil, MSAA2 for color, sampled D32S8, stencil export, a graphics+compute
queue and a storage buffer range of 41943040 bytes. Unsupported features report
BLOCKED with actual capabilities and return status2; nothing is simulated.
API failures return FAIL/status1. A submitted-error return keeps resources alive
until the native shell is closed. Normal PASS/FAIL returns reach its end marker.

It fills every source pixel/sample with exactly representable depths
0.125..0.5625 and independent stencil tags, verifies source4x, then verifies:

1. Patterned import of rect1280x256 into depth.75/stencilA5 sentinels, including
   every pixel outside the rectangle.
2. Equal source/destination depth with different stencil, rect1280x208:
   transfer NOT_EQUAL fails depth but stencil depthFail=REPLACE still imports.
3. Constant .25/stencil3 import into the first tile, read before any draw/clear.
4. Original overscan quad with exact NDC scales/offsets and native viewports
   720/464/208, scissors256/256/208, GEQUAL6, stencilALWAYS7/passREPLACE/zfailKEEP.
   It reads every sample before each clear, then clears depth0/stencil0/color0
   and reads every sample again. The overscan geometry avoids edge coverage
   ambiguity; this case does not test interpolation of sloped title triangles.

Transfer uses full sample shading, FragDepth plus stencil export, depth
NOT_EQUAL/write, stencilALWAYS/passREPLACE/depthFailREPLACE. Source/destination
are separate images. Two immutable descriptor sets avoid rewriting descriptors
of commands recorded earlier in the same submission. Explicit image/memory
barriers, a real fence and mapped invalidation guard every readback.

Compute reads depth and stencil through separate multisample views and reads
both color samples. Every output record is poisoned before dispatch. Every
checkpoint checks all2621440 samples, bitwise float32, stencil8, RGBA and the
record write marker. Logs contain observed/expected FNV64 hashes plus the first
sample and at most16 mismatches. A single-sample color resolve is also read back;
`vkCmdCopyImageToBuffer` is used only on that 1x image, never on a MSAA image.
Before clearing, the three actual resolve readbacks compose the original
1280x720 PPM: red256, green256, blue208. There are10 checkpoints.

Build under Cygwin from the R-comp root:

```sh
bash gpu/vulkan/driver-reports/depth-msaa-transfer/build.sh \
  "$PWD/build/vulkan-depth-msaa-transfer-20260930"
```

The script emits validated SPIR-V1.0 and its lock/hashes, runs the host mapping
oracle, checks C syntax with the SDK using Wall/Wextra/Werror, then copies the
existing native builder into this isolated build directory with exactly three
asserted substitutions. The common builder, driver, SDK and active game build
are not modified. Mode `draw` here names the native-shell link variant; its
actual test source is `depth_probe.c`. No generated PPC C++ is involved.

PRIME only, after the console is free, using the session's verified service
settings and chmod payload already configured:

```sh
RCOMP_RUN_TIMEOUT=120 \
RCOMP_RUN_LOG_DIR="$PWD/build/vulkan-depth-msaa-transfer-20260930/console" \
  bash platform/ps5/tools/run_title.sh \
  --app "$PWD/build/vulkan-depth-msaa-transfer-20260930/draw/dist/PPSA88360"
```

Choose the fresh run directory returned by the runner, then fetch the separate
stderr and PPM and judge both numerical results and actual composite bytes:

```sh
python3 gpu/vulkan/driver-reports/depth-msaa-transfer/collect.py \
  "$RUN_DIRECTORY" --fetch \
  --build-out "$PWD/build/vulkan-depth-msaa-transfer-20260930"
```

The collector never launches/kills a title. `--fetch` requires the same explicit
console authorization and host/FTP settings. Without it, it judges local files.
The expected `--build-out` is mandatory. Before judging any BLOCKED/FAIL/GPU
result, the collector checks the most recent native BEGIN against that build.
Missing/different BEGIN is NOT TESTED and excludes old stderr/PPM; with `--fetch`
they are not fetched at all. A matching BEGIN without native END is NOT TESTED
(timeout/incomplete collection), even if partial error text exists. A completed
status 0 with missing checkpoints is FAIL of the evidence contract. It emits
`depth-probe-result.json`; PASS requires all10 actual PS5 checkpoints,
matching observed/expected hashes, native status 0, matching build ID and exact
PPM bytes. BLOCKED is distinct from FAIL. Test policy fixtures live only under
`build/vulkan-depth-msaa-transfer-20260930/tests/TESTDOUBLE_*`; their PASS checks
the collector alone and is never GPU evidence. A timeout/incomplete evidence does
prove a driver failure. Fence waits are bounded at60 seconds per submission;
normal work is finite, and the runner bounds the whole execution at120 seconds.

The initial state7 is stencilALWAYS, not depthAlways. The first tile is tested
with imported nonzero depth before its first clear, whereas the later tiles
start with cleared depth0. If the probe passes, the next title experiment should
inspect depth/stencil immediately after import, opaque color before resolve,
EDRAM after Dump, then destination memory after ResolveCopy. A sloped prepass
precision test needs an independent Xenos oracle before a driver fault claim.
