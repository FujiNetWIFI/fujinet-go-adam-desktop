/*
 * adamsession internals shared between the session, audio, gamepad, FujiNet,
 * and debugger translation units. Nothing here is API; frontends must only
 * include adamsession.h / adamdebug.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAMSESSION_INTERNAL_H
#define ADAMSESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "adamcore.h"
#include "adamsession.h"

#define ADAM_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

/* One input source's hold on one controller port. The keyboard, the keypad
 * window and each gamepad are separate sources so a release on one cannot
 * cancel a press on another; input.c merges them into the controller word. */
#define ADAM_SRC_PANEL    0
#define ADAM_SRC_KEYBOARD 1
#define ADAM_SRC_PAD      2 /* + pad slot */
#define ADAM_MAX_PADS     4
#define ADAM_SRC_COUNT    (ADAM_SRC_PAD + ADAM_MAX_PADS)
#define ADAM_DIR_UP    1
#define ADAM_DIR_DOWN  2
#define ADAM_DIR_LEFT  4
#define ADAM_DIR_RIGHT 8
typedef struct {
    uint8_t dirs;   /* ADAM_DIR_* */
    uint8_t fires;  /* bit 0 Fire L, bit 1 Fire R */
    uint16_t keys;  /* keypad keys held, bit per key */
    int8_t klast;   /* the keypad key the word shows, -1 none */
} adam_src_state;

struct adamsession {
    /* ---- resolved paths ---- */
    char config_dir[ADAM_PATH_MAX];
    char data_dir[ADAM_PATH_MAX];
    char cart_dir[ADAM_PATH_MAX];
    char settings_file[ADAM_PATH_MAX];
    char fujinet_lib[ADAM_PATH_MAX];  /* "" = unavailable/disabled */
    int fujinet_lib_disabled;         /* the caller passed "": don't search */
    char fujinet_src[ADAM_PATH_MAX];  /* provisioning source override */
    char fujinet_root[ADAM_PATH_MAX];
    char fujinet_config[ADAM_PATH_MAX];
    char fujinet_sd[ADAM_PATH_MAX];
    char fujinet_data[ADAM_PATH_MAX];
    char webui_url[64];

    /* ---- settings store ---- */
    pthread_mutex_t settings_mtx;
    setting_kv *settings;
    int settings_dirty;

    /* ---- lifecycle ---- */
    pthread_mutex_t lifecycle_mtx;
    pthread_t emu_thread;
    int emu_thread_started;
    volatile int stop_flag;
    volatile int running;
    adamcore *volatile core; /* owned by the emulator thread while running */
    adamsession_start_opts opts;
    char cart_path[ADAM_PATH_MAX];

    /* ---- latest-frame store (emulator thread -> UI thread) ---- */
    pthread_mutex_t frame_mtx;
    uint16_t frame[ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT];
    uint64_t frame_serial;

    /* ---- vsync phase-lock (UI tick -> emulator pacing) ---- */
    pthread_mutex_t vs_mtx;
    pthread_cond_t vs_cv; /* CLOCK_MONOTONIC */
    int64_t vs_ns;        /* latest tick time */
    long vs_iv_us;        /* most recent tick interval */
    int64_t vs_seen_ns;   /* last tick the emulator paced on */

    /* ---- backends ---- */
    void *audio;   /* audio_sdl.c state */
    void *gamepad; /* gamepad_sdl.c state */
    volatile int audio_mute;

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    int fujinet_loaded;  /* dlopen'd (kept for process lifetime) */
    int fujinet_running;
    int fujinet_external;      /* listening for a FujiNet that isn't ours */
    char boip_host[64];        /* where the BoIP listener binds */
    int boip_port;
    char boip_endpoint[80];    /* "host:port" for display */
    volatile int fujinet_link; /* BoIP client connected (emulator thread) */

    /* ---- debugger ---- */
    adamdebug *debugger;
    volatile int dbg_engaged; /* emulator loop runs the stepped path */

    /* ---- controllers (input.c) ---- */
    pthread_mutex_t input_mtx;
    adam_src_state src[2][ADAM_SRC_COUNT]; /* per port, per input source */
    uint32_t held_keysym[ADAM_TARGET_COUNT]; /* UI thread only */
    uint32_t chord_keysym[10];               /* Ctrl+digit chords, UI thread */

    char last_error[512];
};

/* session.c */
void session_set_error(adamsession *s, const char *fmt, ...);
void session_publish_frame(adamsession *s, const uint16_t *fb);

/* settings.c */
void settings_init(adamsession *s);
void settings_free_all(adamsession *s);

/* paths.c */
int  paths_init(adamsession *s, const adamsession_paths *p);
int  paths_provision_fujinet(adamsession *s); /* fills fujinet_* or "" */
int  mkdir_p(const char *path);
int  copy_file(const char *src, const char *dst);

/* audio_sdl.c */
int  audio_start(adamsession *s);
void audio_stop(adamsession *s);

/* input.c */
void input_init(adamsession *s);
void input_release_everything(adamsession *s);
/* Hold or release one per-port target (target < ADAM_TARGET_SYSACT(0)) for a
 * source. */
void input_apply(adamsession *s, int src, int target, int down);
/* Replace a gamepad slot's whole hold: the port it drives and its state
 * (the other port is cleared, so reassigning a pad cannot strand a hold). */
void input_pad_set(adamsession *s, int slot, int port, int dirs, int fires,
                   uint16_t keys);
void input_pad_clear(adamsession *s, int slot);

/* gamepad_sdl.c */
int  gamepad_start(adamsession *s);
void gamepad_stop(adamsession *s);

/* fujinet_runtime.c */
int  fujinet_start(adamsession *s);
void fujinet_stop(adamsession *s);
int  fujinet_copy_log(adamsession *s, char *dst, int max);
void fujinet_mix_audio(adamsession *s, int16_t *buf, int nsamples, int rate);

/* debugger.c: one debugger-controlled frame slice on the emulator thread;
 * returns the frame-changed flag (may block while paused). */
int  adamdebug_session_frame(adamsession *s);
/* Unpark a paused debugger so a stopping emulator thread can exit. */
void adamdebug_resume_for_stop(adamdebug *d);
/* Apply persisted breakpoints/hook to a freshly created core (emulator
 * thread, right after adamcore_create). */
void adamdebug_on_core_created(adamsession *s);
/* adamcore's BoIP wire trace (adamcore_set_boip_trace); ud is the engine. */
void adamdebug_boip_trace(void *ud, int dir, const uint8_t *buf, int len);
/* The emulator thread is about to destroy its core: sets s->core = NULL
 * under the engine's lock so a UI-thread debugger call is never mid-flight
 * on it. */
void adamdebug_clear_core(adamsession *s);
/* Free the engine (adamsession_free, after the emulator thread has joined). */
void adamdebug_destroy(adamdebug *d);

#endif /* ADAMSESSION_INTERNAL_H */
