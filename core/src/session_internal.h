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

struct adamsession {
    /* ---- resolved paths ---- */
    char config_dir[ADAM_PATH_MAX];
    char data_dir[ADAM_PATH_MAX];
    char cart_dir[ADAM_PATH_MAX];
    char settings_file[ADAM_PATH_MAX];
    char fujinet_lib[ADAM_PATH_MAX];  /* "" = unavailable/disabled */
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

    /* ---- debugger ---- */
    adamdebug *debugger;
    volatile int dbg_engaged; /* emulator loop runs the stepped path */

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

#endif /* ADAMSESSION_INTERNAL_H */
