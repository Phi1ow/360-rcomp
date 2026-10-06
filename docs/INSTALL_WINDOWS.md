# Installing R-comp on a Windows PC

This guide takes a fresh checkout to a game installed on your PS5. The steps are the ones a clean checkout was run
through on 4 October 2026 (see [What was tested](#what-was-tested)).

## What you need

- **A Windows 10 or 11 PC**, 64-bit, with Git for Windows (`git` on the PATH) and Python 3.9 or newer (3.12 tested;
  use `py` where this guide says `python` if that is how Python starts on your PC).
  About 20 GB free for the toolchain and a first game: roughly 3 GB for the toolchain, then per game the extracted
  disc (6.5 GB), the build (about 1 GB) and the finished app folder (6.7 GB).
- **Your own game disc**, as an ISO or as a folder you already extracted (the folder that holds `default.xex`). Grand
  Theft Auto IV and Episodes from Liberty City are the supported games. No game content is in this repository.
- **A jailbroken PS5** (tested on firmware 9.40) running, for each console boot, these payloads: an FTP server
  (zftpd, port 2120), the ELF loader (port 9021), kstuff, then ShadowMount+. `ps5vkctl` (port 9111) is optional: it only
  starts and stops titles from the PC. This repository does not provide those payloads.
- The PC and the PS5 on the same network. Enter the PS5's address in the installer's Settings before the first console action (it has no default).

## 1. Get the sources

```sh
git clone https://github.com/Phi1ow/360-rcomp.git
cd 360-rcomp
```

Clone into a short path without spaces (for example `C:\src\360-rcomp`). It was run from paths of about 25 characters;
a path with spaces, or one long enough to approach Windows' 260-character limit once the build folders are added, is
NOT TESTED.

No special Git setting is needed. Git for Windows converts text files to CRLF by default; the scripts that depend on
line endings (`*.sh` are forced to LF by `.gitattributes`, and the XenonRecomp copy is normalised before it is patched)
cope with it.

## 2. Set up the toolchain: one command

```sh
python tools/setup_windows.py
```

It runs the steps below in order, skips the ones already done, and stops at the first failure with its reason, so it
can simply be run again. About 430 MB are downloaded and it takes about ten minutes on a fast connection. Nothing
outside the checkout is touched: no registry, no PATH, no system package.

| Step | What it does | Download |
| --- | --- | --- |
| `sources` | `git submodule update --init --recursive third_party/XenonRecomp` (pinned commits) | 65 MB |
| `zstandard` | `pip install --target build/prime-host-tools/pylibs zstandard==0.25.0` | 0.5 MB |
| `cygwin` | `tools/bootstrap_cygwin.py`: the locked Cygwin (clang/LLVM 22, gcc 14, CMake, Ninja, Python, git: 146 packages whose SHA-512 are checked), then the two small scripts and the CA bundle described in the script's header | 268 MB |
| `kit` | `tools/install_kit.py`: the prebuilt PS5 toolchain kit of [`deps/kit.lock.json`](../deps/kit.lock.json) (ps5-payload SDK, Mesa RADV for the PS5, compiler-rt, the FFmpeg XMA codec, `libc.prx`, the prepared Xenos source, the PS5_Vulkan link inputs), size and SHA-256 checked | 95 MB |
| `extract-xiso` | `tools/build_extract_xiso.sh`: the disc extractor, built from its pinned source | < 1 MB |
| `xenonrecomp` | `tools/m6_setup.sh`: the patched XenonRecomp and XenonAnalyse, and `rcomp_xex_decode` | none |

`python tools/setup_windows.py --list` shows which steps are done. Each step is also a script you can run alone;
`python tools/cygwin_run.py <command>` runs a command inside the Cygwin the bootstrap made (for example
`python tools/cygwin_run.py bash tools/m6_setup.sh`).

The kit is a release asset of this repository. While the repository is private, `tools/install_kit.py` cannot fetch it
anonymously: it falls back to the GitHub CLI (`gh auth login` once). Without `gh`, download the four archives named in
`deps/kit.lock.json` from the release page into one folder and run `python tools/install_kit.py --from <folder>`.
Do not extract the archives with `tar`: they contain symbolic links (the SDK's header aliases such as `errno.h`) that a
tar run from Git Bash or PowerShell cannot create, and the SDK would be left without `errno.h`, `fcntl.h` and `float.h`.

## 3. Start the installer

```sh
python rcomp-installer/server.py
```

It opens `http://127.0.0.1:8360/` (loopback only). Then:

1. **Settings**: set the PS5 address. The other defaults fit a normal checkout: the R-comp checkout is the folder the
   installer sits in, and the work, app and disc folders are created next to it in `rcomp-installer-data`
   (the pipeline refuses a work folder inside the checkout). **Save**.
2. **3 · Recompile from ISO**, **Check toolchain**: every line must be PASS. A BLOCKED line names the command that
   provides it.

## 4. Recompile and install a game

1. On the **3 · Recompile from ISO** tab pick the ISO (**ISO…**) or the extracted folder (**Folder…**). Type a PS5
   title id: `PPSA` and five digits, for example `PPSA88380` (the page tells you whether it is free on the console).
   The profile stays on *Auto*, which applies the measured profile of GTA IV or of Episodes from Liberty City; the
   render scale stays on 3 (4K).
2. **Start the pipeline**. It extracts the disc, runs XenonRecomp, compiles the AOT archives, links with the pinned
   RADV driver and assembles the app folder. On the developer's PC (8 logical cores) a game takes 5 to 10 minutes
   once the disc is extracted. A failed run can be continued with **Resume**.
3. **Install it →** uploads the app folder to `/data/homebrew/<id>` over FTP, reads every file back and compares its
   SHA-256, sets mode 0777 with the chmod payload (built from the SDK on first use) and registers the title with
   ShadowMount+. A 6.7 GB game took 7 to 9 minutes over Wi-Fi (28 to 37 MiB/s with 8 connections).
4. The game is on the PS5 home screen. Launch it from there. The first screen is the launch options menu (resolution,
   frame rate cap); the game starts after 15 seconds without input.

ShadowMount+ does not rescan its list while a game is running: install while no title is running, or the
registration step reports a failure and completes when the game is closed.

## If something fails

| Symptom | Cause and fix |
| --- | --- |
| `Check toolchain` shows BLOCKED lines | Run the command in the hint, normally `python tools/setup_windows.py`. |
| `errno.h`, `fcntl.h` or `float.h` not found while compiling | The kit was extracted with `tar`. Run `python tools/install_kit.py`. |
| `rev: command not found`, `/llvm-: No such file`, or `duplicate symbol: payload_exit` | The Cygwin comes from an older bootstrap. Delete `build/prime-host-tools/cygwin` and run `python tools/setup_windows.py`. |
| `error adding trust anchors from file: /etc/pki/tls/certs/ca-bundle.crt` | Same cause. The current bootstrap runs `update-ca-trust`. |
| `build/prime-host-tools/cygwin exists but is incomplete` | An earlier bootstrap was interrupted. Delete that folder and run `python tools/setup_windows.py` again. |
| `hunks FAILED` and `*.rej` files while patching XenonRecomp | The sources were converted to CRLF. Delete `build/cpu-xenonrecomp-src` and run `python tools/setup_windows.py --only xenonrecomp`. |
| `work folder must not be inside ...` | A work or output folder in Settings is inside the checkout. Use the defaults or a folder elsewhere. |
| `launch refused 0x80940033` on the PS5 | ShadowMount+ is not loaded. Load kstuff, then ShadowMount+, after each console restart. |
| `0x80aa001a` on the PS5 | The title files are not executable: the chmod step did not run. Reinstall with the installer. |

## Updating

After a `git pull`, run `python tools/setup_windows.py` again: it only does what is missing. It does not re-extract the
kit while the files it checks are present, so when `deps/kit.lock.json` changed, run `python tools/install_kit.py`
(it replaces the prepared Xenos source, which must never mix two versions). Games already installed on the console
keep running; reinstall a game from the installer to get a newer build of it.

## What was tested

On 4 October 2026, from a fresh clone on the developer's Windows 11 PC (8 logical cores, 31 GB RAM) with an empty
`build/`: the Cygwin bootstrap, the kit, the host tools, and then, in the installer, the recompilation, the packaging and
the installation on a PS5 (firmware 9.40) of Grand Theft Auto IV (about 6 minutes) and of Episodes from Liberty City
(about 7 minutes). Every installed file was read back from the console and matched by SHA-256, and ShadowMount+ listed
both titles on the home screen.

The first run of that test needed manual workarounds, which `tools/setup_windows.py`, `tools/install_kit.py`,
`tools/bootstrap_cygwin.py` and the installer's defaults now handle: the symbolic links of the kit, the missing
`llvm-config` (the SDK compiler wrapper needs it; `rev`, which the wrapper uses on a fallback path, is provided too), the
empty CA bundle of Cygwin's git, CRLF sources that the patches do not match, the locations the installer looks for the host
tools in, and the default settings that pointed at the developer's folders.

The fixed scripts were then run on two more fresh clones made with Git for Windows' default line endings. On both,
`python tools/setup_windows.py` ended with all six steps passing and the installer's *Check toolchain* showed every line
PASS with its default settings; the first of them also recompiled and packaged Grand Theft Auto IV from the installer in
under 7 minutes. (Those two runs reused the downloaded Cygwin packages and kit archives of the first run, so only the first
one fetched them.)

**NOT TESTED**: starting the two installed games on the PS5 (the same configuration was played from the developer's own
builds); a PC other than the developer's; Windows 10; a PS5 other than the developer's; extracting an ISO from the
installer in a fresh checkout (the discs were folders that were already extracted; the extractor built by the script listed
a real ISO correctly); downloading the kit without the GitHub CLI, which needs the repository to be public.
