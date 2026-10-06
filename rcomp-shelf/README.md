# R-comp shelf

The game menu for [R-comp](../README.md) titles on a jailbroken PS5: a 3D cover-flow shelf of the statically
recompiled Xbox 360 games, to **play** the installed ones and to **install** the packaged ones. It is its own PS5
application (title id `PPSA88300`), separate from every game.

The shelf is PS5SX2's (Spyros, [Swordpdf/PS5SX2](https://github.com/Swordpdf/PS5SX2), GPL-3.0-or-later), used
with its author's agreement and adapted from PS2 disc images to R-comp titles. On screen the shelf shows
R-comp's author's handles (Discord `.philow`, X `@Phi10w`); the upstream credit is kept in the sources and here. See [UPSTREAM.md](UPSTREAM.md).

## Why a separate application per game

An emulator such as PS5SX2 reads a game's code when the game starts, so one application runs every game. R-comp
compiles each game ahead of time: the game's code *is* its eboot. Each recompiled title is therefore a PS5
application of its own (`/data/homebrew/<PS5 title id>/`, shown on the home screen by ShadowMount+). The shelf lists
them, installs new ones from packages and starts the one you pick. Recompiling a game is done on the PC with R-comp;
the console only installs and runs the result.

## On the console

| Folder | What |
| --- | --- |
| `/data/homebrew/PPSA88300/` | the shelf itself (`eboot.bin`, `sce_sys/`, `sce_module/libc.prx`) |
| `/data/homebrew/<PPSA id>/` | installed R-comp titles: `eboot.bin`, `sce_sys/param.json`, `image/plain.xex`, `game/` |
| `/data/rcomp/packages/<any name>/` | packages: the same folder, not installed yet |
| `<USB drive>/rcomp/<any name>/` | packages on a USB drive (`/mnt/usb0`..`7`, folder name in any case) |
| `/data/rcomp/covers/` | your covers: `<PPSA id>`, `<title ID>-<media ID>`, `<title ID>` or `<title>` `.jpg`/`.png` |
| `/data/rcomp/cache/` | downloaded x360db entries and covers |
| `/data/rcomp/trash/` | the previous copy of a title an update replaced (never deleted by the shelf) |
| `/data/rcomp/lang/<code>.txt` | text fixes (`hint.play = Jouer`), as PS5SX2's |
| `/data/rcomp/logs/shelf.log` | the shelf's log |

A title folder also counts when it holds `cover.jpg` / `cover.png`.

**Each title needs its own PS5 title id** in `sce_sys/param.json`; the shelf never invents one. (R-comp currently
packages both GTA IV and Episodes from Liberty City as `PPSA88360`: give one of them another id before both can be
installed.) A folder in `/data/homebrew` that isn't an R-comp title of the same id is never written over.

### Names and covers

The title ID, media ID and version come from the title's `image/plain.xex` (execution info), its name and 64×64
image from the XEX's XDBF resource. Display names and box art come from
[xenia-manager/x360db](https://github.com/xenia-manager/x360db) (`titles/<title ID>/info.json`,
`artwork/boxart.jpg`), fetched over HTTPS and cached.

Several products can share a title ID: Episodes from Liberty City is `545407F2`, like GTA IV. x360db lists every disc
with its own product name, so a disc is named after its media entry ("Grand Theft Auto: Episodes from Liberty City"),
and the title ID's box art is only used for discs of the product most of that title's discs belong to. Others show
the painted front (the XDBF image and the title) until you put a cover in `/data/rcomp/covers/<PPSA id>.jpg`.

### Controls

| Button | Does |
| --- | --- |
| D-pad / left stick | browse |
| L1 / R1 | jump five |
| Cross (or OPTIONS) | **Play** an installed title, **Install** a package |
| Triangle | **Update** an installed title from a newer package of it |
| Square | **Refresh**: look for titles and packages again (after plugging in a USB drive) |
| Circle | cancel an install in progress |

### Installing

A package in `/data/rcomp/packages` is *moved* into `/data/homebrew/<PPSA id>` (instant, same file system). A
package on a USB drive is *copied*, with progress, then renamed into place. An update moves the installed copy to
`/data/rcomp/trash/<PPSA id>-<time>` first; if the final rename fails the old copy is put back.

### Playing

Cross tears the shelf down (display, audio, controller) and asks the system to start the title's own application
(`sceLncUtilLaunchApp`). If that fails, the shelf comes back with the reason. Starting a title from its home-screen
icon always works the same as before.

## Building

All three builds read R-comp's toolchain and pinned inputs from an R-comp checkout (`RCOMP_ROOT`, default
`../rcomp`) without writing to it; their output goes under `build/` (git-ignored).

**PC preview and tests** (Windows: R-comp's full Cygwin; any machine with clang++ and a Vulkan driver):

```sh
VULKAN_HEADERS=<folder with vulkan/vulkan.h> tools/build_host.sh
build/host/shelf_tests
python3 tools/make_preview_data.py --out build/preview-data \
  --title "PPSA88360:installed:xex=<path to a plain.xex>" \
  --title "PPSA88361:package:synth=52430001:00000001:Test title"
build/host/shelf_host --data build/preview-data --out build/shots --online --lang 2 "sleep 3" "shot shelf"
```

`make_preview_data.py` copies only a real XEX's header and XDBF resource (zeros elsewhere); keep such data under
`build/` and never publish it.

**Console application**:

```sh
tools/build_ps5.sh        # build/ps5/dist/PPSA88300/
```

It is R-comp's RADV title link (`gpu/vulkan/ps5/build-radv-probes.sh`): the same pinned RADV archive, payload SDK,
compiler-rt, platform library, wrapped symbols, title CRT, linker script, converter and signer.

## Status

| Check | Status |
| --- | --- |
| Host tests (XEX/XDBF, JSON, x360db disc rule, catalog, installer: move, copy, update to trash, refusal, cancel, thread) | PASS (61 checks) |
| PC preview: shelf, x360db names and covers, install, refresh, play request (RX 9070 XT, Windows Vulkan) | PASS |
| Console application links, converts and signs; no W+X segment | PASS |
| Shelf on the PS5 (display, controller, sound, HTTPS) | NOT TESTED |
| Installing on the PS5, and ShadowMount+ showing a newly installed title without a reboot | NOT TESTED |
| Starting a title from the shelf (`sceLncUtilLaunchApp` from an application) | NOT TESTED |
| The shelf coming back to the front after the title closes | NOT TESTED |

Host validation is not PS5 evidence.

## Not ported from PS5SX2

The options sheet and the phone settings page (PCSX2 settings, patches, memory cards), USB keyboard and mouse,
the installer/updater payloads. R-comp titles have no settings the shelf could change yet.

## License

GPL-3.0-or-later ([COPYING.GPLv3](COPYING.GPLv3)), like PS5SX2. Third-party parts keep their licences:
[NOTICE.md](NOTICE.md).
