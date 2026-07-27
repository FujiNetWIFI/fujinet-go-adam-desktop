# Porting this app to another FujiNet Go target

`fujinet-go-adam-desktop` is the **model repository** for the desktop side of
the FujiNet Go family. This document is the distillation of building it, so
that `fujinet-go-apple2-desktop` (then `-coco-`, `-msx-`) can be built by
following a pattern instead of rediscovering it.

Read it once end to end before creating the new directory. Sections 1–3 are
the design; 4–9 are the mechanical port; 10 is the catalogue of things that
cost real time; 11 is what is genuinely new work for each target.

---

## 1. What every `fujinet-go-<target>-desktop` is

One shared C core plus *N* native frontends. Nothing else.

```
  ┌──────────────┬──────────────┬──────────────┬──────────────┐
  │ GTK4/libadw  │  Qt6 Widgets │    AppKit    │    Win32     │  frontends:
  │   (GNOME)    │    (KDE)     │   (macOS)    │  (Windows)   │  windowing,
  └──────┬───────┴──────┬───────┴──────┬───────┴──────┬───────┘  painting,
         └──────────────┴──────┬───────┴──────────────┘          events only
                    ┌──────────┴──────────┐
                    │  lib<target>session │  emulator thread, audio, gamepad,
                    │      (core/)        │  settings, paths, debugger engine,
                    └──────────┬──────────┘  FujiNet control
              ┌────────────────┴────────────────┐
      ┌───────┴────────┐              ┌─────────┴──────────┐
      │ emulator core  │◄──loopback──►│ libfujinet.{so,     │
      │ (staged src)   │   TCP bus    │  dylib,dll} dlopen'd│
      └────────────────┘              └────────────────────┘
```

Non-negotiable properties, all of which the ADAM app has and the next one
should inherit:

1. **Native toolkits only.** GTK4/libadwaita on GNOME, Qt6 Widgets on KDE,
   AppKit on macOS, Win32 on Windows. No Dear ImGui, no Electron, no
   cross-platform UI layer. Each target must feel like a first-class citizen
   of its desktop.
2. **One shared C API** (`core/include/<target>session.h`). Frontends may
   only call that header. Anything a frontend needs that is not windowing,
   painting or event translation belongs in the core.
3. **Clone-and-build.** `git clone && cmake -B build && cmake --build build`
   produces a running app with FujiNet inside it. No "first run these three
   scripts". CI deliberately does nothing to prepare the tree, so this
   promise is tested on every push.
4. **FujiNet is in-process**, not a subprocess: the firmware is built as a
   shared library, `dlopen`'d, and joined to the emulator over loopback TCP.
5. **Self-contained artifacts.** The macOS `.app` and the Windows folder ship
   with SDL and FujiNet inside and depend only on system libraries. CI fails
   the build if a non-system library leaks in.
6. **One settings store** shared by every frontend of the target
   (`~/.config/fujinet-go-<target>/settings.ini`).
7. **A full native debugger in every frontend**, over one shared engine.
8. **No on-screen input panels visible at startup** — only on explicit user
   toggle.

### The derivation shortcut

Each desktop repo has an Android sibling that already solved the
target-specific half. The port is a two-way diff:

```
  <target>-desktop  =  (adam-desktop  −  adam-android)  +  <target>-android
```

- **From `adam-desktop`** (this repo): the desktop shape — CMake dependency
  provisioning, the four frontends, packaging, CI, release, the desktop entry
  wrapper, the paths layer.
- **From `fujinet-go-<target>` (Android)**: the target-specific half — which
  emulator core, how it is staged and driven, the transport and its port, the
  host callbacks (video/audio/input), the FujiNet PC target name, the web UI
  port.

Concretely, when starting `fujinet-go-apple2-desktop`:

| Take from | What |
|---|---|
| `adam-desktop/cmake/*` | verbatim, rename `ADAM_`/`adam_` → `APPLE2_`/`apple2_` |
| `adam-desktop/.github/workflows/ci.yml` | verbatim, rename artifacts |
| `adam-desktop/frontends/*` | structure + all the toolkit recipes; rewrite the display geometry and input mapping |
| `adam-desktop/tools/fujinet/build-fujinet-desktop.sh` | the desktop-specific patches (signals, exports, version, mbedTLS, MSYS2) |
| `apple2-android/tools/fujinet/build-fujinet.sh` | the APPLE-target specifics (PC target name, BoIP port 1985) |
| `apple2-android/tools/applewin/build-applewin-core.sh` | how to stage AppleWin and what to transform |
| `apple2-android/app/src/main/cpp/apple2_host.cpp` | the libretro host callbacks — port, do not reinvent |
| `adam-desktop/core/src/*` | session/pacing/settings/paths/audio/gamepad shape |

---

## 2. Per-target parameters

Everything that differs between targets, in one table. Fill the new column in
before writing any code; most of the port is substitution.

| | **ADAM** (this repo) | **Apple2** | **CoCo** | **MSX** |
|---|---|---|---|---|
| Emulator core | adamcore (clean-room C) | AppleWin libretro core (C++) | XRoar 1.11 (C) | openMSX (C++) |
| Core license | GPL-3.0-or-later | GPL-2.0-or-later | GPL-3.0-or-later | GPL-2.0-or-later |
| Core source | `tschak909/adamcore` submodule | `FujiNetWIFI/AppleWin` branch `linux` | XRoar release | `FujiNetWIFI/openmsx` branch `feat/fujinet` |
| Driven by | `adamcore_run_frame()` | `retro_run()` (libretro) | `xroar_run()` | `msxhost_core_run_frame()` |
| FujiNet PC target | `ADAM` | `APPLE` | `COCO` (DriveWire) | `RS232` |
| FujiNet checkout used for dev | `fujinet-pc-adam` | `fujinet-pc-apple2` | `fujinet-pc-coco` | `fujinet-pc-msx` |
| Transport | AdamNet Bus-over-IP | SmartPort-over-SLIP | Becker port (DriveWire-over-TCP) | FujiBusPacket-over-SLIP |
| Loopback port | **65216** | **1985** | **65504** | **1985** |
| Who listens | emulator listens, FujiNet connects in | emulator (`CT_SmartPortOverSlip`) listens, FujiNet connects in | **FujiNet listens**, XRoar connects out | **FujiNet listens**, openMSX connects out |
| Web UI port | 65214 | 8000 | 8002 | 8055 |
| Framebuffer | 256×212 RGB565 | (confirm from the Android host) | (confirm) | (confirm) |
| Frame rate | 59.922 Hz | ~59.94 Hz | ~59.94 / 50 Hz | 60 / 50 Hz |
| System ROMs | bundled, public domain, embedded | **Apple copyright — see §11** | **Tandy/Microware copyright** (HDB-DOS Becker ROMs are free) | C-BIOS (freely redistributable) |
| Joystick | digital, 2 ports + keypad | analog paddles | analog (2× 6-bit DAC) | digital |
| CPU (debugger) | Z80 | 6502/65C02 | 6809/6309 | Z80 |

**The listen/connect direction matters for startup ordering.** ADAM and
Apple2 have the emulator listening, so the session can start the emulator
first and FujiNet second, and a FujiNet restart reconnects on its own. CoCo
and MSX invert this: FujiNet must be up before the emulator's client tries to
connect, and an emulator restart is the thing that reconnects. Encode that in
`<target>session_start()` rather than hoping the race resolves.

---

## 3. Repository layout to replicate

```
CMakeLists.txt              version + frontend selection + icon install
COMPLIANCE.md               per-component licence provenance (§9)
README.md                   build/install/release docs, incl. the macOS
                            signing-without-a-Mac recipe
TODO                        rolling "done / next" notes
cmake/
  Dependencies.cmake        pinned commits + adam_provide_dependency()
  Stage<Core>.cmake         stage the emulator sources into core/*-generated
  FujiNetRuntime.cmake      build + install libfujinet
  toolchains/mingw-w64.cmake  cross-compile Windows from Linux
core/
  include/<t>session.h      THE frontend contract
  include/<t>debug.h        debugger engine contract
  src/session.c             paced emulator thread, frame publish
  src/settings.c            shared INI
  src/paths.c               XDG dirs + FujiNet runtime provisioning
  src/input_map.c           keysym → machine key
  src/audio_sdl.c           SDL audio out
  src/gamepad_sdl.c         SDL gamepad + hotplug
  src/fujinet_runtime.c     dlopen + the fujinet_desktop_* contract
  src/compat.h              clock/thread-name portability seams
  src/dynlib.h              dlopen vs LoadLibrary seam
  debugger/                 disassembler, symbols, video views
  tests/                    ctest suite
  <core>-generated/         staged emulator sources (git-ignored)
frontends/gnome/            GTK4 + libadwaita (+ WebKitGTK)
frontends/kde/              Qt6 Widgets (+ QtWebEngine)
frontends/macos/            AppKit (+ WKWebView)
frontends/windows/          Win32 (GDI + DwmFlush)
build-aux/flatpak/          flatpak manifests (GNOME Builder's convention)
build-aux/windows/          NSIS installer script
third_party/                pinned submodules
tools/<core>/               staging helpers, ROM embedding
tools/fujinet/              build-fujinet-desktop.sh + support/entry wrapper
tools/icons/                icon rendering from the Android launcher art
tools/symbols/              debug-symbol extraction (if the target has any)
data/icons/                 rendered hicolor set
data/screenshots/           for the AppStream metainfo
.github/workflows/ci.yml    linux / macos / windows / flatpak / release
```

Keep the names parallel across repos (`core/`, `frontends/`, `tools/`,
`build-aux/`). Divergence here costs more than it saves.

---

## 4. The build system

Three CMake modules do all the provisioning. They are target-agnostic except
for names, so copy them and substitute.

### 4.1 `cmake/Dependencies.cmake` — provide, don't require

`adam_provide_dependency()` resolves each dependency in three steps:

1. `<NAME>_SRC` cache variable or environment — an out-of-tree working
   checkout, which is how the dependencies are developed in tandem.
2. `third_party/<name>`, initialising the submodule
   (`--filter=blob:none`, falling back to a full fetch for servers that
   refuse partial clones).
3. An outright clone of the pinned commit, for trees with **no git
   metadata** — release tarballs and IDE source copies.

Two details that are easy to get wrong:

- **`SENTINEL`.** Test for a specific file inside the checkout
  (`src/machine.c`), not for the directory. An uninitialised submodule
  directory exists and is empty; it is otherwise indistinguishable from a
  populated one.
- **Pin drift warning.** Compare the checkout's HEAD to the pinned commit and
  say so when they differ. The FujiNet source patches are anchored to exact
  text and fail confusingly against a drifted tree.

Pins live *here*, and are repeated verbatim in the flatpak manifest.

### 4.2 `cmake/Stage<Core>.cmake` — stage, don't build in place

The emulator sources are copied into `core/<core>-generated/` and compiled by
`core/CMakeLists.txt` as if they were ours. This mirrors what the Android app
does (`cpp-generated/`) and lets the core be built with our own flags.

- Staging is **done in CMake, not a shell script**: MSYS2/MinGW CMake cannot
  `execute_process` a `.sh` ("inappropriate file type or format").
- A `.source-info` marker records the source commit; staging re-runs
  automatically when it changes, or on `-D<CORE>_RESTAGE=ON` (which is how
  uncommitted edits in a `*_SRC` checkout get picked up).

For AppleWin/openMSX/XRoar the staging step also applies the handful of
idempotent CMake edits that let the upstream tree build as a *subdirectory*
of ours — the Android scripts already enumerate them; reuse that list.

### 4.3 `cmake/FujiNetRuntime.cmake` — FujiNet is part of the normal build

- `option(WITH_FUJINET ... ON)`; `-DWITH_FUJINET=OFF` gives a bare emulator.
- The recipe is a bash script, always invoked **through `bash` by name**
  (`find_program(BASH_EXECUTABLE bash)`), never as a bare `.sh` — same MSYS2
  reason as above.
- A `configure_file(... COPYONLY)` stamp holds the source dir + commit.
  `configure_file` leaves the file alone when the content is unchanged, so
  re-configuring does **not** trigger a multi-minute FujiNet rebuild.
- `add_custom_target(fujinet-runtime ALL DEPENDS ${FUJINET_LIB})`, plus a
  `fujinet-runtime-refresh` escape hatch that re-stages and rebuilds
  unconditionally for people editing the FujiNet sources.
- Install layout differs by platform: on Windows the DLL goes beside the exe
  and the runtime tree under it (a folder you copy); elsewhere the library
  goes to `<libdir>/fujinet-go-<target>` and the pristine tree to
  `<datadir>/fujinet-go-<target>/fujinet`.

### 4.4 Top-level `CMakeLists.txt`

- **The version lives in `project(... VERSION x.y.z)` and nowhere else.**
  About boxes, the macOS bundle, the Windows installer and the release check
  all read it from there.
- **`include(GNUInstallDirs)` at the top level.** It caches
  `CMAKE_INSTALL_DATADIR` as an empty string and only sets the `share`
  fallback in the *including* scope. Include it in `core/` alone and the
  frontends get an empty DATADIR, so `${CMAKE_INSTALL_DATADIR}/applications`
  resolves to the absolute `/applications` — outside the prefix. This one
  silently produces a broken install.
- Frontend selection: `FRONTEND=all` (default) considers only the *host's*
  viable frontends, so a Windows or macOS configure never descends into the
  pkg-config Linux frontends. Naming one explicitly still forces it.
- Icons install once per frontend app-id, at every hicolor size — desktop
  environments look the icon up by desktop-entry id.
- **The AppKit frontend needs its own `.icns`, and it's easy to ship
  without one.** GNOME/KDE find the icon by desktop-entry id via the
  hicolor install above, and Windows embeds `app.ico` through
  `resource.rc` — neither of those wires up macOS, so an AppKit port that
  only copies the hicolor-install loop ends up with no dock/Finder icon
  and no build error, since `MACOSX_BUNDLE_INFO_PLIST` happily configures
  a plist with no `CFBundleIconFile`. `tools/icons/make-icons.py` also
  writes `data/icons/<app>.icns` (Pillow's ICNS writer packs plain PNGs
  into the TOC, so this needs no `iconutil`/macOS host to generate). Wire
  it up in the frontend's `CMakeLists.txt`: add the `.icns` as a source
  with `MACOSX_PACKAGE_LOCATION "Resources"` so CMake copies it into the
  bundle, and set the target property `MACOSX_BUNDLE_ICON_FILE` to its
  filename — that property is what `Info.plist.in`'s
  `${MACOSX_BUNDLE_ICON_FILE}` substitutes into `CFBundleIconFile` (same
  mechanism as the existing `${MACOSX_BUNDLE_EXECUTABLE_NAME}`).

### 4.5 `.gitignore`

`build*/` swallows `build-aux/` — which is *source*, not output. Negate it
(`!build-aux/`) or new manifests never show up as untracked and quietly go
missing from CI.

---

## 5. The FujiNet embedding contract

This is the highest-value part of the port and the part with the most
non-obvious failure modes. The entry wrapper
(`tools/fujinet/support/fujinet_desktop_entry.cpp`) and the patch set in
`build-fujinet-desktop.sh` are ~95% target-independent.

### 5.1 The ABI the session talks to

Six symbols, `dlsym`'d out of the library:

```c
bool        fujinet_desktop_start_runtime(root, config, sd, data, listen_port);
void        fujinet_desktop_stop_runtime(void);
const char *fujinet_desktop_last_error_message(void);
int         fujinet_desktop_read_audio(int16_t *out, int max, int rate);
void        fujinet_desktop_clear_audio(void);
int         fujinet_desktop_copy_recent_log(char *out, int max_bytes);
```

The first three are mandatory; the audio/log three are optional
(`NULL` = feature absent). Keep these names identical across targets — the
session code then ports with zero edits.

### 5.2 Rules

- **Never `dlclose` the library.** It owns background threads (web admin,
  network listeners) whose code lives in its mapping; unmapping it while one
  runs executes freed memory. Load once per process, restart through the same
  handle, and on a broken contract just mark the handle unusable.
- **Export control is per linker**, and on Linux it is not merely cosmetic:
  - ELF: a version script (`local: *;`) exporting only `fujinet_*`. Required
    for a clean `dlsym` surface **and** because a system static mbedTLS is
    built without `-fPIC` — its PC32 relocations only link when the
    referenced symbols are local.
  - Mach-O: `-exported_symbols_list` (always PIC, so only the API surface
    matters). Remember the leading underscore on C symbols.
  - PE: no equivalent, so the entry points carry
    `extern "C" __declspec(dllexport)` — which also stops MinGW
    auto-exporting every symbol in the library.
- **Force `fnconfig.ini` to the loopback endpoint** the emulator uses:
  `[BOIP] enabled=1 host=127.0.0.1 port=<target port>`.
- **Bind the web admin to 127.0.0.1 by default** on desktop (the Android
  builds use `0.0.0.0`), with an env override.

### 5.3 The patches applied to the staged firmware tree

Target-independent (carry these over as-is):

| Patch | Why |
|---|---|
| `src/main.cpp`: no `signal()`/`atexit` under `FUJINET_EMBEDDED`, and `SDL_HINT_NO_SIGNAL_HANDLERS` | in-process, those handlers hijack the **host app's** SIGINT/SIGTERM, so a plain `kill` shuts down the FujiNet service loop and leaves a wedged half-alive app |
| `fnSystem`: add `clear_shutdown_request()`, make `reboot()` ask the service loop to stop | the stock path calls `exit()`, which kills the host app |
| `fujinet_pc.cmake`: SHARED library target including the entry wrapper | instead of an executable |
| `build.sh`: read extra cmake args from an environment variable | its `getopts` rejects pass-through `-D` flags outright |
| `version_common.py`: fall back when git fails | the staged tree has no `.git`; two helpers let the failure escape and abort the build. Pass the real description in through the environment so the banner still names the upstream commit |
| upstream `PC_BUILD` typo | never set, so every PC build tried to `pip install` PlatformIO |
| mbedTLS resolution: honour `MBEDTLS_ROOT_DIR` by explicit path | `find_library` does not descend into `<root>/lib` from a `HINTS` directory |
| `dist` target: skip the `ldd` DLL harvesting for embedded Windows builds | the embedded library links its runtimes statically and has nothing to harvest; `ldd` is meaningless when cross-compiling |
| `pc_rtos`: name worker threads after their FreeRTOS task | debuggable core dumps (optional) |

Target-**dependent** (expect to find a new set):

| Patch | ADAM instance | What to look for on a new target |
|---|---|---|
| `uint` spelling | `lib/device/adamnet/modem.h` | glibc/BSD-isms in `lib/device/<bus>/` — that device tree has usually never been compiled for MinGW |
| `setenv()` → `_putenv_s` shim | `lib/device/adamnet/adamFuji.cpp` | the same shim `lib/clock/Clock.cpp` already carries; apply wherever the target's Fuji device calls `setenv` |

Both are worth sending upstream rather than carrying forever.

### 5.4 Script mechanics that matter

- **Read and write the staged sources as UTF-8 with `surrogateescape` and no
  newline translation.** Python on Windows otherwise decodes them as cp1252
  (a stray `0x90` in the firmware tree kills the build) and converts every LF
  to CRLF on the way out, corrupting the shell scripts it just patched.
- **Normalise paths with `cygpath -m` up front** under MSYS2: the script runs
  in an MSYS shell but drives *native* Windows cmake/ninja/python, which
  cannot follow `/d/a/...` paths.
- **Create the Python venv with `--system-site-packages`.** The FujiNet build
  pip-installs pyyaml/jinja2 when missing, which needs network and fails in a
  build sandbox; with system site packages the distribution's modules satisfy
  the check and pip only runs when they are genuinely absent.
- **Keep the build directory across runs** (no `-c`), use Ninja and a
  parallel level. Force a clean rebuild when the generator changed, or when a
  `.adam-target`-style key (library name + cmake args + mbedTLS root) shows
  the tree was last built for a different target — one work tree serves both
  the host build and the mingw cross build, and their caches are not
  interchangeable.
- **mbedTLS must be 3.x.** 4.x drops the legacy `mbedtls/md5.h` FujiNet
  includes, and Homebrew has moved to 4.x. Use the system copy when it is a
  usable 3.x with static libraries (Linux distributions ship one), otherwise
  build the pinned 3.6.5 with `MBEDTLS_THREADING_C`/`_PTHREAD` enabled —
  FujiNet drives TLS from several threads.

### 5.5 Runtime discovery and provisioning

The session finds the library and the pristine runtime tree in this order:

1. `$FUJINET_LIB` / an explicit path from the frontend (the macOS app passes
   its bundle's `Contents/Frameworks` and `Resources/fujinet`).
2. **The directory holding the executable** — this is what the Windows
   artifact ships (exe + `fujinet.dll` + `fujinet/`).
3. The install libdir/datadir, baked in at configure time.
4. The dev build output (`tools/fujinet/work/out`), so a git checkout runs
   with no install step.

On first start the user's runtime tree (`~/.local/share/fujinet-go-<target>/
fujinet`: `fnconfig.ini`, `data/`, `SD/`) is provisioned from the pristine
copy. On Windows use `LOAD_WITH_ALTERED_SEARCH_PATH` so the library's own
directory is searched for *its* dependencies.

---

## 6. The session core

### 6.1 Threading and pacing

The emulator runs on its **own thread**, never on the UI thread:

- It free-runs at the machine's exact frame rate on an absolute-time sleeper.
- Frontends call `<t>session_notify_vsync(s, frame_time_ns)` from their
  display callback. While a steady ~60 Hz tick stream arrives, the emulator
  **phase-locks one emulated frame per tick**; otherwise it falls back to the
  wall clock. This is what makes it smooth in both tiling and floating window
  managers, and on machines with no usable frame clock.
- Frames are published with a **serial number**; `copy_frame(dst, &serial)`
  copies only when the serial differs and returns 0 otherwise. Pass serial 0
  to force a copy after a window map.
- `ADAM_PACE_LOG=1`-style once-per-second diagnostics (frames, frames behind,
  frames on vsync) pay for themselves the first time pacing looks wrong.

`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` is Linux-only;
`pthread_setname_np` has three different signatures. Both go behind
`core/src/compat.h`.

### 6.2 The frontend contract

Model the new header on `core/include/<t>session.h`. The shape that worked:

- opaque `session *` + a `paths` struct where every member is optional
- settings get/set by key (int and string) + flush
- a `start_opts` struct filled from settings by `default_opts()`
- `start` / `stop` / `is_running` / `last_error`
- `copy_frame` / `notify_vsync`
- key and joystick input, plus a pure function
  `<t>_key_from_event(keysym, unicode, ctrl)` → machine key code, shared by
  every frontend and unit-tested on its own
- gamepad enumeration/assignment, audio pull, FujiNet status/log/URL
- `import_media()` — one entry point that routes a dropped file to the right
  place (disk images to the FujiNet SD folder, cartridges to the cart dir)
- `debugger()` — lazily created engine handle

Frontends translate their toolkit's key events to X11/xkb keysyms (GDK
keyvals are already that; Qt and Win32 map through a small table) so one
mapping table serves all four.

---

## 7. Frontend recipes

Each frontend is ~400–900 lines. The per-toolkit knowledge worth copying:

| Concern | GNOME (GTK4) | KDE (Qt6) | macOS (AppKit) | Windows (Win32) |
|---|---|---|---|---|
| Display widget | `GtkDrawingArea` + Cairo/GdkTexture | `QOpenGLWidget` | `NSView` + CALayer | GDI `StretchDIBits` |
| Vsync tick source | `GdkFrameClock` tick callback (GTK4 presents on the compositor's vsync) | `paintGL` with swap interval 1, self-scheduling | `CVDisplayLink` | `DwmFlush()` on a present thread |
| Web UI | WebKitGTK 6.0 (optional) | QtWebEngine (optional) | `WKWebView` | system browser |
| Settings UI | `AdwPreferencesWindow` | `QDialog` | Settings window (⌘,) | dialog |
| Deps | `pkg_check_modules` | `find_package(Qt6 ... QUIET)` | frameworks | link libs |

- **Found-or-skipped.** Every Linux frontend `return()`s with a `STATUS`
  message when its toolkit is missing, so a lean machine still builds the
  core, the tests, and the other frontend.
- **Optional webview.** `WITH_WEBVIEW=OFF`, or the package simply not being
  installed, falls back to opening the FujiNet config in the system browser.
  Never make the web engine a hard dependency — QtWebEngine in particular is
  enormous and absent on many systems.
- **KDE uses plain Qt6 Widgets**, not KDE Frameworks: it picks up Breeze
  through the platform theme, and staying framework-free keeps the door open
  for reusing the Qt frontend elsewhere.
- **Aspect handling** belongs in the display widget: square-pixel, TV 4:3 and
  integer scaling, letterboxed, with a smooth-scaling toggle. Store the mode
  in the shared settings so all frontends agree.
- **`WIN32_EXECUTABLE`** ⇒ `WinMain`, no console window. Generate the app
  manifest from `PROJECT_VERSION` with `configure_file` (comctl32 v6 +
  per-monitor DPI) rather than repeating the version.
- **macOS bundle assembly** is a `POST_BUILD` custom command: dylib into
  `Contents/Frameworks`, runtime tree into `Contents/Resources/fujinet`,
  `add_dependencies` on the `fujinet-runtime` target so the ordering is real.

### The debugger

One engine in `core/debugger/` (breakpoints, stepping, run-to, trace,
disassembler, symbol table, video-memory views) with four thin native
windows. Keys are the same everywhere: F12 open, F5 pause/continue, F7 step
into, F8 step over, Shift+F8 step out, click a disassembly line to toggle a
breakpoint. `<T>_OPEN_DEBUGGER=1` and `<T>_DEBUGGER_TAB=` open it at launch —
invaluable when the app crashes before you can reach the menu.

**Write the disassembler fresh, or take one whose licence you have checked.**
For ADAM this was forced: ADAMEm's `z80dasm` is non-commercial-licensed and
GPL-incompatible, so the Z80 disassembler here is a fresh implementation from
the Zilog manual. For Apple2/CoCo/MSX the emulator cores are GPLv2+/GPLv3, so
their own disassemblers *can* be reused — check before reimplementing a 6502
or 6809 disassembler for no reason.

---

## 8. Packaging, CI and release

### 8.1 CI jobs

`linux` · `macos` · `windows` · `flatpak` (matrix over frontends) · `release`.

The jobs deliberately do nothing to prepare the tree beyond checkout, so CI
exercises exactly what a fresh clone gets. What each job proves:

- **linux** — the plain user path; also `nm -D` the built library to confirm
  the `fujinet_desktop_*` entry points are actually exported.
- **macos** — substitute for owning a Mac. Builds, tests, signs (when secrets
  exist), notarises (tags only), and uploads a ready-to-run bundle.
  **Verifies self-containment with `otool -L` over every Mach-O in the
  bundle** and fails if anything under `/opt/homebrew` or `/usr/local`
  leaked.
- **windows** — MSYS2/UCRT64. Builds the exe *and* `fujinet.dll`, then
  verifies both against a whitelist of Windows system DLLs.
- **flatpak** — the strictest test of the clone-and-build promise: the
  sandbox has no network during the build, so anything the manifest does not
  declare is simply absent.
- **release** — runs only for `v*` tags and only after all four pass.

### 8.2 Self-containment checks (both non-negotiable)

- **Windows: check the import table (`objdump -p`), not `ldd`.** `ldd`
  resolves against the *build machine's* PATH, where the MSYS copies exist,
  so it reports dependencies as satisfied that the target will not have.
  `fujinet.dll` shipped importing `libcrypto-3-x64.dll` and `zlib1.dll` this
  way: the exe launched, `LoadLibrary` failed, and the ADAM quietly fell
  through to SmartWriter with no FujiNet and no error anyone could see.
  Keep the whitelist regex; add `ldd` on the exe as a second net for
  transitively pulled runtimes.
- **macOS: `otool -L` every Mach-O in the bundle.** A bundle that links
  `/opt/homebrew` paths only launches on machines that happen to have that
  Homebrew formula installed.

Link SDL3 **statically** on macOS and Windows for this reason (and
`-static -static-libgcc -static-libstdc++` on MinGW — including for the
*test* binaries, which otherwise need `libwinpthread-1.dll` beside them and
only run inside an MSYS2 shell).

### 8.3 Flatpak

- Manifests live in `build-aux/flatpak/` — GNOME Builder's convention, so
  Builder finds the project and its Run button works.
- Everything is a declared source: SDL3, mbedTLS 3.6.5 (+ the `sed` enabling
  threading), the PyYAML/Jinja2/MarkupSafe **sdists** (the GNOME SDK has
  openssl/expat/krb5 but not yaml/mbedtls/rsync), and both dependency repos
  at the same commits pinned in `cmake/Dependencies.cmake`. The app itself is
  `type: dir` with `third_party/` skipped.
- Drive `flatpak-builder` directly in CI rather than the community container
  action, whose tags stop at gnome-47. Pass `--disable-rofiles-fuse` (FUSE is
  not available to the runner user).
- Runtimes go EOL fast (GNOME 48 was retired in March 2026). Note the bump in
  TODO and check with `flatpak remote-ls flathub --runtime`.

### 8.4 Release

- **The version is declared in the tree, not derived from the tag**, so a
  downloaded build's About box can never disagree with the download it came
  from. The release job checks that the tag matches `project(... VERSION)`
  **and** that every `metainfo.xml` has a matching `<release>` entry, and
  stops if not.
- Releases are created as **drafts** with `--generate-notes`; if a release
  already exists (written in the web UI first), it is left alone and only the
  assets are attached.
- Assets: macOS zip, Windows zip + NSIS installer (both from the same staged
  folder, so the two downloads are the same bits), one flatpak bundle per
  frontend.

### 8.5 macOS signing without a Mac

The full recipe is in the README and is entirely reusable — the certificate
request, the `.p12` and the App Store Connect API key can all be produced on
Linux with `openssl`. Two details that waste an afternoon otherwise:

- Export the PKCS#12 with `-keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES
  -macalg sha1`. macOS cannot import the AES/PBKDF2 container OpenSSL 3
  writes by default.
- Include the **Developer ID Certification Authority intermediate** in the
  bundle, or `codesign` on the runner cannot build a chain to Apple's root.

Sign the **dylib first, then the bundle, with the same identity**: the
hardened runtime that notarisation requires only lets the app `dlopen` a
library signed by the same team. Signing runs on every build (cheap, catches
an expired certificate early); notarisation only on `v*` tags. With no
secrets at all the job is a no-op and ships an unsigned bundle, so forks and
pull requests stay green.

### 8.6 Tests

Keep a real `ctest` suite — it is what makes the macOS and Windows CI jobs
meaningful when you cannot run those platforms yourself:

- input mapping, disassembler, boot smoke, debugger-boot, gamepad mapping,
  video decode
- `SKIP_RETURN_CODE 77` on the boot-dependent ones so ROM-less
  configurations still pass
- `desktop-file-validate` and `appstreamcli validate --no-net --pedantic` as
  tests, so a malformed launcher is caught at build time rather than after
  installing

---

## 9. Compliance

Write `COMPLIANCE.md` **before** the first public build, with a row per
component: origin, licence, and how it enters the build. Also record what was
deliberately *not* used and why — for ADAM that is ADAMEm (non-commercial
licence, GPL-incompatible; no code from it is present, including its
disassembler) and Gearcoleco (consulted as a feature checklist only).

Each target's ROM story differs and is the part that needs a real decision:

- **ADAM** — EOS/OS7/WP determined to have lapsed into the public domain by
  the maintainer; bundled and embedded, with a documented "delete them and
  the build embeds zero-filled placeholders" escape.
- **Apple2** — the monitor/Applesoft/Disk ][ ROMs are **Apple copyrighted
  firmware and not freely licensed**. The Android app embeds them via
  AppleWin's `apple2roms` resource target and its COMPLIANCE.md flags this as
  unsuitable for public distribution. See §11.
- **CoCo** — the Color BASIC / Extended / Super Extended ROMs are
  Tandy/Microware copyright, with the same caveat as Apple2. The **HDB-DOS
  Becker** ROMs (`hdbdw3bck`, `hdbdw3bc3`) are freely redistributable and the
  FujiNet link needs them, so those can be bundled either way.
- **MSX** — C-BIOS is freely redistributable; ship it (MSX/MSX2/MSX2+ boot
  with no copyrighted ROMs at all) and let users import real system ROMs for
  turboR and real machines. **This is the model to imitate** where a target
  has a free BIOS available.

Debug-symbol tables (names + addresses extracted from public disassemblies)
are tables of *facts* and contain no program code — worth stating explicitly
in COMPLIANCE.md, as this repo does for EOS/OS7.

---

## 10. The gotcha catalogue

Everything that cost real debugging time, in one list.

**Build system**

1. `GNUInstallDirs` in a subdirectory silently yields `/applications` outside
   the prefix. Include it at the top level.
2. `build*/` in `.gitignore` swallows `build-aux/`; new manifests then never
   appear as untracked.
3. MSYS2 CMake cannot `execute_process` a `.sh`. Do directory work in CMake;
   invoke scripts through `bash` by name.
4. An empty submodule directory looks exactly like a populated one — test for
   a sentinel file.
5. `configure_file(COPYONLY)` for stamps, so re-configuring does not trigger
   the multi-minute FujiNet rebuild.
6. A FujiNet build directory made by a different generator, or for a
   different target, cannot be reused — detect and clean.

**FujiNet in-process**

7. Never `dlclose`: background threads live in the library's mapping.
8. FujiNet's `signal()`/`atexit` handlers hijack the host app's signals; a
   plain `kill` then shuts down FujiNet and wedges the app. Gate them out and
   set `SDL_HINT_NO_SIGNAL_HANDLERS`.
9. `reboot()` calls `exit()` — guard it to a service-loop stop request.
10. The ELF version script is required not just for a clean `dlsym` surface
    but because a non-PIC system static mbedTLS only links when the
    referenced symbols are local.
11. MinGW auto-exports every symbol unless the entry points are marked
    `__declspec(dllexport)`.
12. `build.sh`'s `getopts` rejects pass-through `-D` flags — use an env var.
13. `version_common.py` aborts without git; the staged tree has none.
14. Upstream's never-set `PC_BUILD` made every PC build try to pip-install
    PlatformIO.
15. `find_library` will not descend into `<root>/lib` from `HINTS` — resolve
    `MBEDTLS_ROOT_DIR` by explicit path.
16. mbedTLS 4.x drops `mbedtls/md5.h`; you need 3.x, with threading enabled.
17. Python on Windows decodes the firmware tree as cp1252 and rewrites LF to
    CRLF, corrupting patched shell scripts. UTF-8 + `surrogateescape` +
    `newline=''`.
18. `pip` in a build sandbox has no network — create the venv with
    `--system-site-packages`.
19. Expect a fresh set of MinGW portability gaps (`uint`, `setenv`) in the
    *target's* device tree; nobody has compiled it for Windows before.

**Runtime**

20. A stray standalone `fujinet-pc-<target>` service on the same machine
    steals the loopback port and produces a metronomic ~5 s
    connect/disconnect loop that looks exactly like a bus regression. Its
    `fnconfig.ini` often has an *empty* `port=` and falls back to the same
    default. Check `ps aux | grep fujinet` and `ss -tnp` **first**. Tells:
    more "new-accept" drops than FujiNet "connected" lines, and the first
    drop reporting a non-idle state before the embedded FujiNet connected at
    all.
21. On the ADAM, FujiNet's boot DDP mounts from the `data/` (SPIFFS) dir, not
    the SD — expect a similar target-specific asset location.

**Windows**

22. `ldd` lies about a cross/MSYS build's dependencies; check the import
    table. This shipped a broken FujiNet DLL once — the app launched and
    silently ran without FujiNet.
23. Link SDL and the MinGW runtime statically, for the test binaries too.
24. `LOAD_WITH_ALTERED_SEARCH_PATH` so a DLL finds its own neighbours.
25. Normalise MSYS paths with `cygpath -m` before handing them to native
    tools.
26. Cross-compile from Linux with the checked-in toolchain and smoke-test
    under wine — that is how Windows changes get validated before CI, when
    there is no Windows box.

**macOS**

27. Homebrew's mbedTLS is 4.x; build the pinned 3.6.5.
28. Static SDL, or the bundle only runs on machines with that Homebrew
    formula.
29. Sign the dylib before the bundle, same identity, or the hardened runtime
    refuses to `dlopen` it.
30. macOS cannot import OpenSSL 3's default PKCS#12; use the SHA1-3DES
    algorithms and include the intermediate certificate.
31. `ditto -c -k --keepParent`, not a plain artifact upload of a `.app`
    directory (which loses executable bits and structure).

**Pacing / display**

32. Phase-lock to the UI's frame clock when one exists, wall clock otherwise.
    Feed ticks from `GdkFrameClock` / `paintGL` / `CVDisplayLink` /
    `DwmFlush`.
33. Publish frames with a serial and skip unchanged copies.
34. `clock_nanosleep(TIMER_ABSTIME)` and `pthread_setname_np` need compat
    seams.

---

## 11. What is genuinely new work for Apple2

Everything above is substitution. These need design:

1. **The core is a libretro core, not a small C API.** AppleWin is C++, large,
   and driven through `retro_run()` with environment/video/audio/input
   callbacks. `core/src/session.c`'s "call `<core>_run_frame()` on a paced
   thread" shape still holds, but the layer underneath it is the libretro
   host. **Port `apple2-android/app/src/main/cpp/apple2_host.cpp`** rather
   than writing it again — it already maps libretro video/audio/input onto a
   host, and the desktop host differs only in where the pixels and samples
   go.
2. **Staging AppleWin is heavier than staging adamcore.** It needs a subset
   of the tree (`source`, `resource`, `libyaml`, `minizip`, `bin`), a few
   idempotent CMake edits to build as a subdirectory, header-only **Boost**,
   and zlib. The Android script enumerates all of it. Decide early whether
   Boost is a documented system dependency or a declared flatpak module — it
   must appear in the flatpak manifest either way.
3. **ROM compliance is a decision, not a copy.** The "embed the ROMs in the
   binary" property the ADAM app has does not transfer: Apple's firmware is
   copyrighted and not freely licensed. Either ship without ROMs and provide
   an import path (and make the app boot usefully in that state), or
   distribute only to people supplying their own. This has to be settled
   *before* the first release job runs, because it changes what CI uploads.
   The `spoverslip.bin` firmware is part of the FujiNetWIFI AppleWin fork and
   is not affected.
4. **Analog paddles, not a digital joystick.** `input_map.c`'s digital
   encoding and the SDL gamepad layer both change shape: the left stick
   drives paddle axes proportionally. The CoCo target has the same problem
   (two 6-bit DACs), so design this seam to serve both.
5. **A 6502/65C02 debugger.** The engine, window layouts, key bindings and
   symbol machinery all carry over; the disassembler and register model do
   not. AppleWin is GPLv2-or-later, so unlike ADAM you may reuse its own
   disassembler — check that first.
6. **Display geometry.** Apple II resolutions, the 4:3 vs square-pixel
   question, and text-mode legibility at small window sizes all differ from
   256×212. The aspect-mode setting and the letterboxing code carry over; the
   numbers do not.
7. **Two frontends currently develop against the working checkouts**
   (`~/Workspace/AppleWin` branch `linux`, `~/Workspace/fujinet-pc-apple2`).
   The desktop repo must pin *pushed* commits as submodules and keep
   `APPLEWIN_SRC` / `FUJINET_SRC` as the tandem-development override — the
   Android app's "always use the local checkout" model does not survive
   contact with CI or flatpak.

### Heads-up for CoCo and MSX, while it is fresh

- **The emulator side may need its own transport patch.** The CoCo app
  replaces XRoar's `becker.c` with a lazy-connect variant, because FujiNet
  listens and XRoar connects out — a client that gives up at startup never
  recovers. Both inverted-direction targets need the emulator to retry, and
  that patch belongs in the staging script with a header comment explaining
  why (it stays GPL either way).
- **openMSX drags in a real dependency chain** — SDL2, Tcl, freetype — where
  adamcore drags in nothing. That lands on the flatpak manifest (declare
  them), the macOS bundle (static or bundled?), and the Windows
  self-containment check (the import whitelist will need honest additions,
  not a widened regex). Budget for it before promising a single-file
  artifact.
- **`ADAM_USE_SDL2` exists in this repo's options for a reason**: a target
  whose core already links SDL2 should not also drag in SDL3. Decide which
  SDL the session layer uses per target, once.
- **50/60 Hz switching** (CoCo, MSX) breaks the "phase-lock to a ~60 Hz tick
  stream" assumption. The wall-clock fallback already handles PAL correctly;
  make sure the phase-lock declines to engage rather than locking to the
  wrong rate.

---

## 12. Order of work

The sequence that worked here, with the risky things early:

1. `core/` + one frontend (GNOME) + `WITH_FUJINET=OFF` — get pixels on screen
   from a staged emulator core, paced, with keyboard input.
2. `cmake/Dependencies.cmake` + staging, so the clone-and-build promise holds
   from day one instead of being retrofitted.
3. The FujiNet runtime: the entry wrapper, the patch set, `dlopen`, and the
   loopback bus. **This is the long pole** — the machine booting with FujiNet
   present is the moment the project exists.
4. Settings, media import, gamepad, audio.
5. The second frontend (KDE). Doing this early is what keeps the core honest
   about being toolkit-agnostic; leaving it late guarantees leakage.
6. The debugger engine + its first native window.
7. CI: linux job first, then macOS and Windows (which are the *only* way
   those platforms get tested), then flatpak.
8. Icons, desktop entries, AppStream metainfo, screenshots.
9. Packaging and the release job.
10. The remaining frontends' debugger windows and polish.

Keep a `TODO` with "Done (this pass) / Next" sections as you go — it is what
makes the next session (human or otherwise) able to pick the work up cold.
