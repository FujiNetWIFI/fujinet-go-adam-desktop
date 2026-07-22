/*
 * adamsession -- session lifecycle and the paced emulator loop.
 *
 * Port of the Android host layer (adam_host.c + session_runtime.cpp): the
 * emulator thread free-runs adamcore_run_frame at 59.922 Hz on a wall-clock
 * sleeper, phase-locking to the UI's vsync ticks whenever a steady ~60 Hz
 * stream of them arrives (adamsession_notify_vsync). Frames are published
 * to a latest-frame store the UI thread pulls on its own tick, so a stalled
 * paint can never freeze the Z80 -- and therefore AdamNet/FujiNet traffic.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"
#include "roms_embedded.h"
#include "session_internal.h"

/****************************************************************************/
/** Small helpers                                                          **/
/****************************************************************************/

void session_set_error(adamsession *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
    fprintf(stderr, "adamsession: %s\n", s->last_error);
}

static long read_timer_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000L);
}

/* Absolute-deadline sleeping and monotonic condvars live in compat.h so
 * the same code paces correctly on Linux and macOS. */

/****************************************************************************/
/** Vsync phase-lock (ticks fed by the frontends)                          **/
/****************************************************************************/

void adamsession_notify_vsync(adamsession *s, int64_t frame_time_ns)
{
    if (!s) return;
    pthread_mutex_lock(&s->vs_mtx);
    if (s->vs_ns)
        s->vs_iv_us = (long)((frame_time_ns - s->vs_ns) / 1000);
    s->vs_ns = frame_time_ns;
    pthread_cond_broadcast(&s->vs_cv);
    pthread_mutex_unlock(&s->vs_mtx);
}

/* True while ~60Hz ticks are arriving (window visible on a ~60Hz display). */
static int vsync_fresh(adamsession *s, long now_us)
{
    int fresh;
    pthread_mutex_lock(&s->vs_mtx);
    fresh = s->vs_ns != 0 &&
            (now_us - (long)(s->vs_ns / 1000)) < 100000 && /* fresh (<100ms) */
            s->vs_iv_us >= 15000 && s->vs_iv_us <= 18500;  /* ~55-66Hz */
    pthread_mutex_unlock(&s->vs_mtx);
    return fresh;
}

/* Block until the next UI tick after the one we last paced on and return its
 * time (us). Self-correcting: if a frame's work overran a tick, the next call
 * returns at once. Bails on a short timeout so it can never hang if the ticks
 * stop mid-wait (window hidden). */
static long wait_next_vsync(adamsession *s)
{
    long v;
    pthread_mutex_lock(&s->vs_mtx);
    while (!s->stop_flag && s->vs_ns <= s->vs_seen_ns) {
        /* 40ms safety timeout so this can never hang if the ticks stop. */
        if (adam_cond_timedwait_ms(&s->vs_cv, &s->vs_mtx, 40) == ETIMEDOUT)
            break;
    }
    s->vs_seen_ns = s->vs_ns;
    v = (long)(s->vs_ns / 1000);
    pthread_mutex_unlock(&s->vs_mtx);
    return v;
}

/****************************************************************************/
/** Frame store                                                            **/
/****************************************************************************/

void session_publish_frame(adamsession *s, const uint16_t *fb)
{
    pthread_mutex_lock(&s->frame_mtx);
    memcpy(s->frame, fb, sizeof(s->frame));
    s->frame_serial++;
    pthread_mutex_unlock(&s->frame_mtx);
}

int adamsession_copy_frame(adamsession *s, uint16_t *dst, uint64_t *serial_inout)
{
    int copied = 0;
    pthread_mutex_lock(&s->frame_mtx);
    if (s->frame_serial != *serial_inout) {
        memcpy(dst, s->frame, sizeof(s->frame));
        *serial_inout = s->frame_serial;
        copied = 1;
    }
    pthread_mutex_unlock(&s->frame_mtx);
    return copied;
}

/****************************************************************************/
/** Emulator thread                                                        **/
/****************************************************************************/

static void *emu_thread_main(void *arg)
{
    adamsession *s = arg;
    adamcore_config cfg;
    adamcore *core;
    long next_us;
    const int pace_log = getenv("ADAM_PACE_LOG") != NULL;

    adam_thread_setname("adam-emu");

    memset(&cfg, 0, sizeof(cfg));
    cfg.os7_rom_data = adam_rom_os7;
    cfg.eos_rom_data = adam_rom_eos;
    cfg.wp_rom_data = adam_rom_wp;
    cfg.cart_path = s->cart_path[0] ? s->cart_path : NULL;
    cfg.start_machine = s->opts.machine ? ADAMCORE_MACHINE_CV
                                        : ADAMCORE_MACHINE_ADAM;
    cfg.boip_listen_port =
        (s->opts.enable_fujinet && s->fujinet_running) ? ADAMSESSION_BOIP_PORT
                                                       : 0;
    cfg.palette = s->opts.palette;
    cfg.expansion = s->opts.expansion;
    cfg.joystick_mode = s->opts.joystick_mode;
    cfg.swap_buttons = s->opts.swap_buttons;
    cfg.reverse_keypad = s->opts.reverse_keypad;
    cfg.audio_rate = 44100;

    core = adamcore_create(&cfg);
    if (!core) {
        session_set_error(s, "adamcore_create failed (ROM paths? BoIP port "
                          "%d in use?)", cfg.boip_listen_port);
        s->running = 0;
        return NULL;
    }
    s->core = core;
    if (s->debugger)
        adamdebug_on_core_created(s);

    next_us = read_timer_us();
    while (!s->stop_flag) {
        int changed;
        if (s->dbg_engaged)
            changed = adamdebug_session_frame(s);
        else
            changed = adamcore_run_frame(core);

        if (changed)
            session_publish_frame(s, adamcore_framebuffer(core, NULL, NULL));

        /* Pacing: one emulated frame per UI vsync tick while ticks arrive,
         * else the 59.922 Hz wall clock. Once-per-second diagnostics with
         * ADAM_PACE_LOG=1. */
        {
            long now = read_timer_us();
            int vfresh = vsync_fresh(s, now);
            static long dbg_t0;
            static int dbg_frames, dbg_behind, dbg_vsync;

            if (dbg_t0 == 0) dbg_t0 = now;
            dbg_frames++;
            if (vfresh) dbg_vsync++;

            next_us += 16688; /* 59.922 Hz NTSC frame */
            if (now >= next_us) dbg_behind++;

            if (now - dbg_t0 >= 1000000L) {
                if (pace_log)
                    fprintf(stderr,
                            "adamsession pace: %d fps over %ld ms (%d behind, "
                            "%d on vsync)\n",
                            dbg_frames, (now - dbg_t0) / 1000, dbg_behind,
                            dbg_vsync);
                dbg_t0 = now;
                dbg_frames = dbg_behind = dbg_vsync = 0;
            }

            if (vfresh) {
                /* Snap the wall clock so a later fallback resumes from real
                 * time rather than a stale schedule. */
                next_us = wait_next_vsync(s);
            } else if (now < next_us) {
                adam_sleep_until_us(next_us);
            } else {
                next_us = now; /* behind: don't accumulate debt */
            }
        }
    }

    s->core = NULL;
    adamcore_destroy(core);
    s->running = 0;
    return NULL;
}

/****************************************************************************/
/** Lifecycle                                                              **/
/****************************************************************************/

adamsession *adamsession_new(const adamsession_paths *paths)
{
    adamsession *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    if (paths_init(s, paths) != 0) {
        free(s);
        return NULL;
    }

    pthread_mutex_init(&s->lifecycle_mtx, NULL);
    pthread_mutex_init(&s->frame_mtx, NULL);
    pthread_mutex_init(&s->vs_mtx, NULL);
    adam_cond_init_monotonic(&s->vs_cv);

    settings_init(s);
    return s;
}

void adamsession_free(adamsession *s)
{
    if (!s) return;
    adamsession_stop(s);
    adamsession_settings_flush(s);
    settings_free_all(s);
    pthread_mutex_destroy(&s->lifecycle_mtx);
    pthread_mutex_destroy(&s->frame_mtx);
    pthread_mutex_destroy(&s->vs_mtx);
    pthread_cond_destroy(&s->vs_cv);
    free(s);
}

void adamsession_default_opts(adamsession *s, adamsession_start_opts *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->machine = adamsession_get_int(s, "machine", 0);
    opts->cart_path = adamsession_get_str(s, "cart_path", NULL);
    opts->palette = adamsession_get_int(s, "palette", 0);
    opts->expansion = adamsession_get_int(s, "expansion", 0);
    opts->joystick_mode = adamsession_get_int(s, "joystick_mode", 1);
    opts->swap_buttons = adamsession_get_int(s, "swap_buttons", 0);
    opts->reverse_keypad = adamsession_get_int(s, "reverse_keypad", 0);
    opts->enable_fujinet = 1;
    opts->enable_audio = 1;
    opts->enable_gamepad = 1;
}

int adamsession_start(adamsession *s, const adamsession_start_opts *opts)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (s->running) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return 0;
    }

    s->opts = *opts;
    s->cart_path[0] = '\0';
    if (opts->cart_path && opts->cart_path[0])
        snprintf(s->cart_path, sizeof(s->cart_path), "%s", opts->cart_path);

    if (adam_roms_placeholder)
        fprintf(stderr, "adamsession: built without real system ROMs; the "
                        "machine will not boot anything\n");

    /* FujiNet first; its BoIP client retries connecting to the core's
     * listener, so ordering is forgiving. A missing runtime is not fatal:
     * the machine still boots, just without the FujiNet drive. */
    if (opts->enable_fujinet)
        fujinet_start(s);

    s->stop_flag = 0;
    s->frame_serial = 0;
    s->vs_ns = 0;
    s->vs_seen_ns = 0;
    s->vs_iv_us = 0;
    s->running = 1;
    if (pthread_create(&s->emu_thread, NULL, emu_thread_main, s) != 0) {
        s->running = 0;
        fujinet_stop(s);
        session_set_error(s, "could not start the emulator thread");
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return -1;
    }
    s->emu_thread_started = 1;

    if (opts->enable_audio)
        audio_start(s);
    if (opts->enable_gamepad)
        gamepad_start(s);

    pthread_mutex_unlock(&s->lifecycle_mtx);
    return 0;
}

void adamsession_stop(adamsession *s)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->emu_thread_started) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return;
    }
    s->stop_flag = 1;
    /* Unpark a debugger-paused emulator thread so it can exit. */
    if (s->debugger)
        adamdebug_resume_for_stop(s->debugger);
    pthread_mutex_lock(&s->vs_mtx);
    pthread_cond_broadcast(&s->vs_cv);
    pthread_mutex_unlock(&s->vs_mtx);
    pthread_join(s->emu_thread, NULL);
    s->emu_thread_started = 0;
    audio_stop(s);
    gamepad_stop(s);
    fujinet_stop(s);
    s->running = 0;
    pthread_mutex_unlock(&s->lifecycle_mtx);
}

int adamsession_is_running(const adamsession *s)
{
    return s && s->running;
}

const char *adamsession_last_error(const adamsession *s)
{
    return s->last_error;
}

/****************************************************************************/
/** Input / audio / misc accessors                                         **/
/****************************************************************************/

void adamsession_key(adamsession *s, uint8_t adam_code)
{
    adamcore *c = s->core;
    if (c) adamcore_inject_key(c, adam_code);
}

void adamsession_joystick_raw(adamsession *s, int port, uint16_t state)
{
    adamcore *c = s->core;
    if (c) adamcore_set_joystick(c, port, state);
}

void adamsession_reset(adamsession *s, int mode)
{
    adamcore *c = s->core;
    if (c) adamcore_request_reset(c, mode);
}

int adamsession_render_audio(adamsession *s, int16_t *out, int nsamples)
{
    adamcore *c = s->core;
    if (!c || s->audio_mute) {
        memset(out, 0, (size_t)nsamples * sizeof(int16_t));
        return nsamples;
    }
    adamcore_render_audio(c, out, nsamples);
    fujinet_mix_audio(s, out, nsamples, 44100);
    return nsamples;
}

const char *adamsession_config_path(const adamsession *s) { return s->config_dir; }
const char *adamsession_data_path(const adamsession *s) { return s->data_dir; }
const char *adamsession_sd_path(const adamsession *s) { return s->fujinet_sd; }

const char *adamsession_fujinet_webui_url(const adamsession *s)
{
    return s->webui_url;
}

int adamsession_fujinet_running(const adamsession *s)
{
    return s->fujinet_running;
}

int adamsession_fujinet_copy_log(adamsession *s, char *dst, int max)
{
    return fujinet_copy_log(s, dst, max);
}

/****************************************************************************/
/** Media import                                                           **/
/****************************************************************************/

static const char *path_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return "";
    return dot + 1;
}

static int ext_is(const char *ext, const char *want)
{
    for (; *ext && *want; ext++, want++)
        if ((*ext | 0x20) != *want) return 0;
    return *ext == '\0' && *want == '\0';
}

int adamsession_import_media(adamsession *s, const char *src_path,
                             char *dest_out, int dest_sz)
{
    const char *ext = path_ext(src_path);
    const char *base = strrchr(src_path, '/');
    const char *dir;
    char dst[ADAM_PATH_MAX];

    base = base ? base + 1 : src_path;
    if (ext_is(ext, "dsk") || ext_is(ext, "ddp")) {
        if (!s->fujinet_sd[0]) {
            session_set_error(s, "No FujiNet SD directory (FujiNet runtime "
                              "unavailable)");
            return -1;
        }
        dir = s->fujinet_sd;
    } else if (ext_is(ext, "rom") || ext_is(ext, "col") || ext_is(ext, "bin")) {
        dir = s->cart_dir;
    } else {
        session_set_error(s, "Unsupported media type \".%s\" (expected .dsk, "
                          ".ddp, .rom, .col or .bin)", ext);
        return -1;
    }

    mkdir_p(dir);
    snprintf(dst, sizeof(dst), "%s/%s", dir, base);
    if (copy_file(src_path, dst) != 0) {
        session_set_error(s, "Could not copy %s to %s", src_path, dst);
        return -1;
    }
    if (dest_out && dest_sz > 0)
        snprintf(dest_out, (size_t)dest_sz, "%s", dst);
    return 0;
}
