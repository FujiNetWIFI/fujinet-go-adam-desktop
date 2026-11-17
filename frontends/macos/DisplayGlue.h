/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdatomic.h>

#import "KeyForward.h"
#include "adamsession.h"

#define FN_VIEW      DisplayView
#define FN_SESSION   adamsession
#define FN_FB_W      ADAMSESSION_FB_WIDTH
#define FN_FB_H      ADAMSESSION_FB_HEIGHT
#define FN_INITIAL_W (ADAMSESSION_FB_WIDTH * 2)
#define FN_INITIAL_H (ADAMSESSION_FB_HEIGHT * 2)

/* The keyboard, start(), and the aspect modes (DisplayExtras.inc). */
#define FN_EXTRA_METHODS "DisplayExtras.inc"

/* Whole-number scaling in mode 2. */
#define FN_SNAP_SIZE 1

/* 0: square pixels, 1: the television's 4:3, 2: whole-number scale. */
static _Atomic int s_fn_aspect_mode = 0;

/* The machine runs 59.92 Hz; the session has no refresh-rate call. */
static inline int fn_machine_hz(FN_SESSION *s) { (void)s; return 60; }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    adamsession_notify_vsync(s, ns);
}

/* The session's frames are RGB565; the view wants 0x00RRGGBB. Only the
 * display link's thread calls this. */
static inline int fn_pull_frame(FN_SESSION *s, uint32_t *dst, int *lines, uint64_t *serial)
{
    static uint16_t src[ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT];
    if (!adamsession_copy_frame(s, src, serial))
        return 0;
    for (int i = 0; i < ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT; i++) {
        const uint16_t p = src[i];
        unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        dst[i] = (r << 16) | (g << 8) | b;
    }
    *lines = ADAMSESSION_FB_HEIGHT;
    return 1;
}

static inline double fn_picture_aspect(FN_SESSION *s, int lines, BOOL tv)
{
    (void)s; (void)tv;
    return atomic_load(&s_fn_aspect_mode) == 1 ? 4.0 / 3.0 : (double)FN_FB_W / (double)lines;
}

/* Mode 2: the largest whole multiple of the frame that fits (at least 1). */
static inline BOOL fn_snap_size(CGSize avail, int width, int lines, CGSize *out)
{
    if (atomic_load(&s_fn_aspect_mode) != 2) return NO;
    int scale = (int)fmin(avail.width / width, avail.height / lines);
    if (scale < 1) scale = 1;
    *out = CGSizeMake(scale * width, scale * lines);
    return YES;
}
