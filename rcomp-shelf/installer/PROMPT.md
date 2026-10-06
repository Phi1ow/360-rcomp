# Prompt — build the "R-comp Installer" local web app

Copy everything below the line into the other AI. It is self-contained: it carries the console facts, the
file formats, and the three modes to build. Two reference files already exist and should be reused:
`pack.py` (XEX/XDBF reader + app-folder builder) and `ps5_deploy.py` (FTP install + chmod payload + launch);
both are in this folder. Paste their contents when you hand over the prompt, or point the AI at them.

---

You are building **R-comp Installer**, a local web application that runs on the developer's Windows PC and
turns a statically recompiled Xbox 360 game into a PlayStation 5 application and installs it on a jailbroken
PS5. Deliver a working tool, not a description. Do not fake or stub any step: if a step cannot run, make the
UI say so clearly (PASS / FAIL / BLOCKED / NOT TESTED), never report success you did not verify.

## Background (facts, not guesses)

- **R-comp** recompiles an Xbox 360 game ahead of time into a standalone native PS5 app: the game's code *is*
  the app's `eboot.bin`. There is no emulator and no menu on the console — each game is its own PS5
  application under `/data/homebrew/<PS5 title id>/`, shown on the PS5 home screen by the resident
  **ShadowMount+** payload. A game reads all its own files from `/app0` (its own package), so everything the
  game needs is packaged inside the app folder. The app runs sandboxed; it never needs general `/data` access.
- Installing a game therefore means: build + package it on the PC, copy the app folder to
  `/data/homebrew/<id>/` over FTP, and let ShadowMount+ register it. It then appears on the home screen with
  its own icon, exactly like a retail app.

## The PS5 app folder (what "an installed title" is)

```
/data/homebrew/<PPSA id>/
  eboot.bin                 the linked, fake-signed native executable (the recompiled game)
  sce_sys/param.json        app metadata (title id, content id, concept id, display name)
  sce_sys/icon0.png         512x512 home-screen icon
  sce_module/libc.prx       the C runtime module the eboot needs
  image/plain.xex           the decoded Xbox 360 executable (the app reads it from /app0/image/plain.xex)
  game/                     the extracted game files (the app reads them from /app0/game)
  manifest.sha256           sha256 of every other file, "<hex>  ./<relative path>"
```

`param.json` must carry: `titleId` = `PPSA` + 5 digits (each installed game needs a **unique** id — do not
reuse one that is already installed), `conceptId` = those 5 digits, `contentId` =
`UP9000-<titleId>_00-<16 chars>`, and `localizedParameters.defaultLanguage` = `en-US` with
`en-US.titleName` = the display name. A minimal valid example:

```json
{
  "applicationCategoryType": 0, "applicationDrmType": "free", "attribute": 0,
  "conceptId": "88370", "contentId": "UP9000-PPSA88370_00-RCOMP8837000000",
  "contentVersion": "01.000.000", "masterVersion": "01.00",
  "localizedParameters": { "defaultLanguage": "en-US", "en-US": { "titleName": "Grand Theft Auto IV" } },
  "requiredSystemSoftwareVersion": "0x0000000000000000", "sdkVersion": "0x0000000000000000",
  "titleId": "PPSA88370"
}
```

## Reading the XEX (for the title id, name and icon)

R-comp produces a decoded `image/plain.xex` (no encryption, no compression). Parse it to fill in the app's
metadata automatically:

- Big-endian. Header: magic `"XEX2"` at 0, header-data offset (`pe_off`) u32 at 8, key/value count u32 at 20,
  then `count` pairs {u32 key, u32 value} from offset 24.
- Key `0x40006` (execution info) → value is an offset in the header: media id u32 at +0, version u32 at +4,
  **title id u32 at +12** (format it `%08X`, e.g. `545407F2`), disc number u8 at +18, disc count u8 at +19.
- Key `0x10201` = image base address (u32). Key `0x3FF` = file format: at value+4 u16 encryption,
  at value+6 u16 compression — both must be 0 for a plain image.
- Key `0x2FF` = resource info: u32 total size at value+0, then 16-byte records {8-byte name, u32 address,
  u32 size}. The record whose name equals the title id string is the **XDBF** (title metadata). Its file
  offset is `pe_off + (address - image base)`.
- **XDBF**: magic `"XDBF"`, then u32 version, u32 entry-table length, u32 entry count, u32 free-table length,
  u32 free count. Entries follow: {u16 namespace, u64 id, u32 offset, u32 size}; data starts after both
  tables, offsets are from there. Namespace 1 id `0x58535443` ('XSTC') → u32 default language at +12.
  Namespace 2 id `0x8000` → a PNG (the title's image). Namespace 3 → a string table per language (the entry's
  id is the language): `"XSTR"`, u32 version, u32 size, u16 count, then {u16 string id, u16 length, bytes};
  string id `0x8000` is the title's name. Use the default language's name, else English (id 1).

Two products can share one title id (Grand Theft Auto IV and Episodes from Liberty City are both `545407F2`,
told apart by media id), so when the title id is reused the tool must let the user set a different PS5 title
id and a different name. Names and box art for display can also come from
`https://raw.githubusercontent.com/xenia-manager/x360db/main/titles/<title id>/info.json` and
`.../artwork/boxart.jpg` (not required for install).

## The console (developer's own jailbroken PS5)

- Address `<PS5_IP>`. Services, all provided by resident payloads loaded at boot:
  - **FTP** on port **2120** (read/write `/data`). It writes files mode `0666` and has no `SITE CHMOD`.
  - **ELF loader** on port **9021**: send a raw `.elf` and it runs it, **un-sandboxed** (full `/data` access).
  - **ps5vkctl** on port **9111**: one text line per connection — `status`, `procs`, `launch <id>`,
    `kill <id>`. `kill` refuses unless the running app is that title id.
  - **kernel log** stream on port **3232**.
  - **kstuff** (jailbreak; mounts each `/data/homebrew/<id>` so the app can run) and **ShadowMount+** (puts
    titles on the home screen) must already be loaded. Do not reimplement them.
- Because FTP writes `0666`, the PS5 refuses to exec `eboot.bin` (error `0x80aa001a`, errno 13). After
  uploading, send a tiny **chmod payload** to the ELF loader that does `chmod(…, 0777)` on every file under
  `/data/homebrew/<id>` (reference: `ps5_chmod_title` — a payload loaded via 9021 runs un-sandboxed, so its
  libc `chmod` reaches the real `/data`). Build it per title id with the ps5-payload SDK.
- **ShadowMount+ registration**: ShadowMount+ reads `/data/shadowmount/manual.lst` (one app folder path per
  line). To make a newly installed title appear on the home screen, add the line
  `/data/homebrew/<id>` to that file (back it up first: copy to `manual.lst.before-…`), then wait ~20 s for
  ShadowMount+'s next scan; confirm with `manual.status` (a line `installed\t<id>\t…`) and that
  `/system_ex/app/<id>` now exists over FTP. The install is only "done" once that registration is confirmed.

## The three modes (one local web app, a browser UI, a local server)

Build a **local web server** (Python 3 standard library only, no pip installs — Python 3.12 is available on
the PC, in both Git-bash and the project's Cygwin) that serves a single-page browser UI and JSON endpoints.
The UI has three tabs. Stream logs live to the page (PASS/FAIL/BLOCKED lines). Reuse `pack.py` and
`ps5_deploy.py`.

1. **Install a finished app** — the user picks an existing app folder (`eboot.bin`, `sce_sys/`, …). The tool
   FTP-uploads it to `/data/homebrew/<id>`, reads every file back and checks sha256, sends the chmod payload,
   adds the `manual.lst` line, waits for ShadowMount+ and confirms `/system_ex/app/<id>`. (`ps5_deploy.py`
   already does the upload + readback + chmod; add the manual.lst + confirmation step.)

2. **Package recompiled artifacts → app folder** — the user gives the already-produced `eboot.bin`, the
   decoded `image/plain.xex`, the `game/` folder and a `libc.prx`; the tool reads the XEX (title id, name,
   icon), lets the user override the PS5 title id and name (required when the title id is already taken), and
   assembles a clean app folder with `param.json`, `icon0.png` (from the XEX image, upscaled to 512, or a
   generated tile) and `manifest.sha256`. Then offer "Install" (mode 1). (`pack.py build` already does this.)

3. **Recompile from the ISO → app folder** — the user gives the Xbox 360 game ISO (or an extracted folder).
   The tool drives the **real R-comp pipeline** end to end and streams its output: extract the disc
   (`extract-xiso`), inventory + recompile the `default.xex` (XenonAnalyse/XenonRecomp), compile the
   generated C++ AOT and link it with the pinned RADV driver, then package (mode 2). This needs the full
   R-comp toolchain (the project's Cygwin with LLVM, CMake, Ninja, Python, the ps5-payload SDK and the pinned
   RADV build). It is long (tens of minutes to hours) and may fail on games R-comp does not yet support — the
   tool must show the real logs and a truthful status, never a fake success. Find the pipeline scripts in the
   R-comp checkout (`tools/`, `gpu/vulkan/ps5/build-radv-probes.sh`, the `build/prime-*` workspaces) and call
   them; do not reinvent the recompiler.

## Constraints

- Local tool for the developer's own machine and their own console. Never enter real credentials anywhere;
  never send anything to a third party except the public x360db raw files and the user's own PS5.
- Standard-library Python only on the host side (no pip). The browser UI is plain HTML/CSS/JS, no CDN needed.
- All generated output goes under a `build/` or output folder the user chooses; never write into the R-comp
  checkout. Never put game content, keys or SDK files in any repository.
- Honest status everywhere. Verify installs by reading back from the console; verify registration via
  `manual.status` and `/system_ex/app`.

## Deliverables

- `server.py` (local web server + the three endpoints, live log streaming), `static/index.html` (+ css/js),
  reuse of `pack.py` and `ps5_deploy.py`, a short `README.md` (how to run, what each mode needs), and a
  `requirements`-free setup. A test: package the sample `plain.xex` into an app folder and install it to the
  console, then confirm it on the home screen.
