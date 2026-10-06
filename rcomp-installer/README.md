# R-comp Installer

A local web app for the developer's Windows PC. It turns a statically recompiled Xbox 360 title
(R-comp) into a PS5 app folder and installs it on the developer's own jailbroken PS5, where
ShadowMount+ puts it on the home screen.

**Grand Theft Auto IV and Episodes from Liberty City are the supported games for now.** Other discs
(Gears of War 2, Halo 3, ...) can be checked and recompiled for bring-up work, but they are not playable yet.

It uses only the Python 3 standard library: no pip, no CDN, no build step.

```bash
python server.py
```

This opens `http://127.0.0.1:8360/`. The server listens on loopback only. Use `--port N` to change the port
and `--no-browser` to skip opening a browser. Stop it with Ctrl+C.

Every step reports **PASS / FAIL / BLOCKED / NOT TESTED**:

- **PASS**: the result was verified (console files by SHA-256 read-back, registration by `manual.status` and
  `/system_ex/app`).
- **BLOCKED**: a precondition is missing.
- **FAIL**: the step ran and did not do what it should.
- **NOT TESTED**: what a step does not prove, for example the PS5 boot after packaging.

Logs stream live into the dock at the bottom of the page. Every job can be cancelled.

## Check a game

Pick one of your Xbox 360 disc images. The check runs R-comp's own tools: extract-xiso, then
`m6_inventory.py` with XenonAnalyse and XenonRecomp. It then adds what the inventory does not judge:

- the runtime's kernel version rule: each title sees the kernel it was built against, Xbox 360 2.0.1888 to
  2.0.17559, never below its declared minimum (owner decision of 3 Oct 2026);
- whether it is a Kinect title;
- whether it is a multi-disc title.

The verdict is one of these:

- **READY**: every import is implemented. Build it and play.
- **TRY IT**: only console functions are missing. **Try it on my PS5** builds the game in exploratory mode,
  installs it under the next free PS5 title id (or updates the same game), and registers it. A missing
  function stops the game with an explicit message only if the game calls it.
- **BLOCKED**: something stops the game before or at start: generator diagnostics, missing kernel variables,
  unknown ordinals, an unsupported kernel, Kinect, or a second disc.

The missing services are grouped by family: saves, profile, LIVE and network, dialogs, SEH, files and
devices, threads, audio, and so on. Each check writes `build/checks/<time>/report.json` and `WORK_ORDER.md`.
The work order is the list a developer or an agent needs to add support for the game, with the rules of
`AGENTS.md` and the commands to reproduce. An extracted disc image is deleted after the check (several GB
each); **Try it on my PS5** extracts it again from the image. A folder you gave as the source is never
touched.

### What happens on the PS5

**Try it on my PS5** then starts the game and watches it (180 s by default, `watch_seconds` in Settings).
It reads the title's own logs (`/app0/rcomp_title.log` and `rcomp_title.err`) and records one of these:

- **running**: still running at the end, with the last on-screen frame rate;
- **stopped**: an `RCOMP-FATAL` line, for example `missing_import` with the name of the function the game
  called;
- **crashed**: an `RCOMP-CRASH` line (signal, address);
- **exited**: the program returned without a fatal line;
- **vanished** or **did not start**: the process ended with no R-comp line, or wrote nothing.

After playing a game yourself from the home screen, **Record the last run** reads its latest run the same
way. Every result goes to `build/checks/<time>/tries.json` and into the game's work order.

### All games: one backlog for the AI

Test as many games as you like. **Check all** checks every `.iso` (and every extracted disc folder) in the
disc images folder (`isos_dir`, default `rcomp-installer-data/isos` next to the checkout). It skips the discs that are already checked
unless you tick the box. Every check and every PS5 result is gathered into `build/checks/BACKLOG.md` and
`backlog.json`, in this order:

1. functions a game really called on the PS5;
2. blockers;
3. the other missing functions, ranked by the number of games that import them.

**Export for the AI** downloads that file (`RCOMP_BACKLOG.md`). It is self-contained: the rules, the
ranked work, the evidence of each game and the commands to reproduce. Hand it over in one go, then run
**Check all** (forced) and **Try** again after the fixes.

Measured on 3 Oct 2026, Gears of War 2 (Unreal Engine 3) is BLOCKED, by the missing `XboxHardwareInfo`
kernel variable only. The kernel check passes (2.0.7645.0), and 45 console functions are missing, against
89 the day before.

## The three modes

### 1 · Install app

The input is an app folder: `eboot.bin`, `sce_sys/param.json`, `sce_sys/icon0.png`, `sce_module/libc.prx`,
`image/plain.xex`, `game/`, and optionally `manifest.sha256`.

1. Validate `param.json` (title id `PPSA` + 5 digits, matching `conceptId` and `contentId`, a display name)
   and hash every file. If the folder has a `manifest.sha256`, the hashes must match it.
2. Refuse if the title is running (checked with ps5vkctl `procs`). Refuse if the id is already used on the
   console by a different title, unless *overwrite* is ticked. Refuse if ShadowMount+ registered the id from
   another path.
3. Upload to `/data/homebrew/<id>/` over FTP (port 2120) and read every file back to compare its SHA-256.
   A file already on the console with the same size is read back first and kept if it matches, so an
   interrupted install can be re-run and resumes. Nothing on the console is ever deleted.

   Transfers use 8 parallel FTP connections (*Parallel FTP connections* in Settings). Measured from this
   PC over Wi-Fi (18 ms RTT), the console's FTP server (zftpd) gives about 4 MiB/s per connection, and the
   rate grows with the number of connections. A real 6.7 GiB install verified at about 20 MiB/s with 8
   connections, against 6 MiB/s with one.

   Larger client socket buffers made no measurable difference in an A/B test over this Wi-Fi link. The
   lever is parallelism, or a wired link: zftpd's own README quotes about 110 MB/s on gigabit (not
   measured here).

   A transport error is retried on a fresh connection, up to 4 times with back-off. Such errors are a dropped
   connection, a temporary 4xx reply, or `550 Cannot …`. A SHA-256 mismatch is never retried: it is a FAIL.
4. Write the id to `/data/rcomp-installer/chmod.request`, then send the chmod payload to the ELF loader
   (9021). The payload sets mode 0777 under `/data/homebrew/<id>`; without that the PS5 refuses to run
   `eboot.bin` (`0x80aa001a`). It also records the capacity of `/data` with `statfs(2)` in
   `savedata/.rcomp-capacity`: a sandboxed title cannot measure it, and games read their free space before
   saving. The step is checked twice: the payload's `failed=0` line, and the mode FTP
   shows for `eboot.bin`.

   The console's zftpd accepts `SITE CHMOD` and answers `200 CHMOD command successful`, but the mode
   stays `0666`. This was checked with MLSD on a probe file on 2 Oct 2026, so the tool does not use it.
5. Copy `/data/shadowmount/manual.lst` to `manual.lst.before-rcomp-installer-<timestamp>` and verify the
   copy, then append `/data/homebrew/<id>` and read the file back.
6. Poll for up to 90 s (configurable) until `manual.status` has `installed\t<id>\t…` **and**
   `/system_ex/app/<id>` exists. Only then is the install PASS.

### 2 · Package artifacts

The inputs are the linked `eboot.bin`, the decoded `plain.xex`, the game files folder and `libc.prx`.
`libc.prx` is prefilled from R-comp's `build/platform-ps5-tools/libc.prx`.

The XEX is parsed (XEX2 headers, then the XDBF resource) for:

- the Xbox title id, media id and disc number;
- the title name in every language;
- the title image.

You choose the PS5 title id and the name. The page suggests the next id that is free on the console and
warns when an id is taken.

Two products can share an Xbox title id. GTA IV and Episodes from Liberty City are both `545407F2`, and
EFLC's XDBF name is "GTA IV". In that case, enter a different PS5 id and name.

The output is `<output>/<PPSA id>/` containing:

- `param.json` in the same shape as the R-comp titles already running on the console;
- `icon0.png` at 512×512, made from the XEX image, from a PNG you give, or generated with the name;
- `manifest.sha256`.

Then press **Install it →**.

### 3 · Recompile from ISO

The input is an Xbox 360 ISO, or an extracted disc folder that contains `default.xex`. The tab drives the
real R-comp pipeline and streams its output:

| Step | R-comp tool |
| --- | --- |
| Extract the disc | `build/catalog-tools/extract-xiso/extract-xiso.exe -x` |
| Decode and inventory the XEX, then recompile | `tools/m6_inventory.py --require-supported` with the patched XenonAnalyse/XenonRecomp and `rcomp_xex_decode` that `tools/m6_setup.sh` builds |
| Compile the AOT archives | `tools/build_ps5_archives.sh` (Xenos source `build/prime-radv-resume-20260929/xenos-source-v3`, SDK, XMA codec) |
| Link with the pinned RADV driver and fake-sign | `gpu/vulkan/ps5/build-radv-probes.sh game` |
| Assemble the app folder | mode 2 |

The commands run in R-comp's Cygwin: the one `tools/bootstrap_cygwin.py` creates (`build/prime-host-tools/cygwin`),
or the developer's full install (`build/vulkan-gta-radv-20260928/host-cygwin-full`).
**Check toolchain** lists each required tool as PASS or BLOCKED, with the command that provides a missing one.
`python tools/setup_windows.py` sets all of it up on a Windows PC ([docs/INSTALL_WINDOWS.md](../docs/INSTALL_WINDOWS.md)).
The host tools are looked for in two places, the first that exists wins: `build/catalog-tools/xenonrecomp-v23` and
`build/prime-xex-decode-v18` (the developer's catalog build), then `build/cpu-xenonrecomp` and `build/cpu-xex-decode`
(what `tools/m6_setup.sh` builds).

The run is long. Some cases stop it:

- **FAIL**: a generator warning or an unrecognized instruction.
- **BLOCKED**: the inventory's production gate. You can tick *Exploratory build* to link anyway, as was
  done for TBoGT. Only listed missing functions are admitted, and each one stays fatal when the game calls it.

The render scale defaults to 3 (4K).

**Build profiles** (`rinstaller/profiles.py`). A profile is the set of R-comp CMake options a title is built
with. It is applied to the archive tree after `build_ps5_archives.sh`, then checked in `CMakeCache.txt`. *Auto*
picks it from the disc identity: the title id plus the media id, or the disc layout for other regions of the two
games.

| Profile | Options | Measured |
| --- | --- | --- |
| every profile | PC sampler off, periodic frame capture off (it wrote 24 MB `.ppm` files into the app folder), native-audio probe and timebase observer off | — |
| `gta4`: Grand Theft Auto IV | RADV `cswave32,pswave32,gewave32` and `nofmask,nonggc`; guest threads on CPUs 0-7, replay on 8, bridge on 12; Xenos `rcomp_async_submit`, `rcomp_revalidate_frames=8`, `rcomp_fused_resolve` | 54.69 fps at 4K (phase 173, final 90 s of the scripted run) |
| `eflc`: Episodes from Liberty City | the `gta4` options plus RAGE's cache devices at `/app0/cache`, read-only | about 55-56 fps at 4K in The Ballad of Gay Tony (bring-up measurement) |
| `generic` | diagnostics off only | — |

`RCOMP_M6_CAPTURE_INTERVAL_SECONDS=0` did not compile in R-comp: `frame_dumper` became unused under
`-Werror`. `app/m6/main.cpp` now compiles the capture thread only for an interval greater than 0.

`build_ps5_archives.sh` builds only `rcomp_m6_title` and `rcomp_m6_generated`. The runtime calls the platform
audio API, so the tool also builds R-comp's `rcomp_platform_audio` target in the same tree. Without it,
`build-radv-probes.sh` stops with `BLOCKED platform audio requested but missing`. This gap belongs to R-comp's
script and should be proposed to its owner.

A failed run can be resumed with **Resume**. Resuming reuses the extracted disc and the inventory (generated
C++) of that run. The archive build is incremental (ninja), and the link and packaging run again.

Measured on 2 Oct 2026 with the extracted Episodes from Liberty City disc:

- inventory with XenonRecomp: 33 s, 58,213 functions, no generator warning;
- AOT archives: 336 s with 6 jobs;
- link: 12 s;
- packaging: 7 s;
- result: a 101.4 MiB `eboot.bin`.

Its exploratory production gate is BLOCKED, because the title has listed missing imports.

That build (`dc4d66eaa2b3b302`) was installed with mode 1 and launched from the Console tab. It started on
the PS5 and presented frames: `RCOMP-FPS fps=60.00`, more than 1,500 swaps, a 3840×2160 display plane, and
XMA audio decoding. Gameplay was NOT TESTED. The generic recipe does not carry the RAGE-tuned performance
options of the TBoGT bring-up, so the measured 4K frame rate of that title applies to the tuned build only.

ISO extraction was checked separately on the EFLC ISO: 231 files, with the same names and sizes as the
reference extraction.

R-comp's `build_ps5_archives.sh` and `build-radv-probes.sh` refuse any output outside `R-comp/build/`, so
their intermediate output goes to `R-comp/build/rcomp-installer/<id>-<time>/`. That directory is
git-ignored. Everything else goes to this tool's own folders:

- the extracted disc and the inventory go to the work folder (`work_dir`);
- the app goes to the chosen output folder.

## Console tab

- Status of FTP, ps5vkctl and `procs`.
- The R-comp/homebrew titles with their `manual.lst`, `manual.status` and `/system_ex/app` state.
- **Register** for a folder that is installed but not registered.
- **Launch** and **Close** through ps5vkctl.
- **Load ps5vkctl**: loads R-comp's `build/platform-ps5-payloads/ps5vkctl.elf`, only if its SHA-256
  matches the pinned one.
- **Kernel log**: 30 s of the 3232 stream.

## Settings

Settings are saved in `build/settings.json`:

- the console address (empty by default: set it before the first console action) and ports
  (defaults: FTP 2120, loader 9021, ps5vkctl 9111, kernel log 3232);
- the R-comp checkout (by default the folder this tool sits in: `rcomp-installer/` is a folder of the repository) and
  the reference deps folder (never written);
- Cygwin `bash.exe`;
- the ps5-payload SDK;
- the output and work folders (by default `rcomp-installer-data/` next to the checkout: the pipeline refuses a work
  or output folder inside the checkout; a separate copy of this tool keeps them in its own `build/`);
- the ShadowMount+ wait;
- the number of parallel FTP connections.

The app output is refused inside the R-comp checkout or the deps folder.

## What it needs

- Python 3.9 or newer on Windows (3.12 tested; Git Bash or Cygwin python also work).
- The console on the LAN, with its boot payloads loaded: FTP, ELF loader, kstuff, ShadowMount+, and
  optionally ps5vkctl for the running-title check and for launching titles from the PC. Without ps5vkctl an
  install still works (it warns that it cannot check that the title is not running); **Launch**, **Close** and
  **Try it on my PS5** need it. This repository does not provide the payloads.
- The chmod payload: `payloads/rcomp_chmod.c`. It is built once with the ps5-payload SDK's `prospero-clang`
  (R-comp's `build/vulkan-gta-radv-20260928/ps5-sdk` by default) into `build/payloads/`, and only
  rebuilt when its source changes. It never contains a title id.
- Mode 3 only: R-comp's toolchain as listed by **Check toolchain**, set up by `python tools/setup_windows.py`.

## Files

```
server.py                 HTTP server (loopback), JSON API, server-sent events log stream
rinstaller/xex.py         XEX2 / XDBF reader (ids, names per language, title image)
rinstaller/png.py         PNG decode / fit to 512 / encode, generated title tile (zlib only)
rinstaller/package.py     app folder assembly and validation, param.json, manifest.sha256
rinstaller/console.py     FTP install + read-back, chmod payload, ShadowMount+ registration, console state
rinstaller/payloads.py    chmod payload build, ps5vkctl loading (SHA-256 pinned)
rinstaller/pipeline.py    mode 3: drives R-comp's own scripts
rinstaller/cygwin.py      runs commands in R-comp's Cygwin, streams output and log files, cancel = kill tree
rinstaller/jobs.py        background jobs, live log, one console job at a time
static/                   the browser UI (plain HTML/CSS/JS)
payloads/rcomp_chmod.c    the chmod payload source
tests/                   offline tests (XEX, PNG, packaging, parallel transfer pool with a fake console):
                          python -m unittest discover -s tests
build/logs/               one log file per job (generated)
```

No game content, keys or SDK files belong in this folder. `build/` holds generated output only.
