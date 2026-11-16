/*
 * vidcap.c -- the Video tab's capture of the VDP (core/include/adamvidcap.h).
 * The emulator thread (session.c) hands over its core with
 * adam_vidcap_core_set and calls adam_vidcap_frame_done after each frame;
 * tabs ask for captures through the adam_host_capture_* functions.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "adamcore.h"
#include "adamcore_debug.h"
#include "adamvidcap.h"
#include "vidcap_internal.h"

static adamcore *s_core;

/* ---- Video tab capture ----------------------------------------
 * While a Video tab exists (s_cap_want > 0) the machine thread copies the VDP
 * and the finished frame once per frame into s_cap, and the core records the
 * VDP registers of every scanline. s_cap_lock guards s_cap, s_core's lifetime
 * for the parked read below, and s_lc_applied. */

static pthread_mutex_t s_cap_lock = PTHREAD_MUTEX_INITIALIZER;
static adamvid_capture *s_cap;
static atomic_int s_cap_want;
static int s_lc_applied;           /* the core's line capture is on (s_cap_lock) */

static void rgb565_to_xrgb(const uint16_t *src, uint32_t *dst, size_t count)
{
    size_t item;
    for (item = 0; item < count; item++) {
        uint32_t pix = src[item];
        uint32_t red = (pix >> 11) & 0x1F, grn = (pix >> 5) & 0x3F, blu = pix & 0x1F;
        red = red << 3 | red >> 2;
        grn = grn << 2 | grn >> 4;
        blu = blu << 3 | blu >> 2;
        dst[item] = red << 16 | grn << 8 | blu;
    }
}

/* Match the core's line capture to whether anybody wants it. Machine thread,
 * or the caller of capture_fill while the machine is parked. */
static void capture_sync_locked(adamcore *core)
{
    int want = atomic_load(&s_cap_want) > 0;
    if (core && want != s_lc_applied) {
        adamcore_vdp_line_capture_enable(core, want);
        s_lc_applied = want;
        if (!want && s_cap) s_cap->serial = 0;
    }
}

void adam_vidcap_sync(void)
{
    pthread_mutex_lock(&s_cap_lock);
    capture_sync_locked(s_core);
    pthread_mutex_unlock(&s_cap_lock);
}

/* Machine thread, a frame just finished. */
void adam_vidcap_frame_done(void)
{
    adamcore *core = s_core;
    adamvid_capture *cap;
    uint64_t frame = 0;
    int line, have = 0;

    pthread_mutex_lock(&s_cap_lock);
    cap = s_cap;
    if (!core || !cap || atomic_load(&s_cap_want) <= 0) {
        pthread_mutex_unlock(&s_cap_lock);
        return;
    }
    adamcore_debug_beam(core, NULL, &frame);
    rgb565_to_xrgb(adamcore_framebuffer(core, NULL, NULL), cap->pixels,
                   (size_t)ADAMVID_FB_W * ADAMVID_FB_H);
    cap->fb_valid = 1;
    adamcore_vdp_state(core, cap->regs, &cap->status, &cap->addr);
    {
        adamcore_vdp_latch lat;
        adamcore_vdp_latch_state(core, &lat);
        cap->latch = lat.latch;
        cap->latched = lat.latched;
        cap->read_ahead = lat.read_ahead;
        cap->int_line = lat.int_line;
    }
    memcpy(cap->palette565, adamcore_palette565(core), sizeof cap->palette565);
    memcpy(cap->vram, adamcore_vdp_vram(core), sizeof cap->vram);

    /* The lines of the frame that just finished carry its index: one less
     * than the count of frames completed now. */
    for (line = 0; line < ADAMVID_LINES; line++) {
        adamvid_line *rec = &cap->log[line];
        uint64_t stamp = 0;
        rec->valid = 0;
        if (frame > 0 &&
            adamcore_vdp_line_regs(core, line, rec->regs, &rec->status, &stamp) &&
            stamp == frame - 1) {
            rec->valid = 1;
            have++;
        }
    }
    cap->log_valid = have == ADAMVID_LINES;
    cap->log_frame = frame ? frame - 1 : 0;
    cap->frame = frame;
    cap->live = 0;
    cap->live_pixels_valid = 0;
    cap->serial++;
    pthread_mutex_unlock(&s_cap_lock);
}

void adam_host_capture_enable(int enable)
{
    pthread_mutex_lock(&s_cap_lock);
    if (enable) {
        if (!s_cap) s_cap = calloc(1, sizeof *s_cap);
        if (s_cap) atomic_fetch_add(&s_cap_want, 1);
    } else if (atomic_load(&s_cap_want) > 0) {
        atomic_fetch_sub(&s_cap_want, 1);
    }
    /* The machine thread applies the change at the top of its next frame. */
    pthread_mutex_unlock(&s_cap_lock);
}

int adam_host_capture_get(adamvid_capture *dst, uint64_t *serial_inout)
{
    int changed = 0;
    pthread_mutex_lock(&s_cap_lock);
    if (s_cap && s_cap->serial != 0 && s_cap->serial != *serial_inout) {
        memcpy(dst, s_cap, sizeof *dst);
        *serial_inout = s_cap->serial;
        changed = 1;
    }
    pthread_mutex_unlock(&s_cap_lock);
    return changed;
}

int adam_host_capture_fill(adamvid_capture *dst)
{
    adamcore *core;
    uint64_t frame = 0;
    int line, beam = 0;
    adamcore_vdp_latch lat;
    adamcore_cpu_aux_state aux;
    adamcore_z80_regs regs;

    pthread_mutex_lock(&s_cap_lock);
    core = s_core;
    if (!core) {
        pthread_mutex_unlock(&s_cap_lock);
        return 0;
    }
    /* The machine is parked, so this is also the moment to start the line
     * log a tab asked for while the thread could not hear it. */
    capture_sync_locked(core);
    adamcore_debug_beam(core, &beam, &frame);
    rgb565_to_xrgb(adamcore_framebuffer(core, NULL, NULL), dst->live_pixels,
                   (size_t)ADAMVID_FB_W * ADAMVID_FB_H);
    dst->live_pixels_valid = 1;
    adamcore_vdp_state(core, dst->regs, &dst->status, &dst->addr);
    adamcore_vdp_latch_state(core, &lat);
    dst->latch = lat.latch;
    dst->latched = lat.latched;
    dst->read_ahead = lat.read_ahead;
    dst->int_line = lat.int_line;
    memcpy(dst->palette565, adamcore_palette565(core), sizeof dst->palette565);
    memcpy(dst->vram, adamcore_vdp_vram(core), sizeof dst->vram);
    for (line = 0; line < ADAMVID_LINES; line++) {
        adamvid_line *rec = &dst->cur[line];
        uint64_t stamp = 0;
        rec->valid = 0;
        if (adamcore_vdp_line_regs(core, line, rec->regs, &rec->status, &stamp) &&
            stamp == frame)
            rec->valid = 1;
    }
    dst->beam_line = beam;
    dst->beam_tstate = adamcore_debug_beam_tstate(core);
    adamcore_cpu_aux(core, &aux);
    dst->nmi_count = aux.nmi_count;
    adamcore_get_regs(core, &regs);
    dst->cycles = regs.cycles;
    dst->frame = frame;
    dst->live = 1;
    pthread_mutex_unlock(&s_cap_lock);
    return 1;
}


/* The emulator thread owns the core: it publishes it here once created and
 * withdraws it, under the capture lock, before adamcore_destroy. */
void adam_vidcap_core_set(adamcore *core)
{
    pthread_mutex_lock(&s_cap_lock);
    s_core = core;
    s_lc_applied = 0;
    if (core) capture_sync_locked(core);
    pthread_mutex_unlock(&s_cap_lock);
}

int adam_host_capture_running(void)
{
    int up;
    pthread_mutex_lock(&s_cap_lock);
    up = s_core != NULL;
    pthread_mutex_unlock(&s_cap_lock);
    return up;
}
