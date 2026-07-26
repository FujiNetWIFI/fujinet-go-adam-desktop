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
| macOS | AppKit (+ WKWebView) | `FujiNet Go Adam.app` | complete (incl. debugger + bundled FujiNet) |
| Windows | Win32 (GDI + DwmFlush) | `fujinet-go-adam-windows.exe` | complete (incl. debugger + bundled FujiNet); CI-built, needs Windows testers |

The maintainer develops on Linux without Mac or Windows hardware: those
builds are compiled and tested on CI's macOS and Windows runners (each
uploads a ready-to-run artifact), and reports from real users on those
platforms are very welcome.

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
`ADAM_OPEN_DEBUGGER=1` opens the debugger at launch. Available in every
frontend — GTK, Qt, AppKit and Win32 — over the one shared engine.

## Building (Linux)

Dependencies: CMake ≥ 3.20, a C/C++17 toolchain, SDL3, and per frontend:
GTK4 ≥ 4.10 + libadwaita ≥ 1.4 (+ `webkitgtk-6.0`, optional), or Qt6 ≥ 6.4
Widgets/OpenGLWidgets (+ WebEngine, optional). Frontends are
found-or-skipped; `-DFRONTEND=gnome|kde|all` selects explicitly and
`-DWITH_WEBVIEW=OFF` swaps the embedded web UI for the system browser.

There is nothing to fetch or stage by hand:

```sh
git clone https://github.com/FujiNetWIFI/fujinet-go-adam-desktop
cd fujinet-go-adam-desktop
cmake -B build-all -G Ninja
cmake --build build-all      # first build also builds FujiNet: a few minutes
ctest --test-dir build-all
./build-all/frontends/gnome/fujinet-go-adam-gnome   # or …-kde
```

What that does for you:

1. **System ROMs**: bundled in `tools/adamcore/roms/` (public domain; see
   COMPLIANCE.md) and embedded into the binaries at build time.
2. **adamcore sources**: the `third_party/adamcore` submodule is fetched at
   configure time (however the tree was obtained — clone, tarball or IDE
   source copy) and staged into `core/adamcore-generated/`.
3. **FujiNet runtime**: the `third_party/fujinet-firmware` submodule is
   fetched and built into `tools/fujinet/work/out/` (`libfujinet.so` plus
   the runtime tree) as part of the normal build, then installed with the
   app. `-DWITH_FUJINET=OFF` builds the bare emulator instead.

Both dependencies are pinned in `cmake/Dependencies.cmake`. To build
against working checkouts of your own, point the build at them:
`cmake -B build -DADAMCORE_SRC=~/Workspace/adamcore
-DFUJINET_SRC=~/Workspace/fujinet-pc-adam`; add `-DADAMCORE_RESTAGE=ON` to
pick up adamcore edits, and `cmake --build build --target
fujinet-runtime-refresh` to re-stage and rebuild FujiNet after editing its
sources.

On first start the app provisions `~/.local/share/fujinet-go-adam/fujinet`
(fnconfig.ini, `data/`, `SD/`) from the build output or the installed
share directory, and finds `libfujinet.so` via `$FUJINET_LIB`, the install
libdir, or the dev build output.

### Installing

Select the frontend at configure time and install:

```sh
cmake -B build-gnome -G Ninja -DFRONTEND=gnome   # or kde, or all
cmake --build build-gnome
sudo cmake --install build-gnome
```

This installs the binary, desktop entry, icons, and the FujiNet runtime
(`libfujinet.so` into `<prefix>/lib/fujinet-go-adam`, the pristine
runtime tree into `<prefix>/share/fujinet-go-adam/fujinet`). For a
sudo-free user install add `-DCMAKE_INSTALL_PREFIX=$HOME/.local` at
configure time (the FujiNet search paths bake in the prefix, so choose
it before building). Uninstall with
`xargs rm < build-gnome/install_manifest.txt`. GNOME and KDE installs
coexist: they share one settings store and FujiNet runtime.

Useful environment switches: `ADAM_PACE_LOG=1` (per-second frame pacing
diagnostics), `FUJINET_QUIET_BLOCKS=1` (suppress per-block disk log
lines), `FUJINET_WEBUI_BIND=addr:port` (web UI bind, default
`127.0.0.1:65214`), `ADAM_OPEN_DEBUGGER=1`,
`ADAM_DEBUGGER_TAB=vdp|trace` (debugger start tab).

### macOS

```sh
brew install cmake ninja sdl3
cmake -B build -G Ninja && cmake --build build
open "build/frontends/macos/FujiNet Go Adam.app"
```

The build fetches its dependencies and builds `libfujinet.dylib` into the
app bundle (`Contents/Frameworks`) along with the runtime tree.

Or skip building: every CI run uploads a ready-to-run
`FujiNet-Go-Adam-macos` app-bundle artifact with SDL statically linked
(no Homebrew needed to run it). It is unsigned: unzip, then
right-click ▸ Open the first time to get past Gatekeeper.

### Windows

Build in an [MSYS2](https://www.msys2.org/) UCRT64 shell (native Win32
frontend; SDL3 and the MinGW runtime are linked statically for a
dependency-free `.exe`):

```sh
pacman -S --needed git mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,SDL3} \
    mingw-w64-ucrt-x86_64-{python,python-yaml,python-jinja} \
    mingw-w64-ucrt-x86_64-{mbedtls,openssl,expat,zlib}
cmake -B build -G Ninja && cmake --build build
./build/frontends/windows/fujinet-go-adam-windows.exe
```

The second and third package lines are what FujiNet needs: the app builds
`fujinet.dll` and runs the same in-process FujiNet as the other platforms
(`-DWITH_FUJINET=OFF` for a bare emulator build without them).

Every CI run uploads a ready-to-run `FujiNet-Go-Adam-windows` artifact —
the exe, `fujinet.dll` beside it, and the FujiNet runtime tree in
`fujinet/`. Copy the folder anywhere and run it.

Windows changes are also cross-compiled (and smoke-tested under wine) from
Linux with the checked-in toolchain file — including `fujinet.dll`; see the
header comments in `cmake/toolchains/mingw-w64.cmake`.

### Flatpak

`build-aux/flatpak/online.fujinet.go.adam.gnome.yml` builds the GNOME app
with everything it needs (SDL3, mbedTLS, adamcore, the FujiNet firmware)
declared as sources, so it needs no preparation and no network access
during the build itself:

```sh
flatpak-builder --user --install --force-clean build-flatpak \
    build-aux/flatpak/online.fujinet.go.adam.gnome.yml
flatpak run online.fujinet.go.adam.gnome
```

This is also the manifest GNOME Builder picks up: open the project and
press Run.

## Repository layout

```
core/                 libadamsession: session, audio/gamepad (SDL3),
                      FujiNet runtime control, settings, debugger engine
core/adamcore-generated/  staged adamcore sources (git-ignored)
third_party/          pinned dependency submodules (adamcore,
                      fujinet-firmware), fetched by the build
cmake/                dependency provisioning, adamcore staging,
                      FujiNet runtime build
frontends/gnome/      GTK4/libadwaita app
frontends/kde/        Qt6 app
frontends/macos/      AppKit app
frontends/windows/    Win32 app
build-aux/flatpak/    flatpak manifest (also used by GNOME Builder)
data/icons/           launcher artwork + rendered hicolor icon set
tools/adamcore/       staging + ROM embedding
tools/fujinet/        libfujinet.so build (desktop entry wrapper, patches)
tools/icons/          icon rendering
tools/symbols/        EOS/OS7 debug-symbol extraction
```

## License

GPL-3.0-or-later (see LICENSE). Third-party provenance and the ROM policy
are documented in COMPLIANCE.md.
