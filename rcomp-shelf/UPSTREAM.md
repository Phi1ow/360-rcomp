# Upstream: PS5SX2's frontend

Source: [Swordpdf/PS5SX2](https://github.com/Swordpdf/PS5SX2), branch `main`, commit
`d96a7031615734cbe4efb1b3953ddfc23e0dd3aa` (2026-10-01), folder `ps5/frontend/`. Author: Spyros
(Discord `sword.pdf`, X `@sword_pdf`). Licence: GPL-3.0-or-later. Used in R-comp with the author's agreement
(owner, 2026-10-02). His copyright and licence notices stay in every file taken from PS5SX2; on screen the
wordmark shows the R-comp author's handles instead (owner's choice, 2026-10-02).

## Files

| Here | Upstream | Change |
| --- | --- | --- |
| `src/frontend/fe_renderer.{h,cpp}`, `fe_text.{h,cpp}`, `fe_vk.{h,cpp}`, `fe_math.h`, `fe_sound.{h,cpp}`, `fe_shaders.inc`, `shaders/`, `build_shaders.sh` | same names | unchanged |
| `src/frontend/third_party/stb_image.h`, `stb_image_resize2.h`, `stb_truetype.h` | `third_party/` | unchanged |
| `src/frontend/fe_app.{h,cpp}` | `fe_app.{h,cpp}` | Play / Install / Update / Refresh / Cancel actions, install progress, messages, green house colour, R-comp wordmark with the R-comp author's handles; options sheet and QR tile removed |
| `src/frontend/fe_covers.{h,cpp}` | `fe_covers.{h,cpp}` | covers named after GameInfo's cover names and cover key (x360db), `cover.jpg` in the title folder, Xbox 360 spine and placeholder with the XDBF image; Open PS2 Loader folders removed |
| `src/frontend/fe_i18n.{h,cpp}` | `fe_i18n.{h,cpp}` | the shelf's strings for R-comp, in the same nine tables; override mechanism and regions kept |
| `src/frontend/fe_games.h` | `fe_games.h` | rewritten: an R-comp title instead of a PS2 disc image |
| `src/host/shelf_host.cpp` | `host/fe_host.cpp` | the R-comp catalog, installer and session; PNG without zlib; Windows `vulkan-1.dll` |
| `src/ps5/shelf_ps5.cpp` | `fe_ps5.cpp` | display, controller, sound and HTTPS kept; the R-comp catalog, installs and `sceLncUtilLaunchApp`; no settings page, no PCSX2 |
| `assets/fonts/Roboto-Regular.ttf`, `promptfont.otf` | PCSX2 `bin/resources/fonts/` (same commit) | unchanged |
| `assets/fonts/fa-brands-400.otf` | `assets/fonts/` | unchanged |

Not taken: `fe_options`, `fe_settings`, `fe_web`, `fe_games.cpp`, `assets/web/`, `assets/presets.ini`,
`third_party/qrcodegen`, `host/` scripts and tests, and everything outside `ps5/frontend/`.

New for R-comp (GPL-3.0-or-later): `src/catalog/` (XEX/XDBF, JSON, x360db, catalog, installer), `src/shelf/`
(the session shared by the console and the PC preview), `tests/`, `tools/`, `sce_sys/`.
