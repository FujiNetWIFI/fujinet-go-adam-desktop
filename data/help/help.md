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
time. <kbd>A</kbd> on a pad is the left fire and <kbd>B</kbd> the right fire;
the left stick or the D-pad steers. The pad's other buttons can be given jobs
in the Controllers window.

## Changing the controls

The controls are changed in the Controllers window (<kbd>F9</kbd>): click
**Map**, click a control, then press the keyboard key or the gamepad button to
give it. **Clear** takes a control's key away, and **Defaults** puts every
control back. The map is kept in the settings, and only what differs from the
defaults is written.

<!-- context: keypad -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) shows both ColecoVision controllers
side by side: the keypad, the stick and the fire buttons each, to click.
It floats on its own, and no panel appears on screen unless you ask for it.

| With the mouse | Does |
|---|---|
| Press a control | Holds it for as long as the button is down |
| Drag off a control | Lets it go |

The machine reads its controllers once a frame, so a control is *held*, not
pulsed, as the plastic one is: a value present for an instant falls between
frames and a polling game would never see it. Keys typed in the Controllers
window go to the machine too. **Map**, **Clear** and **Defaults** change the
controls (see [Changing the controls](#changing-the-controls)).

# Machines

The machine is chosen in Preferences, and applies when the session restarts.

- **ADAM (computer)**: the Coleco ADAM, with its keyboard and its AdamNet
  devices. The FujiNet serves the disks and data packs.
- **ColecoVision (game)**: the console, which boots a cartridge. **Load
  Cartridge…** starts it in this mode.
- **Palette** picks the TMS9928A's colour set: *Default (TMS9928)*, *Palette
  2*, *Palette 3* or *Palette 4*.
- **Expansion module** fits one of the ColecoVision's controller expansions:
  the roller controller, the driving module or the Super Action speed roller,
  with the mouse or a joystick as its input.

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

On Windows there is no Preferences window: the one thing it would set that
nothing else does is how FujiNet connects, and that is **FujiNet ▸
Connection…**. See [Connection settings](#connection-settings).

<!-- end -->

Preferences set up the machine. The Machine and FujiNet settings apply by
restarting the session when Preferences closes; the Display settings apply at
once. The settings are one file, shared by every frontend of the app on the
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
defaults) and `window_width`, `window_height` and `window_maximized` (the
window's size).

# Debugger

The Debugger (<kbd>F12</kbd>) stops the machine and shows what it is doing.
It has seven tabs; closing it lets the machine go on as it was. Opening it
stops the machine.

| Button | Does |
|---|---|
| Run or Stop (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a call, `RST`, `DJNZ`, block operation or `HALT` |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Frame (<kbd>Shift+F7</kbd>) | On to the next frame |
| Run to Cursor | On to the selected line |
| Load Symbols | Reads `.sym`, `.map` and `.noi` files |
| Save | Saves the disassembly, memory, VRAM or registers to a file |
| Help | Opens the topic of the tab that shows |

<!-- context: debugger/console -->
## Console

The Console takes commands, with completion on <kbd>Tab</kbd> and recall on
<kbd>Up</kbd> and <kbd>Down</kbd>. Numbers are `$C000`, `0xC000`, `C000h`,
`49152` or `%1010`; expressions can use registers, flags, `[addr]` and `{addr}`
for memory, labels, and C's operators. `help` lists the commands.

| Command | Does |
|---|---|
| `step`, `over`, `out` | One instruction; over a call; to the return |
| `run`, `go`, `stop` | Go on or break |
| `runto` | Runs to an address |
| `scanline`, `frame` | Runs some beam lines or frames |
| `break`, `bp` | An execute breakpoint |
| `bpr`, `bpw` | Memory reads or writes |
| `bpin`, `bpout` | I/O port reads or writes |
| `delete`, `enable`, `clear` | Manage breakpoints (also `disable`, `breaks`) |
| `print` | Evaluates an expression |
| `regs`, `set` | Shows or sets the registers |
| `mem`, `dump`, `poke` | Dumps memory; writes bytes |
| `vram`, `vpoke`, `vdp` | The video memory and the VDP |
| `io`, `map` | The memory map, ports and VDP registers |
| `fujinet` | The FujiNet link |
| `label`, `labels`, `symbols` | Name an address; load labels |
| `disasm`, `dis` | Disassembles |
| `trace` | The instruction trace |
| `save` | Saves state to a file |

<!-- context: debugger/cpu -->
## CPU and memory

The Z80's registers, flags and the memory view. Edit a value while stopped.

<!-- context: debugger/disassembly -->
## Disassembly

The Z80 disassembled at the PC, annotated with the EOS and OS-7 symbols and with
any labels you load. The current instruction is in the app's accent colour,
breakpoints are red (grey when disabled). Click a line to select it; **Follow
PC** keeps the current line in view.

<!-- context: debugger/vdp -->
## VDP

The TMS9928A, decoded from its live video memory: the name table, the pattern
banks, the sprites with their attribute table, the palette, and the registers
with the table addresses the beam is fetching from. The VRAM view shows 16K a
page at a time.

<!-- context: debugger/breakpoints -->
## Breakpoints

Execute breakpoints with conditions, address ranges and hit counts, and memory
read and write and port in and out watchpoints, each of which can be disabled.
**Add** makes one, **Clear All** removes them.

<!-- context: debugger/io -->
## Memory map and I/O

The ADAM's memory map, its I/O ports and the VDP registers.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every AdamNet packet between the ADAM and FujiNet, both ways, named and in hex,
recorded from launch, with the state of the Bus-over-IP link beside it. Repeats
(the idle keepalive, a poll) are folded into one line with a count. **Clear**
empties the list.

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
- **A gamepad does nothing**: its buttons may be given other jobs, or Joystick
  mode may be set to *No joystick*.

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
