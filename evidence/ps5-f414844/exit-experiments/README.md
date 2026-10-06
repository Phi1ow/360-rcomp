# Title exit experiments (2026-09-26, 00:34–00:41)

Diagnostic copies of kit eboots, made outside the kit. Only the bytes listed
below change, and each copy gets its manifest.sha256 recomputed.

| Folder | Copy of | Change | Result |
| --- | --- | --- | --- |
| `6-noteardown/` | `6-vk-draw-test` | CRT `je` → `jmp` at text+0x161: the loader's `teardown` (rsi) is no longer registered with `atexit` | Still `eboot.bin calls exit() exit_value=0` then `SIGSYS`; the process stays until killed at 60 s. So `teardown` is not the cause: the kernel refuses the application's exit syscall. |
| `1-park/` | `diag-kit/1-platform` (libkernel fix) | CRT `mov edi,eax; call *exit@GOT` at text+0xef → `jmp .` (the title parks after `main` returns) | `end status=0`, 11/11 PASS. The runner saw the end line after 2 s and closed the title; **no fatal signal** in the klog; `All processes exited`. |
| `PPSA88360-20260925T224027Z/` | same `1-park` title | first console run of `platform/ps5/tools/run_title.sh` (with `RCOMP_PS5_LOADER_PORT` / `RCOMP_PS5_CHMOD_ELF`) | deploy + read-back, `ps5_chmod_title fixed=8 failed=0`, launch, "end line in the log; closing PPSA88360", `VERDICT PASS`, exit 0. |

The source fix (`rcomp_title_park()` in `platform/ps5/title/main.cpp`, the end of
`main()` in `gpu/vulkan/common/rcomp_title_wrap.c`) sleeps with `sceKernelUsleep`
instead of spinning like `jmp .` does; everything else is the same behaviour.
