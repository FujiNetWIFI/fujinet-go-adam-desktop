/*
 * adamvidcap -- the VDP's state as a frame left it, for the Video tab
 * (adamvidtab.h). Taken once per frame on the emulator thread
 * (core/src/vidcap.c, called next to session_publish_frame) while a tab exists, so a reader
 * sees one consistent frame while the machine runs; a machine parked by the
 * debugger is read live (adam_host_capture_fill).
 *
 * What a capture holds:
 *   - pixels: the last finished frame, 0x00RRGGBB, 256 x 212 (10 border lines
 *     above and below the 192 active ones; Text mode is 240 wide inside it),
 *     the very picture adamsession_copy_frame hands out;
 *   - the VDP at the end of that frame: registers, status, address counter,
 *     the 16 fixed colours and all 16K of VRAM;
 *   - the per-line register log of the frame: the registers and status as
 *     each of the 262 lines finished (adamcore_vdp_line_regs). The TMS9918A
 *     draws every line from the registers as they stand, so this is what
 *     makes split screens and mid-frame mode or colour changes visible.
 *     The model is line-granular: a write in the middle of a line is not
 *     representable and lands on that line's record. The border rows of the
 *     picture are not timed lines (adamcore paints them at the frame's end);
 *   - parked only: the VDP, the VRAM and the frame buffer as they stand, the
 *     records of the lines the frame in progress has finished, and where the
 *     beam is.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAMVIDCAP_H
#define ADAMVIDCAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADAMVID_FB_W      256
#define ADAMVID_FB_H      212
#define ADAMVID_TOP       10      /* picture rows above display line 0 */
#define ADAMVID_ACTIVE    192     /* display lines */
#define ADAMVID_LINES     262     /* NTSC lines in a frame */
#define ADAMVID_VRAM      0x4000

typedef struct adamvid_line {
    uint8_t valid;              /* a record exists for this line */
    uint8_t status;             /* the status register as the line finished */
    uint8_t regs[8];            /* R0-R7 as the line finished */
} adamvid_line;

typedef struct adamvid_capture {
    uint32_t pixels[ADAMVID_FB_W * ADAMVID_FB_H];       /* the last finished frame */
    int fb_valid;
    uint32_t live_pixels[ADAMVID_FB_W * ADAMVID_FB_H];  /* parked: the frame buffer as it stands */
    int live_pixels_valid;

    uint8_t regs[8];            /* R0-R7: at the frame's end, or as they stand when live */
    uint8_t status;
    uint16_t addr;              /* the VRAM address counter */
    uint8_t latch;              /* first byte of a control-port pair */
    uint8_t latched;            /* 1: waiting for the second byte */
    uint8_t read_ahead;
    uint8_t int_line;           /* the INT pin */
    uint16_t palette565[16];
    uint8_t vram[ADAMVID_VRAM];

    int log_valid;              /* all ADAMVID_LINES lines of one finished frame are recorded */
    uint64_t log_frame;         /* the index of that frame (frames completed before it) */
    adamvid_line log[ADAMVID_LINES];    /* the last completed frame */
    adamvid_line cur[ADAMVID_LINES];    /* parked only: the lines the frame in progress finished */

    int beam_line;              /* parked only: the scanline the beam is on, -1 unknown */
    int beam_tstate;            /* parked only: T-states into it */
    uint64_t nmi_count;         /* parked only: VBlank NMIs raised since power-on */
    uint64_t cycles;            /* parked only: the machine timebase */
    uint64_t frame;             /* frames the machine has completed */
    uint64_t serial;            /* 0: nothing captured yet */
    int live;                   /* the VDP part was read from a parked machine */
} adamvid_capture;

/* Frames are captured only while at least one tab wants them (a count:
 * enable(1) from each tab, enable(0) when it is freed). While the count is
 * above zero the machine also records the per-line register log. Thread-safe.
 * The log starts with the next frame the machine runs. */
void adam_host_capture_enable(int enable);

/* 1 while a machine exists that a capture can come from. */
int adam_host_capture_running(void);

/* Copies the latest captured frame into dst iff its serial differs from
 * *serial_inout, updates it and returns 1; 0 and dst untouched otherwise.
 * *serial_inout = 0 forces a copy once something has been captured. */
int adam_host_capture_get(adamvid_capture *dst, uint64_t *serial_inout);

/* Overwrites the VDP part of dst (registers, status, the address counter and
 * latch, palette, VRAM, the live frame buffer, the in-progress line records
 * and the beam) with the machine as it stands, and sets live. The finished
 * frame (pixels, log) is left as it was. Only for a machine parked by the
 * debugger; returns 0, leaving dst alone, when there is no machine. */
int adam_host_capture_fill(adamvid_capture *dst);

#ifdef __cplusplus
}
#endif

#endif
