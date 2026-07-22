/*
 * VDP decoder unit tests against hand-built VRAM snapshots, addressing
 * computed by hand from the TMS9918A datasheet (independently of both the
 * decoder and the core renderer). The Graphics II case pins the color-table
 * base at (R3 bit7) << 6 = 0x2000 -- a decoder regression once rendered
 * every mode-2 color from the wrong half of VRAM.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "adamdebug.h"

static int failures;

/* A recognizable 4-bit-per-entry test palette: palette565[i] encodes i in
 * the red channel so decoded pixels identify their color index. */
static void fill_palette(adamvdp_snapshot *s)
{
    int i;
    for (i = 0; i < 16; i++)
        s->palette565[i] = (uint16_t)(i << 11);
}

static int px_color(const uint8_t *rgba, int stride, int x, int y)
{
    /* Recover the palette index i from the expanded red channel
     * r8 = (i << 3) | (i >> 2): the index lives in bits 3-7. */
    return rgba[(y * stride + x) * 4] >> 3;
}

static void check_px(const char *what, const uint8_t *rgba, int stride,
                     int x, int y, int want)
{
    int got = px_color(rgba, stride, x, y);
    if (got != want) {
        fprintf(stderr, "FAIL %s: pixel (%d,%d) color %d want %d\n", what, x,
                y, got, want);
        failures++;
    }
}

static void test_graphics1(void)
{
    static adamvdp_snapshot s;
    static uint8_t rgba[256 * 192 * 4];

    memset(&s, 0, sizeof(s));
    fill_palette(&s);
    s.regs[0] = 0x00; /* M3 clear: Graphics I */
    s.regs[1] = 0x40;
    s.regs[2] = 0x01; /* nametable 0x0400 */
    s.regs[3] = 0x20; /* color table 0x0800 */
    s.regs[4] = 0x01; /* patterns 0x0800? no: (1)<<11 = 0x0800 */
    s.regs[7] = 0x04; /* backdrop = dark blue (4) */

    /* Char 5 at name cell (0,0): pattern rows 0xF0, color byte fg=9 bg=6. */
    s.vram[0x0400] = 5;
    memset(&s.vram[0x0800 + 5 * 8], 0xF0, 8);
    s.vram[0x0800 + 5 / 8] = 0x96;
    /* Color-table entry index is char/8 -> 0x0800 + 0 written above. */

    adamvdp_render_nametable(&s, rgba);
    check_px("G1 fg", rgba, 256, 0, 0, 9);
    check_px("G1 bg", rgba, 256, 7, 0, 6);
}

static void test_graphics2(void)
{
    static adamvdp_snapshot s;
    static uint8_t rgba[256 * 192 * 4];

    memset(&s, 0, sizeof(s));
    fill_palette(&s);
    s.regs[0] = 0x02; /* M3: Graphics II */
    s.regs[1] = 0x40;
    s.regs[2] = 0x0E; /* nametable 0x3800 */
    s.regs[3] = 0xFF; /* colors at 0x2000, full mask */
    s.regs[4] = 0x03; /* patterns at 0x0000, full mask */
    s.regs[7] = 0x01;

    /* Bank 2 (rows 16-23), name 0x42 at cell row 16, col 3:
     * effective index = 2*256 + 0x42 = 0x242.
     * pattern row 0 at 0x0000 + 0x242*8, colors at 0x2000 + 0x242*8. */
    s.vram[0x3800 + 16 * 32 + 3] = 0x42;
    s.vram[0x242 * 8] = 0xAA;          /* alternating fg/bg */
    s.vram[0x2000 + 0x242 * 8] = 0xC5; /* fg=12, bg=5 */

    adamvdp_render_nametable(&s, rgba);
    check_px("G2 fg", rgba, 256, 3 * 8 + 0, 16 * 8, 12);
    check_px("G2 bg", rgba, 256, 3 * 8 + 1, 16 * 8, 5);

    /* An unset cell elsewhere renders bg=0 -> backdrop (1). */
    check_px("G2 backdrop", rgba, 256, 128, 100, 1);

    /* Pattern-bank view: bank 2, tile 0x42 row 0 must show the 0xAA row. */
    {
        static uint8_t pat[256 * 64 * 4];
        int tile_x = (0x42 % 32) * 8, tile_y = (0x42 / 32) * 8;
        adamvdp_render_patterns(&s, 2, pat);
        if (pat[(tile_y * 256 + tile_x) * 4] <= 0x20) {
            fprintf(stderr, "FAIL G2 patterns: bank-2 tile pixel not set\n");
            failures++;
        }
        if (pat[(tile_y * 256 + tile_x + 1) * 4] ==
            pat[(tile_y * 256 + tile_x) * 4]) {
            fprintf(stderr, "FAIL G2 patterns: 0xAA row not alternating\n");
            failures++;
        }
    }
}

static void test_sprites(void)
{
    static adamvdp_snapshot s;
    static uint8_t rgba[128 * 64 * 4];
    adamvdp_sprite info[32];

    memset(&s, 0, sizeof(s));
    fill_palette(&s);
    s.regs[1] = 0x42;  /* SIZE=1: 16x16 sprites */
    s.regs[5] = 0x36;  /* SAT 0x1B00 */
    s.regs[6] = 0x07;  /* sprite patterns 0x3800 */

    /* Sprite 1: y=40 x=60 pattern 8 color 15. */
    s.vram[0x1B00 + 4] = 40;
    s.vram[0x1B00 + 5] = 60;
    s.vram[0x1B00 + 6] = 8;
    s.vram[0x1B00 + 7] = 0x0F;
    /* Top-left quadrant row 0 fully set. */
    s.vram[0x3800 + 8 * 8] = 0xFF;

    adamvdp_render_sprites(&s, rgba, info);
    if (info[1].y != 40 || info[1].x != 60 || info[1].pattern != 8 ||
        info[1].color != 15) {
        fprintf(stderr, "FAIL sprites: SAT decode wrong (%d,%d,%d,%d)\n",
                info[1].y, info[1].x, info[1].pattern, info[1].color);
        failures++;
    }
    /* Sprite 1's cell is at grid (1,0) -> pixel base (16,0). */
    check_px("sprite px", rgba, 128, 16, 0, 15);
}

int main(void)
{
    test_graphics1();
    test_graphics2();
    test_sprites();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("vdp_decode: G1/G2/sprite decoding ok\n");
    return 0;
}
