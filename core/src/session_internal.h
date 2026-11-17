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
#include <stdatomic.h>
#include <stdint.h>

#include "adamcore.h"
#include "adamsession.h"

#define ADAM_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

/* Who is holding an input down (the bits of pad_src): the keyboard, the
 * window's mouse, and for each gamepad (by the stable owner number its slot
 * was given, 0 to ADAM_MAX_PADS - 1) its buttons and its stick / hat
 * separately. So letting go of one never releases another, even two pads or
 * two buttons bound to the one key. */
#define ADAM_PORTS    2
#define ADAM_MAX_PADS 8
#define SRC_KEYBOARD 1u
#define SRC_PAD(owner)   (1u << (1 + (owner)))
#define SRC_STICK(owner) (1u << (1 + ADAM_MAX_PADS + (owner)))
#define SRC_POINTER  (1u << (1 + 2 * ADAM_MAX_PADS))

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
    int frame_valid;                 /* frame[] holds a frame of this run */
    atomic_ullong frame_count;       /* frames run, published or not */
    pthread_mutex_t cb_mtx;          /* held across frame_cb calls */
    adamsession_frame_cb frame_cb;
    void *frame_cb_ctx;

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

    /* ---- controllers (input.c) ----
     * What drives each controller action, one bit per source (SRC_*), so
     * letting go of one source never releases another's. input_mtx covers
     * these, held_keysym, host_word and the calls that hand the merged word
     * to the machine; it is a leaf lock. */
    pthread_mutex_t input_mtx;
    uint32_t pad_src[ADAM_PORTS][ADAM_ACT_PER_PORT];
    /* keys the keyboard holds, by target (the keysym it was pressed as), so a
     * release clears exactly what its press asserted even if the binding
     * changed meanwhile */
    uint32_t held_keysym[ADAM_TARGET_COUNT];
    uint32_t chord_keysym[10];          /* Ctrl+digit chords */
    uint16_t host_word[ADAM_PORTS];     /* what was last pushed to the machine */

    /* cross-thread system-action latch (adamsession_sysaction_post) */
    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    /* the pad generation, carried across gamepad_stop/start so a restart of
     * the session never replays an old toast */
    unsigned pad_gen_base;
    /* a pad thread has run before: its first scan finds the same pads again,
     * which is no hot-plug (no toast, no generation change) */
    int pad_restarted;
    /* the last gamepad hot-plug event, for a frontend toast */
    pthread_mutex_t pad_event_mtx;
    char pad_event[128];

    /* ---- keyboard skin (keyboard.c; input_mtx covers it) ---- */
    uint8_t kbd_down[ADAM_KBD_CAPS_MAX];       /* the mouse is on the cap */
    int64_t kbd_flash_ns[ADAM_KBD_CAPS_MAX];   /* CLOCK_MONOTONIC of the last press */
    uint8_t kbd_latch[4];                      /* by ADAM_MOD_*: 1 = latched */

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

/* bindings.c */
void bindings_init(adamsession *s);
void bindings_shutdown(adamsession *s);

/* input.c */
void input_init(adamsession *s);
/* Let go of every source and tell the machine (a stop). */
void input_release_everything(adamsession *s);
/* Hand the machine whatever is held (a new core is idle). */
void input_resend(adamsession *s);

/* keyboard.c: what pressing cap `index` sends (and its latch effects), or -1
 * when it sends nothing (a modifier, a release, a bad index). The public
 * adamsession_keyboard_press sends the result through adamsession_key. */
int  keyboard_press_code(adamsession *s, int index, int down);

/* gamepad_sdl.c */
int  gamepad_start(adamsession *s);
void gamepad_stop(adamsession *s);
/* Called by the gamepad thread for the pad that owns `owner`. press: a pad
 * button drives a controller target (the pad's SRC_PAD source). stick: an
 * analog stick or hat drives a direction (its SRC_STICK source). */
void session_gamepad_press(adamsession *s, int owner, int target, int down);
void session_gamepad_stick(adamsession *s, int owner, int port, int act, int down);
/* Called by the gamepad thread on a connect/disconnect. */
void session_gamepad_event(adamsession *s, const char *text);

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
