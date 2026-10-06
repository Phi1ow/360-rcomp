# Host title-entry diagnostics

This target executes the real AOT entry point and production runtime until a
missing service, unsupported field, guest fault or rendering boundary stops it.
It is a developer diagnostic, not a PS5 title or a gameplay test.

`tools/host_title_diagnostic.py` admits known missing **functions** only after
validating the original generated artifacts, function mapping, instruction
diagnostics, TLS, resolved variables and current runtime source selection.
Those functions retain the production missing-import fatal handler. This tool
does not edit an inventory, set `supported_for_link`, change `BOOT_GATE.json`,
or authorize the production M6 packaging path. CMake rejects cross-compilation.

The diagnostic configuration explicitly describes a virtual PAL60 HDMI
1280×720/60 display and English language for XConfig. It provides no signed-in
XAM user, game-region/AV-pack raw enum, online session or host firmware claim.
An unconfigured service therefore still stops with its real diagnostic.
The kernel version comes from the supplied XEX's versioned import libraries.
`game:` and `d:` are read-only mounts. The GPU test boundary is the existing
`TESTDOUBLE_cpu_only_gpu`, which aborts on rendering; no guest image is skipped
or reported as rendered.

Run the Python tool with a verified inventory, new `--out` directory under
`build/`, patched `--xenon-source` and the extracted `--game-root`. Without
`--run` it only compiles. Execution is bounded by `--timeout` (1–120 seconds),
and stores the exact command, process exit and first fatal lines separately
from the unchanged production gate. `--resume` rebuilds the same diagnostic
after source changes before another run.

Only locally owned title inputs are accepted by the workflow. All decoded
images, generated commercial C++, binaries and trace logs remain in ignored
build outputs.
