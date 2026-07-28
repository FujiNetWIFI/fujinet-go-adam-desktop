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
  every frontend — GNOME preferences, the Qt settings dialog and the macOS
  Settings window (⌘,) all read and write the same keys.
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
lines), `FUJINET_WEBUI_BIND=addr:port` (force the web UI to a fixed bind
instead of the random free port picked each run), `ADAM_OPEN_DEBUGGER=1`,
`ADAM_DEBUGGER_TAB=vdp|trace` (debugger start tab).

The web admin normally binds a random free loopback port each run (the
FujiNet Configuration window always points at whichever one was picked) so a
second FujiNet-family process on the machine holding a fixed port can never
make it unreachable.

### Debugging FujiNet firmware

`libfujinet.so` is `dlopen`'d into the emulator process, not run as a
subprocess, so gdb can attach to it exactly like any other shared library once
it's loaded — the only reason this normally isn't useful is that the runtime
is always built Release, with no debug info. Build a Debug copy instead:

```sh
FN_DEBUG=1 tools/fujinet/build-fujinet-desktop.sh
FUJINET_LIB=tools/fujinet/work/out/libfujinet.so gdb --args \
    ./build-all/frontends/gnome/fujinet-go-adam-gnome
```

`FUJINET_LIB` (see above) points the session at that build instead of
whatever CMake normally provides, so the two can coexist — no need to rebuild
the main app. Because the library loads part-way through startup, set
breakpoints after it's mapped (`catch load libfujinet.so` before `run`, or
just interrupt with Ctrl-C once FujiNet's console output appears and set them
then). `FN_DEBUG=1` forces a rebuild if the tree was last built Release (and
vice versa), so switching back just needs a plain
`tools/fujinet/build-fujinet-desktop.sh`.

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
pacman -S --needed git make mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,SDL3} \
    mingw-w64-ucrt-x86_64-{python,python-yaml,python-jinja} \
    mingw-w64-ucrt-x86_64-{mbedtls,openssl,expat,zlib}
cmake -B build -G Ninja && cmake --build build
./build/frontends/windows/fujinet-go-adam-windows.exe
```

(`make` is not used for anything — the build runs Ninja — but FujiNet's
`build.sh` refuses to start without it on the PATH.)

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

## Cutting a release

Pushing a `v*` tag runs the normal CI build on every platform and, only if
all of it passes, publishes the packages it produced as release assets:

| Asset | Contents |
|---|---|
| `FujiNet-Go-Adam-<version>-macos.zip` | the `.app` bundle (SDL and FujiNet inside) |
| `FujiNet-Go-Adam-<version>-windows.zip` | the exe, `fujinet.dll`, and the `fujinet/` runtime tree |
| `FujiNet-Go-Adam-<version>.flatpak` | single-file bundle: `flatpak install ./…flatpak` |

The version is declared in the tree, not derived from the tag, so that a
downloaded build's About box can never disagree with the download it came
from — the release job checks the two match and stops if they do not. To
release 0.2.0:

1. `project(... VERSION 0.2.0 ...)` in `CMakeLists.txt` (the About boxes,
   the macOS bundle and the Windows folder all follow from there).
2. Add a `<release version="0.2.0" date="…"/>` entry to both
   `frontends/*/data/*.metainfo.xml` — this is what software centres show,
   so put the user-visible notes there.
3. Commit, then `git tag v0.2.0 && git push origin v0.2.0`.

The release is created as a **draft** so the notes can be edited before it
goes out; if you prefer to write the release in the web UI first, do that
and the assets are attached to it when the tag build finishes.

### Signing the macOS build

The macOS job signs and notarises the app when these repository secrets
exist, and quietly ships an unsigned bundle when they do not — so forks and
pull requests, which never receive secrets, still build:

| Secret | What it is |
|---|---|
| `MACOS_CERTIFICATE` | "Developer ID Application" certificate + key, exported as `.p12`, then `base64 -i cert.p12` |
| `MACOS_CERTIFICATE_PASSWORD` | the password used for that export |
| `MACOS_SIGN_IDENTITY` | e.g. `Developer ID Application: Your Name (TEAMID)` |
| `MACOS_NOTARY_KEY` | App Store Connect API key `.p8`, base64-encoded |
| `MACOS_NOTARY_KEY_ID` | that key's Key ID |
| `MACOS_NOTARY_ISSUER` | the Issuer ID from App Store Connect |

All of it requires a paid Apple Developer Program membership: a Developer
ID certificate cannot be issued without one, and notarisation is what
actually removes the Gatekeeper warning. Certificate but no notary key is a
valid halfway house (the app is signed and its origin verifiable, but first
launch still needs right-click ▸ Open); the build says so in a warning.
Signing runs on every build once the certificate secrets exist — cheap, and
it catches an expired certificate early — while notarisation only runs for
`v*` tags.

#### Producing those secrets without a Mac

Apple's own instructions assume Keychain Access, but nothing here needs
macOS — the certificate request, the `.p12` and the API key can all be made
on Linux.

**1. Certificate.** Generate a key and signing request, keeping the key
somewhere safe (losing it means revoking and reissuing):

```sh
openssl req -new -newkey rsa:2048 -nodes \
    -keyout devid.key -out devid.csr \
    -subj "/emailAddress=you@example.com/CN=Your Name/C=US"
```

Upload `devid.csr` at developer.apple.com ▸ Certificates, Identifiers &
Profiles ▸ Certificates ▸ **+** ▸ *Developer ID Application* (only the
account holder may create these), and download the resulting
`developerID_application.cer`. Also fetch the matching **Developer ID
Certification Authority** intermediate from
<https://www.apple.com/certificateauthority/> — without it in the bundle,
`codesign` on the runner cannot build a chain to Apple's root.

**2. Bundle them into a `.p12`.** The explicit algorithms matter: macOS
cannot import the AES/PBKDF2 PKCS#12 that OpenSSL 3 writes by default.

```sh
openssl x509 -inform DER -in developerID_application.cer -out devid.pem
openssl x509 -inform DER -in DeveloperIDG2CA.cer -out intermediate.pem
openssl pkcs12 -export \
    -keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1 \
    -inkey devid.key -in devid.pem -certfile intermediate.pem \
    -name "Developer ID Application" -out devid.p12
openssl x509 -in devid.pem -noout -subject   # the MACOS_SIGN_IDENTITY value
```

The subject's CN is the identity string, in the form
`Developer ID Application: Your Name (TEAMID)`.

**3. Notary key.** App Store Connect ▸ Users and Access ▸ Integrations ▸
App Store Connect API ▸ **Team Keys** ▸ **+**, with Developer access.
Download the `.p8` — it is offered exactly once — and note the Key ID from
the row and the Issuer ID from the top of the page.

**4. Load the secrets** (base64 so the binaries survive as text):

```sh
base64 -w0 devid.p12 | gh secret set MACOS_CERTIFICATE
gh secret set MACOS_CERTIFICATE_PASSWORD          # the -export password
gh secret set MACOS_SIGN_IDENTITY                 # from step 2
base64 -w0 AuthKey_XXXXXXXX.p8 | gh secret set MACOS_NOTARY_KEY
gh secret set MACOS_NOTARY_KEY_ID
gh secret set MACOS_NOTARY_ISSUER
```

The next push to `main` will sign (watch the macOS job's *Sign and notarise*
step for `codesign --verify` output); the next `v*` tag will additionally
notarise and staple.

`libfujinet.dylib` is signed first and the bundle second, both with the same
identity: the hardened runtime that notarisation requires only lets the app
`dlopen` a library signed by the same team.

Without any of this the shipped bundle is unsigned, and first launch needs
right-click ▸ Open (or `xattr -dr com.apple.quarantine "FujiNet Go Adam.app"`).

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

This repository is also the model for the other FujiNet Go desktop targets
(Apple II, CoCo, MSX). [PORTING.md](PORTING.md) distills the architecture,
the per-target parameters, and every lesson that cost real debugging time
into a guide for standing the next one up.

## License

GPL-3.0-or-later (see LICENSE). Third-party provenance and the ROM policy
are documented in COMPLIANCE.md.
