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

/* AdamNet Bus-over-IP loopback port (core listens, FujiNet connects in) and
 * the FujiNet web admin port, both fixed to match the Android app. */
#define ADAMSESSION_BOIP_PORT  65216
#define ADAMSESSION_WEBUI_PORT 65214

typedef struct adamsession adamsession;
typedef struct adamdebug adamdebug;

/* All members optional (NULL = default). The ADAM system ROMs are embedded
 * in the binary at build time, so there is no ROM path to configure.
 *  config_dir: default $XDG_CONFIG_HOME/fujinet-go-adam
 *  data_dir:   default $XDG_DATA_HOME/fujinet-go-adam
 *  fujinet_lib: path to libfujinet.so; default searches $FUJINET_LIB, the
 *              install libdir, then tools/fujinet/work/out. "" disables. */
typedef struct {
    const char *config_dir;
    const char *data_dir;
    const char *fujinet_lib;
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
void adamsession_reset(adamsession *s, int mode); /* 0 = ADAM, 1 = CV */

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
