/*
 * adamsession FujiNet runtime control: dlopen libfujinet.so and drive the
 * fujinet_desktop_* entry points (the desktop build of fujinet-pc-adam plus
 * the in-process entry wrapper, tools/fujinet/support/fujinet_desktop_entry.cpp).
 * Port of the Android fujinet_android.cpp wrapper.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "session_internal.h"

typedef int (*start_runtime_fn)(const char *root, const char *config,
                                const char *sd, const char *data,
                                int listen_port);
typedef void (*stop_runtime_fn)(void);
typedef const char *(*last_error_fn)(void);
typedef int (*read_audio_fn)(int16_t *out, int max_samples, int rate);
typedef void (*clear_audio_fn)(void);
typedef int (*copy_log_fn)(char *out, int max_bytes);

/* One runtime per process: the library owns background threads (web admin,
 * network listeners) that live inside its mapping, so it is loaded once and
 * NEVER dlclose'd -- unmapping it while any such thread still runs executes
 * freed code and crashes. A stopped runtime is restarted through the same
 * handle. Same pattern (and reasoning) as the Android wrapper. */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static void *g_handle;
static start_runtime_fn g_start;
static stop_runtime_fn g_stop;
static last_error_fn g_last_error;
static read_audio_fn g_read_audio;
static clear_audio_fn g_clear_audio;
static copy_log_fn g_copy_log;

static int load_library_locked(adamsession *s)
{
    if (g_handle) return 0;

    g_handle = dlopen(s->fujinet_lib, RTLD_NOW | RTLD_LOCAL);
    if (!g_handle) {
        session_set_error(s, "FujiNet library load failed: %s", dlerror());
        return -1;
    }
    g_start = (start_runtime_fn)dlsym(g_handle, "fujinet_desktop_start_runtime");
    g_stop = (stop_runtime_fn)dlsym(g_handle, "fujinet_desktop_stop_runtime");
    g_last_error =
        (last_error_fn)dlsym(g_handle, "fujinet_desktop_last_error_message");
    g_read_audio = (read_audio_fn)dlsym(g_handle, "fujinet_desktop_read_audio");
    g_clear_audio =
        (clear_audio_fn)dlsym(g_handle, "fujinet_desktop_clear_audio");
    g_copy_log =
        (copy_log_fn)dlsym(g_handle, "fujinet_desktop_copy_recent_log");

    if (!g_start || !g_stop || !g_last_error) {
        session_set_error(s, "%s is missing the desktop runtime contract",
                          s->fujinet_lib);
        /* Leave the handle mapped (never dlclose); just mark it unusable. */
        g_start = NULL;
        return -1;
    }
    return 0;
}

int fujinet_start(adamsession *s)
{
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    if (paths_provision_fujinet(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        fprintf(stderr, "adamsession: FujiNet runtime unavailable; "
                        "continuing without it\n");
        return -1;
    }
    if (load_library_locked(s) != 0) {
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    if (!g_start(s->fujinet_root, s->fujinet_config, s->fujinet_sd,
                 s->fujinet_data, ADAMSESSION_BOIP_PORT)) {
        const char *err = g_last_error ? g_last_error() : NULL;
        session_set_error(s, "FujiNet runtime failed to start: %s",
                          err && *err ? err : "(unknown)");
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    s->fujinet_running = 1;
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

void fujinet_stop(adamsession *s)
{
    stop_runtime_fn stop = NULL;
    clear_audio_fn clear = NULL;

    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        stop = g_stop;
        clear = g_clear_audio;
        s->fujinet_running = 0;
    }
    pthread_mutex_unlock(&g_mtx);

    if (stop) stop();
    if (clear) clear();
}

int fujinet_copy_log(adamsession *s, char *dst, int max)
{
    copy_log_fn fn;
    (void)s;
    if (!dst || max <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_mtx);
    fn = g_copy_log;
    pthread_mutex_unlock(&g_mtx);
    return fn ? fn(dst, max) : 0;
}

/* Overlay FujiNet (SAM speech) audio onto the emulator mix, saturating. */
void fujinet_mix_audio(adamsession *s, int16_t *buf, int nsamples, int rate)
{
    static int16_t overlay[4096];
    read_audio_fn fn;
    int produced, i;

    if (!s->fujinet_running || nsamples <= 0) return;
    pthread_mutex_lock(&g_mtx);
    fn = g_read_audio;
    pthread_mutex_unlock(&g_mtx);
    if (!fn) return;

    while (nsamples > 0) {
        int chunk = nsamples > (int)(sizeof(overlay) / 2)
                        ? (int)(sizeof(overlay) / 2)
                        : nsamples;
        produced = fn(overlay, chunk, rate);
        if (produced <= 0) return;
        for (i = 0; i < produced; i++) {
            int mixed = (int)buf[i] + (int)overlay[i];
            if (mixed > INT16_MAX) mixed = INT16_MAX;
            if (mixed < INT16_MIN) mixed = INT16_MIN;
            buf[i] = (int16_t)mixed;
        }
        if (produced < chunk) return;
        buf += produced;
        nsamples -= produced;
    }
}
