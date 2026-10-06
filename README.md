# R-comp

R-comp recompiles Xbox 360 games ahead of time into native PlayStation 5 applications. There is no
emulator and no JIT.

1. XenonRecomp translates the game's PowerPC code into C++ on a PC.
2. The C++ is compiled and linked with R-comp's runtime into a standalone PS5 `eboot.bin`.
3. The runtime provides the Xbox 360 kernel and XAM services as high-level emulation.
4. The adapted rexglue Xenos GPU translates the Xbox 360 shaders to SPIR-V and renders through
   PS5_Vulkan/RADV.

## Status (4 October 2026)

**Grand Theft Auto IV and Episodes from Liberty City (EFLC) work for now.** Every other game is still in bring-up and
is not supported: it may boot to a menu or further, but it is not playable from start to finish.

| Game | State on the PS5 | 4K (3840×2160) frame rate |
| --- | --- | --- |
| Grand Theft Auto IV | **Works**: boots, reaches gameplay, plays with audio, saves its games from its own menu. A 6-minute run of the release build showed no fatal error. | 54.69 fps in the final 90 s of the scripted 420 s run (tuned build, phase 173); 52-55 fps in the release build run , use capped 30 fps to get less stuttering|
| Episodes from Liberty City (EFLC) | **Works**: boots to the episode menu and into The Ballad of Gay Tony, played on the PS5 on 4 October 2026. Its saves and The Lost and Damned are not tested yet. | 49-56 fps in scripted 90 s windows of The Ballad of Gay Tony (2 and 4 October 2026) , use capped 30 fps to get less stuttering |
| Other games (Gears of War 2, Halo 3, ...) | In bring-up, **not supported yet** | not measured for release |

**4K at a constant 60 fps is not reached** in the heaviest scenes of GTA IV.

**Imports.** GTA IV runs with every XAM import implemented and 145 of its 150 kernel imports.

The five unimplemented imports are `RtlUnwind`, `__C_specific_handler`, `RtlCaptureContext`,
`IoCompleteRequest` and `IoInvalidDeviceRequest`. They belong to:

- structured exception dispatch, which R-comp does not run;
- I/O request packets sent to a game's own device driver, which R-comp never creates.

If a game ever calls one of them, the title stops with an explicit `RCOMP-FATAL` line. It never pretends
the call succeeded.

**Saves.** The PS5 has no Xbox gamertag, so the game sees one local offline profile named "Player". Its
saves and profile settings are written to `savedata/` inside the game's own app folder, so they survive
reinstalls and updates.

- GTA IV saves its games on the PS5 (3 October 2026). A save made from the game's own menu is written as
  an Xbox 360 content package under `savedata/<profile>/545407F2/00000001/` (for example `SGTA412`,
  707,822 bytes, with its `.xcontent` header and thumbnail). Loading it back in a later session: NOT
  TESTED yet.
- Profile settings are persisted on the PS5 (verified with GTA IV).
- Every save service passed a self-test on the PS5's real storage: create, write, close, reopen, read back,
  enumerate, thumbnail, creator, truncate and device capacity.

Xbox LIVE services answer as on a console that is not signed in to LIVE.

**Launch options.** Every game built by R-comp starts on a short options screen:

- **Resolution:** 1280×720, 2560×1440 or 3840×2160 (4K, the default).
- **Frame rate:** unlocked (the default) or capped at 30, 40, 50 or 60 fps. The cap holds every frame for at
  least 1/30, 1/40, 1/50 or 1/60 s, which trades the higher average for an even pace.
- The choices are remembered for the next launch.
- The game starts after 15 seconds without input.

MSAA stays the game's original: the only 1x mode available today leaves part of the picture undrawn.

## Installing a game: R-comp Installer

[`rcomp-installer/`](rcomp-installer/README.md) is a local web app (Python standard library only) with
three modes:

1. **Recompile from the ISO.** It drives the real pipeline: extraction, XenonRecomp, AOT build, RADV link.
   It applies the measured build profile of GTA IV or of Episodes from Liberty City automatically. Other discs
   can be recompiled for bring-up work, but they are not supported games.
2. **Package** already-built artifacts into a PS5 app folder.
3. **Install** an app folder on the console. Every file is read back and checked by SHA-256, the files
   are given mode 0777, and the title is registered with ShadowMount+ so it appears on the home screen.

You need:

- **Your own game disc**, dumped to an ISO. No game content, executable, generated code, key or SDK is in
  this repository, and none may be added.
- **A jailbroken PS5** running its usual payloads: an FTP server (zftpd, port 2120), the ELF loader (9021),
  kstuff and ShadowMount+, plus `ps5vkctl` (9111) to launch titles from the PC.
- **The R-comp toolchain on the PC**, set up by one command, `python tools/setup_windows.py`: a locked Cygwin
  (LLVM, CMake, Ninja), the prebuilt kit (ps5-payload SDK, pinned RADV build, XMA codec, prepared Xenos source),
  XenonRecomp and extract-xiso; about 430 MB to download. The installer's *Check toolchain* lists each piece.
  The whole procedure is in [docs/INSTALL_WINDOWS.md](docs/INSTALL_WINDOWS.md). It was run from a fresh clone on
  the developer's PC (4 October 2026); a different machine has **not been tested yet**.

## Repository layout

| Path | Contents |
| --- | --- |
| `cpu/` | XenonRecomp patches, ABI, generated-code runtime support |
| `runtime/` | Kernel and XAM HLE, VFS, threads, audio, saves ([runtime/docs/XAM.md](runtime/docs/XAM.md)) |
| `gpu/xenos/`, `gpu/vulkan/` | Xenos GPU (adapted from rexglue, BSD-3) and the PS5_Vulkan/RADV integration |
| `platform/` | PS5 platform layer (memory, threads, audio output, input) |
| `app/` | The title shell that ties runtime, GPU and display together |
| `tools/` | Pipeline scripts: inventory, archives, disc catalog, console test kit |
| `rcomp-installer/` | The installer web app |
| `rcomp-shelf/` | A cover-flow launcher for the installed titles (GPL-3.0-or-later, see its NOTICE) |
| `docs/` | The Windows installation guide and evidence files |

To install a game, follow [docs/INSTALL_WINDOWS.md](docs/INSTALL_WINDOWS.md).

## Host checks

```sh
tools/fetch_deps.sh
tools/m6_setup.sh
RCOMP_SKIP_PS5=1 tools/check_all.sh
```

These checks are for contributors, on Linux, WSL or macOS (they need `curl`, `unzip` and a full compiler
toolchain); a Windows PC that only installs games does not need them. They use synthetic fixtures only. Host
validation is never PS5 evidence.

## Credits and licences

R-comp is free software, licensed under the **GNU General Public License, version 3 or (at your option)
any later version** ([LICENSE](LICENSE)). Files that carry their own licence notice, such as the adapted
rexglue-sdk code and `third_party/`, keep it.

Dependencies are pinned with their licences in [deps/deps.lock](deps/deps.lock). They include:

- XenonRecomp (MIT);
- the rexglue-sdk GPU subsystem (BSD-3, Xenia-derived; notices in `gpu/xenos/rexglue/LICENSE.rexglue`);
- PS5_Vulkan and the ps5-payload SDK (GPL-3.0);
- PS5_Mesa / RADV (MIT);
- extract-xiso (BSD-style);
- the FFmpeg XMA decoder (LGPL-2.1-or-later).

Xbox 360, PlayStation 5 and the game titles are trademarks of their owners. This project is not affiliated
with Microsoft, Sony, Rockstar Games or Take-Two Interactive.
