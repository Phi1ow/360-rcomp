# PS5 isolated depth/stencil transfer results

Status: PASS. PRIME ran the original native probe serially after phase17 on
2026-09-30. Runner return 0, collector return 0, native end status 0. Provenance
expected/observed BUILD_ID is `6a56eeabe40c1307`; eboot SHA256 is
`a777634c776ee70d2a1dfc493e3abbf78a8cc32c7f7fda77f6e8424a945b503b`.

Evidence directory:
`build/prime-perf-4k30-20260929/18-native-depth-probe/console/PPSA88360-20260930T110950Z`.
`depth-probe-result.json`, stdout, stderr and actual PPM were read locally;
recorded file hashes were verified against their bytes. No extra console run
was performed during this documentation update.

Actual device: `PlayStation 5 GPU (RADV NAVI21)`. standardSampleLocations=1,
sampleRateShading=1, depth/stencil/color sample-count masks=F, stencilExport=1.
Each of 10 checkpoints checks 2,621,440 records: total 26,214,400 sample records,
bitwise float32 depth, stencil8, color samples where present, and written-record
marker. Every comparison has zero mismatches and equal observed/expected hashes.
The first imported pattern sample is depth 0.5 (`3F000000`), stencil 58 (`3A`);
the first opaque starts with imported depth 0.25 (`3E800000`), stencil 3 before any
clear. Thus the first tile is checked with nonzero imported depth. The equal
NOT_EQUAL depth case preserves the expected depth and copies stencil via
zfailREPLACE. Opaques use GEQUAL6 and stencilALWAYS7, then clear depth0/stencil0.

| Checkpoint | Observed FNV64 | Expected FNV64 | First depth / stencil / RGBA |
| --- | --- | --- | --- |
| source4x-patterns | ED2F07E355A79B25 | ED2F07E355A79B25 | 3E000000 / 00 / 00000000 |
| pattern-transfer256 | BF9979A1ABABE725 | BF9979A1ABABE725 | 3F000000 / 3A / 00000000 |
| equal-depth-stencil-zfail-replace208 | 6D6A1D60B314F425 | 6D6A1D60B314F425 | 3F000000 / 3A / 00000000 |
| constant-import-quarter | 3D6B82B5F00E2325 | 3D6B82B5F00E2325 | 3E800000 / 03 / 00000000 |
| opaque-tile0-preclear | B40393EEAD362325 | B40393EEAD362325 | 3E800000 / 82 / FF0000FF |
| clear-tile0-depth0-stencil0 | 180261C1E0222325 | 180261C1E0222325 | 00000000 / 00 / 00000000 |
| opaque-tile1-preclear | 377E81DFA9C62325 | 377E81DFA9C62325 | 3E800000 / 81 / FF00FF00 |
| clear-tile1-depth0-stencil0 | 180261C1E0222325 | 180261C1E0222325 | 00000000 / 00 / 00000000 |
| opaque-tile2-preclear | 4EF3FBC8DC312325 | 4EF3FBC8DC312325 | 3E800000 / 08 / FFFF0000 |
| clear-tile2-depth0-stencil0 | 180261C1E0222325 | 180261C1E0222325 | 00000000 / 00 / 00000000 |

Actual PPM is exactly 1280x720: red 256 rows, green 256 rows, blue 208 rows,
assembled from the three GPU single-sample resolve readbacks before clear.
Its SHA256 is `6258363c76015dca84b0af930cdb6ee20973efef1b3f63e5445a7815e124f6d9`.
The file bytes were compared against the complete original synthetic oracle.
MSAA depth/stencil/color were read per sample through compute; no multisample
image was copied to a buffer.

| Evidence file | SHA256 |
| --- | --- |
| rcomp_title.log | 6ea3035afb90418a9761a084a316921da9c87fcf746dd2ee70f0ec326c7f56ea |
| rcomp_title.err | 3d2344ab55320785e757371828478963bc0d8de74754fb3544db9745d2e7cf99 |
| depth_tiles.ppm | 6258363c76015dca84b0af930cdb6ee20973efef1b3f63e5445a7815e124f6d9 |

This PASS covers the isolated original GLSL mapping, depth/stencil fixed-function
operations, barriers, multisample readback, native color resolve and three-tile
synthetic composition on the actual PS5. It does not validate guest translated
shaders, Xenos EDRAM Dump/ResolveCopy, guest memory destination or the title's
presentation chain. Correctness of that title chain and the cause of its visual
defect remain NOT TESTED by this probe. The title was not changed by this work;
no RADV defect is established. The next evidence must come from the title's
one-frame checkpoints after import, before resolve/clear and after Dump/ResolveCopy.
