/*
 * VDP visualizer decoders: pure functions over an adamvdp_snapshot that
 * render the TMS9918 tables (nametable, pattern generators, sprites,
 * palette) into RGBA8888 buffers for the native debugger views.
 *
 * Table addressing per the TI TMS9918A datasheet: Graphics II selects the
 * pattern/color banks through the mask bits in R3/R4; the nametable view
 * honors that banking so what it shows is what the beam would fetch.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "adamdebug.h"

static void put_rgba(uint8_t *dst, uint16_t rgb565)
{
    uint8_t r = (uint8_t)((rgb565 >> 11) & 0x1F);
    uint8_t g = (uint8_t)((rgb565 >> 5) & 0x3F);
    uint8_t b = (uint8_t)(rgb565 & 0x1F);
    dst[0] = (uint8_t)(r << 3 | r >> 2);
    dst[1] = (uint8_t)(g << 2 | g >> 4);
    dst[2] = (uint8_t)(b << 3 | b >> 2);
    dst[3] = 0xFF;
}

static int mode_graphics2(const adamvdp_snapshot *s)
{
    return (s->regs[0] & 0x02) != 0;
}

static int mode_text(const adamvdp_snapshot *s)
{
    return (s->regs[1] & 0x10) != 0;
}

void adamvdp_render_nametable(const adamvdp_snapshot *s, uint8_t *rgba)
{
    const uint16_t nt = (uint16_t)((s->regs[2] & 0x0F) << 10);
    const int g2 = mode_graphics2(s);
    const uint16_t pg_base = g2 ? (uint16_t)((s->regs[4] & 0x04) << 11)
                                : (uint16_t)((s->regs[4] & 0x07) << 11);
    /* Graphics II bases/masks must match tms9928a.c's render path exactly
     * (the color base is bit 7 << 6 = 0x2000, not << 5). */
    const uint16_t ct_base = g2 ? (uint16_t)((s->regs[3] & 0x80) << 6)
                                : (uint16_t)(s->regs[3] << 6);
    const uint16_t pg_mask = (uint16_t)(((s->regs[4] & 0x03) << 8) | 0xFF);
    const uint16_t ct_mask = (uint16_t)(((s->regs[3] & 0x7F) << 3) | 0x07);
    const uint16_t backdrop = s->palette565[s->regs[7] & 0x0F];
    int row, col, y, x;

    if (mode_text(s)) {
        /* 40x24 text: 6-pixel glyphs, fixed colors from R7 (fg color 0
         * falls back to the backdrop, as the beam renders it). */
        const uint8_t fg_idx = (uint8_t)((s->regs[7] >> 4) & 0x0F);
        const uint16_t fg =
            fg_idx ? s->palette565[fg_idx] : backdrop;
        memset(rgba, 0, (size_t)256 * 192 * 4);
        for (y = 0; y < 192; y++)
            for (x = 0; x < 256; x++)
                put_rgba(rgba + (y * 256 + x) * 4, backdrop);
        for (row = 0; row < 24; row++) {
            for (col = 0; col < 40; col++) {
                uint8_t name = s->vram[(nt + row * 40 + col) & 0x3FFF];
                for (y = 0; y < 8; y++) {
                    uint8_t bits =
                        s->vram[(pg_base + name * 8 + y) & 0x3FFF];
                    for (x = 0; x < 6; x++) {
                        int px = col * 6 + x + 8; /* center 240px in 256 */
                        int py = row * 8 + y;
                        if (px >= 256) continue;
                        put_rgba(rgba + (py * 256 + px) * 4,
                                 (bits & (0x80 >> x)) ? fg : backdrop);
                    }
                }
            }
        }
        return;
    }

    for (row = 0; row < 24; row++) {
        int bank_entry = row / 8 * 256;
        for (col = 0; col < 32; col++) {
            uint8_t name = s->vram[(nt + row * 32 + col) & 0x3FFF];
            for (y = 0; y < 8; y++) {
                uint16_t pat_off, col_off;
                uint8_t bits, color, fg, bg;
                if (g2) {
                    uint16_t entry =
                        (uint16_t)((bank_entry + name) & pg_mask);
                    uint16_t centry =
                        (uint16_t)((bank_entry + name) & ct_mask);
                    pat_off = (uint16_t)(pg_base + entry * 8 + y);
                    col_off = (uint16_t)(ct_base + centry * 8 + y);
                } else {
                    pat_off = (uint16_t)(pg_base + name * 8 + y);
                    col_off = (uint16_t)(ct_base + name / 8);
                }
                bits = s->vram[pat_off & 0x3FFF];
                color = s->vram[col_off & 0x3FFF];
                fg = (uint8_t)(color >> 4);
                bg = (uint8_t)(color & 0x0F);
                for (x = 0; x < 8; x++) {
                    int set = bits & (0x80 >> x);
                    uint8_t ci = set ? fg : bg;
                    uint16_t rgb = ci ? s->palette565[ci] : backdrop;
                    put_rgba(rgba +
                                 ((row * 8 + y) * 256 + col * 8 + x) * 4,
                             rgb);
                }
            }
        }
    }
}

void adamvdp_render_patterns(const adamvdp_snapshot *s, int bank,
                             uint8_t *rgba)
{
    const int g2 = mode_graphics2(s);
    const uint16_t pg_base = g2 ? (uint16_t)((s->regs[4] & 0x04) << 11)
                                : (uint16_t)((s->regs[4] & 0x07) << 11);
    /* G2: the bank bits pass through R4's low-bit AND mask, exactly as the
     * beam addresses them. */
    const uint16_t pg_mask =
        g2 ? (uint16_t)(((s->regs[4] & 0x03) << 8) | 0xFF) : 0x00FF;
    int tile, y, x;

    for (tile = 0; tile < 256; tile++) {
        int cell_x = (tile % 32) * 8;
        int cell_y = (tile / 32) * 8;
        unsigned idx =
            ((unsigned)(g2 ? bank & 3 : 0) * 256 + (unsigned)tile) & pg_mask;
        for (y = 0; y < 8; y++) {
            uint8_t bits = s->vram[(pg_base + idx * 8 + y) & 0x3FFF];
            for (x = 0; x < 8; x++) {
                uint8_t *px =
                    rgba + ((cell_y + y) * 256 + cell_x + x) * 4;
                uint8_t v = (bits & (0x80 >> x)) ? 0xE6 : 0x20;
                px[0] = px[1] = px[2] = v;
                px[3] = 0xFF;
            }
        }
    }
}

void adamvdp_render_sprites(const adamvdp_snapshot *s, uint8_t *rgba,
                            adamvdp_sprite info[32])
{
    const uint16_t sat = (uint16_t)((s->regs[5] & 0x7F) << 7);
    const uint16_t spg = (uint16_t)((s->regs[6] & 0x07) << 11);
    const int size16 = (s->regs[1] & 0x02) != 0;
    int i, y, x;

    memset(rgba, 0, (size_t)128 * 64 * 4);
    for (i = 0; i < 32; i++) {
        const uint8_t *e = &s->vram[(sat + i * 4) & 0x3FFF];
        int pattern = size16 ? (e[2] & 0xFC) : e[2];
        uint8_t color_idx = (uint8_t)(e[3] & 0x0F);
        uint16_t rgb = s->palette565[color_idx];
        int cell_x = (i % 8) * 16;
        int cell_y = (i / 8) * 16;
        int quads = size16 ? 4 : 1;
        int q;

        if (info) {
            info[i].y = e[0];
            info[i].x = e[1];
            info[i].pattern = pattern;
            info[i].color = color_idx;
            info[i].early_clock = (e[3] & 0x80) != 0;
        }

        for (q = 0; q < quads; q++) {
            /* 16x16 sprites: quadrants are stored column-major
             * (top-left, bottom-left, top-right, bottom-right). */
            int qx = size16 ? (q / 2) * 8 : 0;
            int qy = size16 ? (q % 2) * 8 : 0;
            for (y = 0; y < 8; y++) {
                uint8_t bits =
                    s->vram[(spg + (pattern + q) * 8 + y) & 0x3FFF];
                for (x = 0; x < 8; x++) {
                    if (bits & (0x80 >> x)) {
                        int px = cell_x + qx + x;
                        int py = cell_y + qy + y;
                        put_rgba(rgba + (py * 128 + px) * 4,
                                 color_idx ? rgb : 0x39E7 /* gray */);
                    }
                }
            }
        }
    }
}

void adamvdp_render_palette(const adamvdp_snapshot *s, uint8_t *rgba)
{
    int i;
    for (i = 0; i < 16; i++)
        put_rgba(rgba + i * 4, s->palette565[i]);
}
