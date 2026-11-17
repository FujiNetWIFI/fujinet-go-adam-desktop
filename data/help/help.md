This is the text of FujiNet Go Adam's built-in Help (Help ▸ FujiNet Go Adam
Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to the
frontend it is about; here you see them all. Every topic below is one page in
the app.

# Getting started

FujiNet Go Adam is a Coleco ADAM, or a ColecoVision, with a FujiNet built in.
The machine is adamcore's: the Z80, the TMS9928A video chip, the SN76489 sound
chip and the ADAM's own AdamNet devices. The FujiNet runs inside the app and
talks to the ADAM over AdamNet, so disks, data packs and the network are there
from the first start. The system ROMs are part of the app, so it starts at
once.

1. **Start the app.** The ADAM powers on. Type on your keyboard: it is the
   ADAM's keyboard. See [Controllers and keys](#controllers-and-keys).
2. **Mount a disk with FujiNet.** FujiNet's web page (**FujiNet ▸
   Configuration**) mounts disks and data packs from the SD folder or a
   network host. See [FujiNet](#fujinet).
3. **Or bring your own files.** **Import Disk or Data Pack…** copies a `.dsk`
   or `.ddp` image into FujiNet's SD folder, and **Load Cartridge…** boots a
   `.rom`, `.col` or `.bin` cartridge as a ColecoVision. See
   [Disks, data packs and cartridges](#disks-data-packs-and-cartridges).
4. **Play.** The keys, the gamepads and the Controllers window work both
   controllers.
5. **Change the machine.** [Preferences](#preferences) picks the machine, the
   palette, the expansion module, the picture and FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window and the FujiNet
console log opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too. In
the main window <kbd>F1</kbd> is the ADAM's SmartKey I, so it goes to the
computer.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-adam-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The machine's picture fills the window. The menus have:

- **Reset Computer (ADAM)** and **Reset Game (ColecoVision)**.
- **Import Disk or Data Pack…**, **Load Cartridge…** and **Eject Cartridge**:
  see [Disks, data packs and cartridges](#disks-data-packs-and-cartridges).
- **FujiNet Configuration…** and **FujiNet Console Log…**.
- **Controllers** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **Preferences**, **Help** and **About**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar; <kbd>F10</kbd> opens it).

<!-- end -->

<!-- only: kde -->

The menu bar has Machine, Media, FujiNet, View and Help. Preferences and Quit
are in the Machine menu, and View has Fullscreen (<kbd>F11</kbd>).

<!-- end -->

<!-- only: windows -->

The menu bar has Machine, Media, FujiNet, View and Help. View has Fullscreen
(<kbd>F11</kbd>). The FujiNet menu has **Connection…**, which takes the place
of Preferences: see [Connection settings](#connection-settings).

<!-- end -->

<!-- only: macos -->

On a Mac, Settings (⌘,) and Quit are in the app menu. **Import Disk or Data
Pack…** is ⌘I, **Controllers** ⌘K, **Debugger** F12 and **Toggle Full Screen**
⌃⌘F.

<!-- end -->

## Reset

- **Reset Computer (ADAM)** starts the ADAM as if its power had been cycled.
- **Reset Game (ColecoVision)** starts the ColecoVision side the same way, for
  a cartridge.

## The picture

The picture scales with the shape you choose in Preferences: square pixels
(256:212), a television's 4:3, or whole-number steps. **Smooth scaling**
blends the pixels when the picture is stretched. The window remembers its size.

<!-- only: gnome kde windows -->

<kbd>F11</kbd> toggles full screen.

<!-- end -->

# Controllers and keys

A ColecoVision has two hand controllers: a four-way stick, two fire buttons and
a keypad of 1 to 9, `*`, 0 and `#`. The ADAM also has a keyboard, and the
keyboard here is the real keyboard: nearly every key types on the ADAM.

## Default keys

The first controller's stick is on the numeric keypad. Every other key types.

| Controller 1 | Key |
|---|---|
| Up, Down, Left, Right | <kbd>Keypad 8</kbd> <kbd>Keypad 2</kbd> <kbd>Keypad 4</kbd> <kbd>Keypad 6</kbd> |
| Left fire | <kbd>Keypad 0</kbd> |
| Right fire | <kbd>Keypad Enter</kbd> |
| `*` and `#` | <kbd>Keypad Multiply</kbd> and <kbd>Keypad Divide</kbd> |

The keypad's digits are on <kbd>Ctrl</kbd> and a digit: <kbd>Ctrl+1</kbd> is
the keypad's 1, which is how a game is chosen.

<!-- only: gnome kde windows -->

The ADAM's SmartKeys I to VI are <kbd>F1</kbd> to <kbd>F6</kbd>.

<!-- end -->

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Controllers |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. The first drives Controller 1, the next Controller 2; a third drives
nothing until one is unplugged. A message says which controller a gamepad
took, and when one goes. The Controllers window and the Input settings can
fix which is which. A gamepad's controller is remembered between runs: plug it
back in, or start the app again, and it gets its controller back.

| Gamepad | Does |
|---|---|
| D-pad, or left stick | The stick |
| South (<kbd>A</kbd>) | Left fire |
| East (<kbd>B</kbd>) | Right fire |

The same buttons work on both controllers, for the gamepad that drives it.
Every other button is free to be given a job. The left stick is an eight-way
switch. A gamepad with no standard layout (a plain USB adapter) steers with
its first stick or hat, and its buttons and hat directions can be given jobs
in the same way.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, click a control on the picture (38 in all: both controllers' twelve
keys, four directions and two fire buttons, and the two Reset actions), then
press the keyboard key or the gamepad button to give it. The control's old
holder loses it, and the message names it. Map stays on, so several controls
can be given in a row; <kbd>Escape</kbd> steps back one level and **Done**
leaves Map.

- **Clear** takes the job off the key and the button of the control you
  picked.
- **Defaults** puts every control back (so does **Reset Controls** in the
  Input settings).
- **Show bindings** writes each control's key on it.

A key drives one control; a gamepad button drives one control of each
controller. <kbd>F9</kbd>, <kbd>F11</kbd> and <kbd>F12</kbd> cannot be given
away. The map is kept in the settings, and only what differs from the defaults
is written; a map saved by an earlier version still loads.

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) draws both hand controllers as a player
sees them, Controller 1 on the left and Controller 2 on the right, in the
ADAM's cream plastic. It floats on its own at a fixed size, stays hidden until
you ask for it and hides rather than closes. Each controller has:

- **The stick**: a short disc with a ring of eight direction wedges.
- **The fire buttons**, one on each side of the case.
- **The keypad**: twelve keys, 1 to 9, `*`, 0 and `#`, with a slot at the top
  for a game's overlay (drawn only; nothing there is pressed).

The controls that are held light up in the accent colour, whoever holds them:
the keyboard, a gamepad or the mouse. Under the controllers are the **Reset
Computer** and **Reset Game** buttons, then the row of **Map**, **Clear**,
**Defaults** and **Show bindings**, and a row for each gamepad.

| With the mouse | Does |
|---|---|
| Press a key or a fire button | Holds it for as long as the button is down |
| Press on a wedge | Holds that direction |
| Drag on the stick | Tilts it (eight ways, nothing near the middle) |
| Drag off a control | Lets it go |
| Release on a Reset button | Runs it |

The machine reads its controllers once a frame, so a control is *held*, not
pulsed, as the plastic one is: a value present for an instant falls between
frames and a polling game would never see it. Switching to another window lets
go of everything the mouse holds. Keys typed in the Controllers window go to
the machine too.

Each gamepad has a row with its name, the controller it drives and a choice of
*Automatic*, *Controller 1* or *Controller 2*. The rows follow the gamepads as
they come and go. **Map**, **Clear**, **Defaults** and **Show bindings**
change the controls (see [Changing the controls](#changing-the-controls));
once Map has a control picked, the others are dimmed. Show bindings is
remembered (setting `controllers_show_bindings`).

The window can be opened at launch with `ADAM_OPEN_CONTROLLERS=1`
(`ADAM_OPEN_KEYPAD=1` still works).

## Keyboard view

The **Keyboard** check box adds the ADAM's own keyboard under the controllers:
its 75 keys as printed, with the six SmartKeys I to VI, the editing block
(MOVE/COPY, STORE/GET, CLEAR, INSERT, PRINT, DELETE), the arrow keys with HOME,
ESCAPE/WP, WILD CARD, UNDO and BACK SPACE. Click a key to type it on the ADAM;
a key sends when it is pressed and lights for a moment. <kbd>SHIFT</kbd> and
<kbd>CONTROL</kbd> latch: they stay lit until the next key, and let go after
it. <kbd>LOCK</kbd> toggles. <kbd>SHIFT</kbd> with an editing key sends the
shifted editing code. The keyboard is off by default, and the choice is
remembered (setting `controllers_keyboard`).

# Machines

The machine is chosen in Preferences, and applies when the session restarts.

- **ADAM (computer)**: the Coleco ADAM, with its keyboard and its AdamNet
  devices. The FujiNet serves the disks and data packs.
- **ColecoVision (game)**: the console, which boots a cartridge. **Load
  Cartridge…** starts it in this mode.
- **Palette** picks the TMS9928A's colour set: *Default (TMS9928)*, *Palette
  2*, *Palette 3* or *Palette 4*.
- **Expansion module** names one of the ColecoVision's controller expansions:
  the roller controller, the driving module or the Super Action speed roller.
  The emulator does not act on it yet.

# BIOS and ROMs

The system ROMs the machine needs are public domain firmware, and are part of
the app: embedded in the program, there is nothing to import. See the licences
at the end of this Help.

# Disks, data packs and cartridges

FujiNet's disks and data packs are images in its SD folder; a cartridge is a
file you open.

## Import Disk or Data Pack

**Import Disk or Data Pack…** copies a `.dsk` (a disk) or a `.ddp` (a data
pack) into FujiNet's SD folder, so FujiNet's configuration can mount it. Other
file types are refused.

## Load Cartridge

**Load Cartridge…** takes a `.rom`, `.col` or `.bin` file, copies it into the
app's `carts` folder and starts it in ColecoVision mode. It is remembered and
starts again next time. **Eject Cartridge** forgets it.

# FujiNet

The FujiNet is the FujiNet-PC ADAM firmware, running inside the app. The ADAM
and FujiNet talk AdamNet over a local socket (Bus-over-IP, port 65216), so a
disk or data pack that FujiNet serves is a drive to the ADAM.

## Configuration

**FujiNet Configuration…** shows FujiNet's own web page, which mounts disks and
data packs, joins networks and sets up TNFS hosts. It is on port 65214 on this
computer only. With **Use external FujiNet** on, it is the other instance's
page you want instead.

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom.

## An external FujiNet

**Use external FujiNet** skips the built-in FujiNet: the app listens on the
**Listen address:port** you give, for example `0.0.0.0:65216`, and waits for
another instance to connect. Set that instance's `[BOIP]` host and port to this
address. `0.0.0.0` takes connections from the network, and `127.0.0.1` from this
computer only. Leave the field empty for `127.0.0.1:65216`. Configuration and
the console log belong to the built-in FujiNet, so they say the FujiNet is
external.

## Behind the scenes

- FujiNet's AdamNet link is on port 65216 and its web UI on 65214, on this
  computer only, so another FujiNet on the same computer must not use them.
- FujiNet's settings and its SD folder are in the app's `fujinet` folder.

# Preferences

<!-- only: windows -->

On Windows there is no Preferences window. How FujiNet connects is **FujiNet ▸
Connection…** (see [Connection settings](#connection-settings)), and the input
settings are **View ▸ Input Settings…** (see [Input settings](#input-settings)).

<!-- end -->

Preferences set up the machine. The Machine, FujiNet and Input settings apply
by restarting the session when Preferences closes (except that a gamepad's
controller applies at once); the Display settings apply at once. The settings are one file, shared by every frontend of the app on the
computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (⌘,).

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **Machine**: *ADAM (computer)* or *ColecoVision (game)* (setting `machine`).
- **Palette**: which TMS9928A colour set to use: *Default (TMS9928)*, *Palette
  2*, *Palette 3* or *Palette 4* (setting `palette`).
- **Expansion module**: *None*, *Roller controller (mouse)*, *Roller controller
  (joystick)*, *Driving module (joystick)*, *Driving module (mouse)*, *Super
  Action speed roller, both ports (mouse)*, *Speed roller, port 1 (mouse)* or
  *Speed roller, port 2 (mouse)* (setting `expansion`).
- **Joystick mode**: which ports have a joystick: *No joystick*, *Both ports*,
  *Port 2 only* or *Port 1 only* (setting `joystick_mode`).
- **Swap joystick buttons**: exchange the left and right fire buttons (setting
  `swap_buttons`).
- **Reverse keypad**: reverse the keypad (setting `reverse_keypad`).

*Expansion module*, *Joystick mode* and *Reverse keypad* are stored but not
acted on yet.

## FujiNet settings

- **Use external FujiNet**: skip the built-in FujiNet and wait for another one
  (setting `fujinet_external`).
- **Listen address:port**: where to listen, for example `0.0.0.0:65216` (see
  [An external FujiNet](#an-external-fujinet); setting
  `fujinet_external_addr`).

## Display settings

- **Aspect ratio**: *Square pixels (256:212)*, *TV (4:3)* or *Integer scale*
  (setting `aspect_mode`).
- **Smooth scaling**: blend the pixels when the picture is stretched (setting
  `smooth_scaling`).

<!-- context: prefs/Input -->
## Input settings

On GNOME this is the Input page, in KDE and on a Mac the Input tab, and on
Windows **View ▸ Input Settings…**.

- **Enable gamepads**: poll USB and Bluetooth gamepads (setting
  `enable_gamepad`).
- **Swap joystick buttons**: exchange the left and right fire buttons on both
  controllers (setting `swap_buttons`). Outside GNOME this is in the Machine
  settings, and on Windows it is **Swap controller buttons** here.
- **Gamepads**: each one plugged in, and which controller it drives. A new
  gamepad drives Controller 1, the next Controller 2; choose *Controller 1* or
  *Controller 2* to fix it, or *Automatic*. The choice is remembered between
  runs (setting `pad_assign`), by the gamepad's id and name; two identical
  gamepads cannot be told apart, so the first to connect gets it.
- **Reset Controls**: put every key and gamepad button back to its default.
- **Open Controllers**: open the Controllers window, where the controls are
  changed (see [Changing the controls](#changing-the-controls)).

<!-- context: prefs/Connection -->
## Connection settings

On Windows, **FujiNet ▸ Connection…** chooses between the built-in FujiNet and
an external one: **Use external FujiNet** and the **Listen address:port** (see
[An external FujiNet](#an-external-fujinet)). The other settings are read from the
settings file (see below).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `cart_path` (the loaded
cartridge, empty for none), the `bindings` (the controls that differ from the
defaults), `pad_assign` (the gamepads kept on a controller),
`controllers_show_bindings` and `controllers_keyboard` (the Controllers
window's Show bindings and Keyboard) and `window_width`, `window_height` and
`window_maximized` (the window's size).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the machine and shows what it is doing. It
has twelve tabs; closing it lets the machine go on as it was.

| Button | Does |
|---|---|
| Run or Stop (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step Into (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a call |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Run to Cursor | On to the selected line |
| Go to addr / symbol | Moves the disassembly there |
| Pop Out | Moves the tab that shows into a window of its own; **Dock** puts it back |
| Help | Opens the topic of the tab that shows |

The Z80's registers and flags are in the strip under every tab: click one, type
a new value and press Enter while the machine is stopped.

<!-- context: debugger/console -->
## Console

The Console takes short commands, with history and completion on <kbd>Tab</kbd>.
Addresses are `FC5D`, `$FC5D`, `0xFC5D`, decimal or a symbol (bare hex is hex
unless it is a symbol), and expressions work: registers (`a`, `hl`, `sp`), flags,
`[addr]` for a byte, `{addr}` for a word, symbols, `frame` and `cycles`. A
breakpoint's condition is such an expression, for example `a == 5`, and on a
watchpoint it can use `value`, `addr` and `old`, for instance
`bpw $6000 if value > old`. `help` lists the commands.

| Command | Does |
|---|---|
| `step`, `over`, `out` | One instruction; over a call; to the return |
| `run`, `stop`, `frame` | Go on, break, or run one frame |
| `scanline` | Runs some beam lines |
| `runto` | Runs to an address |
| `break` | An execute breakpoint |
| `bpr`, `bpw`, `bpa` | Memory reads, writes, or both |
| `bpin`, `bpout` | I/O port reads or writes |
| `delete`, `enable`, `clear` | Manage breakpoints (also `disable`, `breaks`, `hits`) |
| `vram`, `vpoke`, `vdp` | The video memory and the VDP |
| `fujinet` | The FujiNet link |
| `print`, `regs`, `set` | Evaluate; show or set the registers |
| `mem`, `poke` | Dumps memory; writes bytes |
| `disasm`, `find` | Disassembles; searches memory |
| `watch`, `unwatch` | Variables to keep an eye on |
| `trace` | Shows the trace |
| `map`, `io`, `cart` | The memory map, ports and controllers, cartridge |
| `sound`, `video` | The sound chips; the video chip |
| `symbols`, `label`, `labels` | Symbols |
| `save` | Writes `ram`, `vram`, `regs` or `trace` to a file |

<!-- context: debugger/disassembly -->
## Disassembly

The Z80 disassembled at the PC, with symbols, and the breakpoints in the gutter
(red when on, grey when off); click a line to toggle its breakpoint, or press
<kbd>F9</kbd>. **Follow PC** keeps the current line in view. The EOS and OS-7 symbols
are known; load more with **Load Symbols**.

<!-- context: debugger/memory -->
## Memory

A hex view of the Z80's 64K as the CPU sees it: the ROMs, the RAM, and the
cartridge. Type an address or a symbol after **Address:** and press Enter.
**Poke** writes a byte while the machine is stopped. **Search** finds hex bytes
or text, and **Find Next** and **Find Previous** step through the matches.

<!-- context: debugger/breakpoints -->
## Breakpoints

Execute breakpoints, memory read, write and access watchpoints and I/O port
watchpoints, each with a condition, a hit count, and enable or disable. **Add**
makes one. When a watchpoint stops the machine the status line says what it did,
for instance `wrote $6010 = $12 (was $11)`.

<!-- context: debugger/watches -->
## Watches

Variables to keep an eye on, with their live values while the machine is
stopped. **Show in Memory** jumps to it, and **Break on Change** adds a write
breakpoint that fires when the value changes.

<!-- context: debugger/cart -->
## Cart and I/O

The ADAM memory configuration (ports $60 and $7F), the cartridge, the FujiNet
link, the memory map (every range the machine answers in), the two controllers and their
strobe, the video chip's latch and interrupt, and the NMI.

<!-- context: debugger/trace -->
## Trace

Every instruction the Z80 runs, live. **Record** turns it on; each instruction is
kept with its address, bytes, registers and flags (the console runs a little
slower while it records). **Branches only** shows jumps and calls, **In range**
a range, and **Clear** empties it. A halted CPU shows as one line with a repeat
count.

<!-- context: debugger/history -->
## History

The same record as the Trace tab, shown when the machine stops: the instructions
leading up to a breakpoint or watchpoint, newest at the bottom.

<!-- context: debugger/video -->
## Video

The TMS9928A, with the picture on the left and its sections on the right. The
console records the registers for each line, so split screens and scrolling show.

| View | Shows |
|---|---|
| Display | The last finished frame, as the screen showed it |
| Decoded | One source: Screen, Patterns, Sprites or Palette |
| Beam | Stopped only: where the beam was in this frame |
| Values | The tables as hex, with the VRAM hex view |

**Zoom** goes from 1 to 16 times, or **Fit**. **Overlays** switch on a grid, the
sprites' boxes, the border, the beam, a heat tint for what was written lately and
the lines where the registers changed. The patterns have Bank 0, 1 and 2.

Point at a pixel to see it in **Pixel**: the tile or sprite and its address.
Click to pin it, click again to let go. **Break on writes** stops the machine when
the CPU writes that VRAM byte or the matching video register.

**Registers** shows the video registers with how long ago each changed,
**Colours** the sixteen colours, **Lines** the screen bands and **Timing** the
frame and line length. The VRAM section shows 16K a page at a time; type
`addr [bytes...]`, for example `1800: 41 42 43`, and press Enter to write.
Addresses wrap at 16K.

<!-- context: debugger/sound -->
## Sound

The sound chips, read while the machine is stopped: the SN76489's three tones,
noise and attenuations decoded into frequencies and levels.

<!-- context: debugger/symbols -->
## Symbols

Every known symbol, the EOS and OS-7 tables and the ones you loaded. **Filter** narrows the
list, and activating a row goes to that address.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every AdamNet packet FujiNet sees over BoIP, both ways, with hex, recorded from
the moment the machine starts. **Clear** empties the list. Beside it is the
state of the FujiNet link.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-adam/settings.ini` and
the data is in `~/.local/share/fujinet-go-adam` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-adam\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-adam`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-adam/settings.ini` and
the data is in `~/.local/share/fujinet-go-adam`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `carts` | cartridges loaded or dropped on the window |
| `fujinet` | FujiNet's settings, its SD folder (`SD`) and its data |

# Troubleshooting

- **Nothing happens when I mount a disk**: mount it from FujiNet's
  configuration page, or put its file in the SD folder with **Import Disk or
  Data Pack…**.
- **Unsupported media type**: only `.dsk` and `.ddp` go to FujiNet, and `.rom`,
  `.col` and `.bin` are cartridges.
- **A port is taken**: another program (a stray `fujinet-pc-adam`) is using
  65216 or 65214. Close it, or point the app at it with **Use external
  FujiNet**.
- **Keys don't reach the machine**: a dialog or another window is in front, or
  the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A game never sees the controller**: a control must be held, not tapped in
  an instant. The Controllers window holds a control as long as the button is
  down.
- **A gamepad does nothing**: Enable gamepads may be off in the Input settings,
  a third gamepad drives nothing, or its buttons may be given other jobs. The
  gamepad's row in the Controllers window shows which controller it drives.
- **A gamepad drives the wrong controller**: choose *Controller 1* or
  *Controller 2* for it in the Input settings or the Controllers window; the
  choice is remembered.

# About and licences

FujiNet Go Adam is free software under the GNU General Public License, version
3 or later. Copyright © 2026 Thomas Cherryhomes.

- **adamcore** (the Z80, the TMS9928A, the SN76489 and the ADAM's AdamNet
  devices, GPL-3.0-or-later) is the emulator.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app.
- **The system ROMs** are public domain firmware (see `COMPLIANCE.md`).
  "Coleco", "ADAM" and "ColecoVision" are trademarks of their owners, used to
  name the machine emulated; this project is not affiliated with them.

[fujinet.online](https://fujinet.online/) ·
[fujinet-go-adam-desktop](https://github.com/FujiNetWIFI/fujinet-go-adam-desktop)
