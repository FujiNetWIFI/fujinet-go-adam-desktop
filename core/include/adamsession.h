/*
 * adamsession -- toolkit-agnostic desktop session for FujiNet Go Adam.
 *
 * Owns the adamcore emulator loop (its own paced thread), the SDL audio and
 * gamepad backends, the in-process FujiNet runtime (dlopen'd libfujinet.so,
 * joined to the core over AdamNet Bus-over-IP on loopback TCP 65216), the
 * shared settings store, and the media/ROM path layout. Frontends (GTK, Qt,
 * and later AppKit/Win32) drive this API and only do windowing, painting,
 * and event translation.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAMSESSION_H
#define ADAMSESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADAMSESSION_FB_WIDTH  256
#define ADAMSESSION_FB_HEIGHT 212

/* The app's accent colour (the launcher icon's background): toggled-on
 * buttons, the debugger's current-line highlight and the Run button. It is
 * light, so text on it is black. */
#define ADAMSESSION_ACCENT_RGB 0x8D84E5

/* AdamNet Bus-over-IP loopback port (core listens, FujiNet connects in),
 * fixed to match the Android app. The FujiNet web admin port is chosen at
 * random each run (see fujinet_runtime.c) so a second FujiNet-family
 * process on the same machine holding a fixed port can never make the
 * config window unreachable; this is only the fallback used if the OS
 * somehow can't hand out a free one. */
#define ADAMSESSION_BOIP_PORT  65216
#define ADAMSESSION_WEBUI_PORT 65214

typedef struct adamsession adamsession;
typedef struct adamdebug adamdebug;

/* All members optional (NULL = default). The ADAM system ROMs are embedded
 * in the binary at build time, so there is no ROM path to configure.
 *  config_dir: default $XDG_CONFIG_HOME/fujinet-go-adam
 *  data_dir:   default $XDG_DATA_HOME/fujinet-go-adam
 *  fujinet_lib: path to libfujinet.so/.dylib; default searches
 *              $FUJINET_LIB, the install libdir, then
 *              tools/fujinet/work/out. "" disables.
 *  fujinet_runtime_src: directory holding the pristine fnconfig.ini +
 *              data/ + SD/ used to provision the user's runtime tree on
 *              first start (a macOS app passes its bundle's
 *              Resources/fujinet here); default searches the install
 *              share dir, then tools/fujinet/work/out. */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
    const char *fujinet_runtime_src;
} adamsession_paths;

/* Creates the session, provisions the config/data directories, and loads the
 * shared settings store. Does not start emulation. Returns NULL only on
 * out-of-memory / unusable directories. */
adamsession *adamsession_new(const adamsession_paths *paths);
void adamsession_free(adamsession *s);

/* ---- settings (shared INI; one store for all frontends/platforms) ------- */
int         adamsession_get_int(adamsession *s, const char *key, int def);
void        adamsession_set_int(adamsession *s, const char *key, int value);
const char *adamsession_get_str(adamsession *s, const char *key, const char *def);
void        adamsession_set_str(adamsession *s, const char *key, const char *value);
void        adamsession_settings_flush(adamsession *s);

/* ---- lifecycle ----------------------------------------------------------- */
typedef struct {
    int machine;            /* 0 = ADAM, 1 = ColecoVision */
    const char *cart_path;  /* optional cartridge image */
    int palette;            /* 0..3 */
    int expansion;          /* controller expansion module 0..7 */
    int joystick_mode;      /* 0 none, 1 both ports, 2 port 2 only, 3 port 1 */
    int swap_buttons;       /* 0/1 */
    int reverse_keypad;     /* 0/1 */
    int enable_fujinet;     /* start the in-process FujiNet runtime */
    int enable_audio;       /* open the SDL audio device */
    int enable_gamepad;     /* start the SDL gamepad thread */
} adamsession_start_opts;

/* Fills opts from the settings store (machine defaults to ADAM, everything
 * enabled). Convenience so frontends launch with the persisted config. */
void adamsession_default_opts(adamsession *s, adamsession_start_opts *opts);

/* Starts FujiNet (if enabled) and the emulator thread. Returns 0 on success,
 * -1 with adamsession_last_error() set. Restart (stop+start) applies machine
 * option changes. */
int  adamsession_start(adamsession *s, const adamsession_start_opts *opts);
void adamsession_stop(adamsession *s);
int  adamsession_is_running(const adamsession *s);
const char *adamsession_last_error(const adamsession *s);

/* ---- video ---------------------------------------------------------------
 * The emulator thread stores the latest changed frame; the UI thread pulls it
 * on its own vsync/frame-clock tick. copy_frame copies the latest frame into
 * dst (ADAMSESSION_FB_WIDTH*HEIGHT uint16 RGB565) iff its serial differs from
 * *serial_inout, updates *serial_inout, and returns 1; returns 0 when the
 * frame is unchanged (dst untouched). Pass *serial_inout = 0 to force a copy
 * (e.g. first paint after a window map). */
int  adamsession_copy_frame(adamsession *s, uint16_t *dst, uint64_t *serial_inout);

/* Feed the UI's vsync ticks (frame-clock time, CLOCK_MONOTONIC ns). While a
 * steady ~60Hz tick stream arrives, the emulator phase-locks one emulated
 * frame per tick; otherwise it paces on the wall clock (59.922 Hz). */
void adamsession_notify_vsync(adamsession *s, int64_t frame_time_ns);

/* ---- input ---------------------------------------------------------------
 * adam_code: ADAM keyboard byte (ASCII for printable keys; EOS codes 0x80+
 * for SmartKeys / editing block / cursor keys -- see adam_key_from_event). */
void adamsession_key(adamsession *s, uint8_t adam_code);
void adamsession_joystick_raw(adamsession *s, int port, uint16_t state);
void adamsession_reset(adamsession *s, int mode); /* 0 = ADAM, 1 = ColecoVision */

/* ColecoVision controller word: active-low, idle 0x7F7F. keypad: 0..9,
 * 10 = '*', 11 = '#', negative = none. */
uint16_t adam_controller_encode(int up, int down, int left, int right,
                                int fire_left, int fire_right, int keypad);

/* Translate a desktop key event to an ADAM keyboard byte, or -1 when the key
 * is not forwarded. keysym is an X11/xkb keysym (== GDK keyval; Qt frontends
 * map through a small table), unicode the translated character (0 if none),
 * ctrl_down the Ctrl modifier state. Mirrors the Android HardwareKeyboard
 * mapping, plus F1-F6 = SmartKeys I-VI and Insert/Undo from the editing
 * block (desktop keyboards have the keys for them). */
int adam_key_from_event(uint32_t keysym, uint32_t unicode, int ctrl_down);

/* ---- controllers -----------------------------------------------------------
 * Every control the two ColecoVision controllers have, plus the session's
 * own actions, flattened into one target index so the bindings table, the
 * settings store and the keypad window can all name them: per port the
 * keypad, the stick and the two fire buttons, then the system actions. The
 * keyboard, the on-screen keypad window and each gamepad hold their own
 * state per port and the session merges them into the controller word. */
#define ADAM_KEYPAD_KEYS 12
typedef enum {
    ADAM_ACT_KEYPAD = 0,       /* + 0..9 = '0'..'9', + 10 = '*', + 11 = '#' */
    ADAM_ACT_UP = ADAM_KEYPAD_KEYS, ADAM_ACT_DOWN, ADAM_ACT_LEFT, ADAM_ACT_RIGHT,
    ADAM_ACT_FIRE_L, ADAM_ACT_FIRE_R,
    ADAM_ACT_PER_PORT
} adam_action;

typedef enum {
    ADAM_SYSACT_RESET_COMPUTER = 0, /* the ADAM computer's reset (Machine menu) */
    ADAM_SYSACT_RESET_GAME,         /* the ColecoVision reset */
    ADAM_SYSACT_COUNT
} adam_sysaction;

#define ADAM_TARGET_PORT(port, act) ((port) * ADAM_ACT_PER_PORT + (act))
#define ADAM_TARGET_SYSACT(sa)      (2 * ADAM_ACT_PER_PORT + (sa))
#define ADAM_TARGET_COUNT           (2 * ADAM_ACT_PER_PORT + ADAM_SYSACT_COUNT)

/* Apply one control from the on-screen keypad window. `down` is press or
 * release; the control stays held until released. A system target acts on
 * press. */
void adamsession_press(adamsession *s, int target, int down);
void adamsession_sysaction(adamsession *s, int sysact);
/* Release everything the keyboard holds (focus loss). */
void adamsession_release_all(adamsession *s);
/* What the machine sees held on a port right now (keyboard, gamepads and the
 * keypad window together), one bit per adam_action. For the keypad
 * window's highlights. */
uint32_t adamsession_buttons_held(adamsession *s, int port);

/* ---- keyboard translation ------------------------------------------------
 * keysym is an X11/xkb keysym (== a GDK keyval; Qt, Win32 and AppKit map
 * through the tables below), so one bindings table serves every frontend.
 * Returns 1 if the key drives a controller target (applied or released here,
 * system actions included) and the frontend must NOT also type it on the
 * ADAM's keyboard, 0 otherwise. Ctrl + a digit is a built-in chord for the
 * port 1 keypad digit. Call for every key press and release. */
int adamsession_input_key(adamsession *s, uint32_t keysym, int down,
                          int ctrl_down);

/* Non-printing keys in the keysym space the frontends translate to. */
enum {
    ADAMSESSION_KEYSYM_NONE = 0,
    ADAMSESSION_KEYSYM_UP = 0xff52, ADAMSESSION_KEYSYM_DOWN = 0xff54,
    ADAMSESSION_KEYSYM_LEFT = 0xff51, ADAMSESSION_KEYSYM_RIGHT = 0xff53,
    ADAMSESSION_KEYSYM_ESCAPE = 0xff1b, ADAMSESSION_KEYSYM_RETURN = 0xff0d,
    ADAMSESSION_KEYSYM_BACKSPACE = 0xff08, ADAMSESSION_KEYSYM_TAB = 0xff09,
    ADAMSESSION_KEYSYM_SPACE = 0x20,
    ADAMSESSION_KEYSYM_F1 = 0xffbe, ADAMSESSION_KEYSYM_F2, ADAMSESSION_KEYSYM_F3,
    ADAMSESSION_KEYSYM_F4, ADAMSESSION_KEYSYM_F5, ADAMSESSION_KEYSYM_F6,
    ADAMSESSION_KEYSYM_F7, ADAMSESSION_KEYSYM_F8, ADAMSESSION_KEYSYM_F9,
    ADAMSESSION_KEYSYM_F10, ADAMSESSION_KEYSYM_F11, ADAMSESSION_KEYSYM_F12,
    ADAMSESSION_KEYSYM_LSHIFT = 0xffe1, ADAMSESSION_KEYSYM_RSHIFT = 0xffe2,
    ADAMSESSION_KEYSYM_LCTRL = 0xffe3, ADAMSESSION_KEYSYM_RCTRL = 0xffe4,
    ADAMSESSION_KEYSYM_LALT = 0xffe9, ADAMSESSION_KEYSYM_RALT = 0xffea,
    ADAMSESSION_KEYSYM_KP_0 = 0xffb0, ADAMSESSION_KEYSYM_KP_1,
    ADAMSESSION_KEYSYM_KP_2, ADAMSESSION_KEYSYM_KP_3, ADAMSESSION_KEYSYM_KP_4,
    ADAMSESSION_KEYSYM_KP_5, ADAMSESSION_KEYSYM_KP_6, ADAMSESSION_KEYSYM_KP_7,
    ADAMSESSION_KEYSYM_KP_8, ADAMSESSION_KEYSYM_KP_9,
    ADAMSESSION_KEYSYM_KP_ENTER = 0xff8d, ADAMSESSION_KEYSYM_KP_MULTIPLY = 0xffaa,
    ADAMSESSION_KEYSYM_KP_DIVIDE = 0xffaf, ADAMSESSION_KEYSYM_KP_PERIOD = 0xffae
};
/* The HID-usage keysym band: a key with no X11 keysym of its own (a media
 * key, an international key) is still bindable through this. */
#define ADAMSESSION_KEYSYM_HID_BASE 0x10000u
#define ADAMSESSION_HID_USAGE_MAX   0xE7u
/* Native key codes for the platforms whose toolkits do not deliver keysyms:
 * Windows scan code (set 1, with the E0 flag), Linux evdev code (GTK/Qt
 * keycode minus 8) and macOS virtual key code, each mapped to a keysym; a
 * key with no keysym of its own lands in the HID band (0x10000 + usage) so
 * it is still bindable. 0 when unknown. */
uint32_t adamsession_keysym_from_win_scancode(unsigned scancode, int extended);
uint32_t adamsession_keysym_from_evdev(unsigned code);
uint32_t adamsession_keysym_from_macos_keycode(unsigned keycode);
/* Name for a keysym ("F1", "Space", "a", "Keypad 5"); returns length. */
int adamsession_keysym_name(uint32_t keysym, char *dst, int dstsz);

/* ---- remappable bindings --------------------------------------------------
 * Every target can be driven by one keyboard key and one gamepad button.
 * Rebinding STEALS: a key drives exactly one target, because one keystroke
 * doing two things is worse than losing the old binding. keysym 0 / button
 * NONE is "clear". Persisted in the settings store under "bindings" as only
 * the entries that differ from the defaults. Defaults: the port 1 stick on
 * the numeric keypad's 8/4/6/2, Fire L = Keypad 0, Fire R = Keypad Enter,
 * '*' and '#' on Keypad * and /; gamepad: South = Fire L, East = Fire R
 * (the stick and D-pad always steer). Everything else goes to the ADAM's
 * keyboard. */
typedef enum {
    ADAM_PAD_BTN_NONE = -1,
    ADAM_PAD_BTN_SOUTH = 0, ADAM_PAD_BTN_EAST, ADAM_PAD_BTN_WEST,
    ADAM_PAD_BTN_NORTH, ADAM_PAD_BTN_BACK, ADAM_PAD_BTN_GUIDE,
    ADAM_PAD_BTN_START, ADAM_PAD_BTN_LEFT_STICK, ADAM_PAD_BTN_RIGHT_STICK,
    ADAM_PAD_BTN_LEFT_SHOULDER, ADAM_PAD_BTN_RIGHT_SHOULDER,
    ADAM_PAD_BTN_DPAD_UP, ADAM_PAD_BTN_DPAD_DOWN, ADAM_PAD_BTN_DPAD_LEFT,
    ADAM_PAD_BTN_DPAD_RIGHT,
    ADAM_PAD_BTN_LEFT_TRIGGER, ADAM_PAD_BTN_RIGHT_TRIGGER,
    ADAM_PAD_BTN_COUNT
} adam_pad_button;

typedef struct {
    uint32_t keysym;     /* 0 = no key */
    int      button;     /* adam_pad_button, NONE = no gamepad button */
} adam_binding;

const char *adam_target_name(int target);          /* "Controller 1: Fire L" */
const char *adam_target_short_name(int target);    /* "Fire L" */
adam_binding adamsession_binding_get(adamsession *s, int target);
/* Bind; the previous holder of the key/button (if any) is described into
 * `stolen` (may be NULL). keysym 0 / button NONE clears. */
void adamsession_binding_set_key(adamsession *s, int target, uint32_t keysym,
                                 char *stolen, int stolensz);
void adamsession_binding_set_button(adamsession *s, int target, int button,
                                    char *stolen, int stolensz);
void adamsession_bindings_reset(adamsession *s);
const char *adam_pad_button_name(int button);
/* The target a keysym / pad button drives, or -1. */
int adamsession_target_for_key(adamsession *s, uint32_t keysym);
int adamsession_target_for_button(adamsession *s, int port, int button);

/* Map mode: after begin(), the next gamepad button pressed on any pad is
 * reported by poll() (returns 1 and the button). cancel() disarms. */
void adamsession_gamepad_capture_begin(adamsession *s);
void adamsession_gamepad_capture_cancel(adamsession *s);
int  adamsession_gamepad_capture_poll(adamsession *s, int *button);

/* ---- gamepads (SDL, hotplug; started by adamsession_start) --------------- */
int  adamsession_gamepad_count(adamsession *s);
/* Name of connected pad idx into dst; returns length or 0. */
int  adamsession_gamepad_name(adamsession *s, int idx, char *dst, int dstsz);
/* Bind pad idx to controller port 0/1; -1 restores automatic assignment
 * (first pad -> port 0, second -> port 1). */
void adamsession_gamepad_assign(adamsession *s, int idx, int port);
/* The controller word the gamepad backend last pushed to a port (idle
 * 0x7F7F when nothing was pushed yet). For status displays and tests. */
uint16_t adamsession_gamepad_last_state(adamsession *s, int port);

/* ---- audio ---------------------------------------------------------------
 * Owned by the session (SDL) when opts.enable_audio was set. A frontend that
 * wants to own the device instead can pull mixed emulator+FujiNet mono S16
 * samples at the configured 44100 Hz. */
int  adamsession_render_audio(adamsession *s, int16_t *out, int nsamples);

/* ---- FujiNet -------------------------------------------------------------*/
int         adamsession_fujinet_running(const adamsession *s);
/* 1 while FujiNet is connected to the core's BoIP listener (updated each
 * emulated frame). */
int         adamsession_fujinet_link(const adamsession *s);
const char *adamsession_fujinet_webui_url(const adamsession *s);
/* Copies the most recent FujiNet console output (NUL-terminated) into dst;
 * returns the number of bytes written (excluding the NUL). */
int         adamsession_fujinet_copy_log(adamsession *s, char *dst, int max);

/* Import media for the emulated machine:
 *  .dsk/.ddp -> FujiNet SD directory (mount via the web UI)
 *  .rom/.col/.bin -> cartridge directory (returned path usable as cart_path)
 * Returns 0 on success and writes the destination path into dest_out. */
int adamsession_import_media(adamsession *s, const char *src_path,
                             char *dest_out, int dest_sz);

/* Directory paths (valid for the session's lifetime). */
const char *adamsession_config_path(const adamsession *s);
const char *adamsession_data_path(const adamsession *s);
const char *adamsession_sd_path(const adamsession *s);

/* ---- debugger ------------------------------------------------------------
 * Lazily created; engaging it (breakpoints/pause) switches the emulator loop
 * to the instruction-stepped core path. See adamdebug.h. */
adamdebug *adamsession_debugger(adamsession *s);

#ifdef __cplusplus
}
#endif

#endif /* ADAMSESSION_H */
