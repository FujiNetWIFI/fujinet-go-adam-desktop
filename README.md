# FujiNet Go Adam — Desktop

[![CI](https://github.com/FujiNetWIFI/fujinet-go-adam-desktop/actions/workflows/ci.yml/badge.svg)](https://github.com/FujiNetWIFI/fujinet-go-adam-desktop/actions/workflows/ci.yml)

A self-contained Coleco ADAM (and ColecoVision) with built-in
[FujiNet](https://fujinet.online/), for users and developers who want the
whole experience in one app: the `adamcore` emulator, the FujiNet-PC ADAM
firmware running in-process (joined over AdamNet Bus-over-IP on loopback),
the FujiNet web configuration UI, and a full native debugger.

The desktop sibling of the Android app
([fujinet-go-adam](https://github.com/FujiNetWIFI/fujinet-go-adam)).
Every target is a **native frontend** built from one shared core:

| Frontend | Toolkit | Binary | Status |
|---|---|---|---|
| GNOME | GTK4 + libadwaita (+ WebKitGTK) | `fujinet-go-adam-gnome` | complete |
| KDE | Qt6 Widgets (+ QtWebEngine) | `fujinet-go-adam-kde` | complete |
| macOS | AppKit (+ WKWebView) | `FujiNet Go Adam.app` | app complete; debugger views pending; CI-built, needs testers with Macs |

A native Windows frontend is planned against the same core API
(`core/include/adamsession.h`). The maintainer develops without a Mac:
the macOS build is compiled and tested on CI's Apple hardware — reports
from real Mac users are very welcome.

## Features

- ADAM / ColecoVision emulation (adamcore, clean-room GPLv3) at 59.922 Hz,
  phase-locked to your display's vsync whenever a ~60 Hz frame clock is
  available, wall-clock paced otherwise.
- The main view scales with correct aspect ratio (square-pixel 256:212,
  TV 4:3, or integer scaling) in tiling and floating window managers alike.
- In-process FujiNet: disk/DDP mounting, TNFS hosts, and network config
  through the embedded FujiNet web UI (`FujiNet ▸ Configuration`), plus a
  live console-log window.
- Automatic gamepad support with hotplug (SDL3; A/X = left fire,
  B/Y = right fire, left stick or d-pad). `Ctrl+digit` presses the game
  controller's keypad (game select on cartridges and tape games).
- Import `.dsk`/`.ddp` images into the FujiNet SD folder; load `.rom`/
  `.col`/`.bin` cartridges (boots in ColecoVision mode).
- Shared settings: palette, expansion module, joystick mode, aspect mode…
  are stored once (`~/.config/fujinet-go-adam/settings.ini`) and shared by
  both frontends.
- No on-screen input panels appear unless you ask for them.

### Developer debugger (F12)

Breakpoints, pause/step into/over/out, run-to, instruction history
(trace), a Z80 disassembler annotated with **EOS and OS7 symbols**
(generated from Richard F. Drushel's EOS-5 disassembly, the eoslib
jump-table names, and the os7lib listing — see `tools/symbols/`), memory
and register editing while paused, and live VDP views: nametable, pattern
banks, sprites (with SAT decode), and palette.

Keys: `F5` pause/continue · `F7` step into · `F8` step over ·
`Shift+F8` step out · click a disassembly line to toggle a breakpoint.
`ADAM_OPEN_DEBUGGER=1` opens the debugger at launch.

## Building (Linux)

Dependencies: CMake ≥ 3.20, a C/C++17 toolchain, SDL3, and per frontend:
GTK4 ≥ 4.10 + libadwaita ≥ 1.4 (+ `webkitgtk-6.0`, optional), or Qt6 ≥ 6.4
Widgets/OpenGLWidgets (+ WebEngine, optional). Frontends are
found-or-skipped; `-DFRONTEND=gnome|kde|all` selects explicitly and
`-DWITH_WEBVIEW=OFF` swaps the embedded web UI for the system browser.

1. **System ROMs** (not redistributed; see COMPLIANCE.md): drop your
   `EOS.rom`, `OS7.rom`, `WP.rom` into `tools/adamcore/roms/`. They are
   embedded into the binaries at build time.
2. **adamcore sources**: staged automatically at configure time from
   `~/Workspace/adamcore` (override with `ADAMCORE_SRC=…`).
3. **FujiNet runtime** (optional but the point of the app):
   `./tools/fujinet/build-fujinet-desktop.sh` builds `libfujinet.so` and
   the runtime tree from a local fujinet-pc-adam checkout
   (`FUJINET_SRC=…` to override) into `tools/fujinet/work/out/`.
4. Build and test:

   ```sh
   cmake -B build-all -G Ninja
   cmake --build build-all
   ctest --test-dir build-all
   ./build-all/frontends/gnome/fujinet-go-adam-gnome   # or …-kde
   ```

On first start the app provisions `~/.local/share/fujinet-go-adam/fujinet`
(fnconfig.ini, `data/`, `SD/`) from the build output or the installed
share directory, and finds `libfujinet.so` via `$FUJINET_LIB`, the install
libdir, or the dev build output.

Useful environment switches: `ADAM_PACE_LOG=1` (per-second frame pacing
diagnostics), `FUJINET_QUIET_BLOCKS=1` (suppress per-block disk log
lines), `FUJINET_WEBUI_BIND=addr:port` (web UI bind, default
`127.0.0.1:65214`), `ADAM_OPEN_DEBUGGER=1`,
`ADAM_DEBUGGER_TAB=vdp|trace` (debugger start tab).

### macOS

```sh
brew install cmake ninja sdl3
git clone https://github.com/tschak909/adamcore.git ~/Workspace/adamcore
# drop your ROMs into tools/adamcore/roms/ as above
cmake -B build -G Ninja && cmake --build build
open "build/frontends/macos/FujiNet Go Adam.app"
```

(`ADAMCORE_SRC=/path/to/adamcore` overrides the default checkout
location.) The FujiNet runtime build for macOS (`libfujinet.dylib`) is not
scripted yet; the app runs without it, minus the FujiNet drive.

### Flatpak

`packaging/flatpak/online.fujinet.go.adam.gnome.yml` builds the GNOME app
from the local tree (stage adamcore, drop in ROMs, and build the FujiNet
runtime first — see the manifest's header comments):

```sh
flatpak-builder --user --install --force-clean build-flatpak \
    packaging/flatpak/online.fujinet.go.adam.gnome.yml
```

## Repository layout

```
core/                 libadamsession: session, audio/gamepad (SDL3),
                      FujiNet runtime control, settings, debugger engine
core/adamcore-generated/  staged adamcore sources (git-ignored)
frontends/gnome/      GTK4/libadwaita app
frontends/kde/        Qt6 app
tools/adamcore/       staging + ROM embedding
tools/fujinet/        libfujinet.so build (desktop entry wrapper, patches)
tools/symbols/        EOS/OS7 debug-symbol extraction
```

## License

GPL-3.0-or-later (see LICENSE). Third-party provenance and the ROM policy
are documented in COMPLIANCE.md.
