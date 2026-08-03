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

static void check_u(const char *what, unsigned got, unsigned want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %u ($%X) want %u ($%X)\n", what, got,
                got, want, want);
        failures++;
    }
}

/* The table decoder must agree with the addressing the renderers above use,
 * which is what makes the printed addresses trustworthy while debugging. */
static void test_tables(void)
{
    static adamvdp_snapshot s;
    adamvdp_tables t;

    memset(&s, 0, sizeof(s));
    s.regs[0] = 0x02; /* M3: Graphics II */
    s.regs[1] = 0xE2; /* 16K, display on, IRQ on, 16x16 sprites */
    s.regs[2] = 0x0E;
    s.regs[3] = 0xFF;
    s.regs[4] = 0x03;
    s.regs[5] = 0x36;
    s.regs[6] = 0x07;
    s.regs[7] = 0x01;

    adamvdp_get_tables(&s, &t);
    if (t.mode != ADAMVDP_MODE_GRAPHICS2) {
        fprintf(stderr, "FAIL tables: mode %d want Graphics II\n", t.mode);
        failures++;
    }
    check_u("G2 name base", t.name.base, 0x3800);
    check_u("G2 name size", t.name.size, 768);
    check_u("G2 color base", t.color.base, 0x2000);
    check_u("G2 color mask", t.color.mask, 0x3FF);
    check_u("G2 pattern base", t.pattern.base, 0x0000);
    check_u("G2 pattern mask", t.pattern.mask, 0x3FF);
    check_u("G2 sprite attr", t.sprite_attr.base, 0x1B00);
    check_u("G2 sprite pattern", t.sprite_pattern.base, 0x3800);
    check_u("G2 backdrop", t.backdrop, 1);
    if (!t.display_on || !t.irq_on || !t.vram_16k || !t.sprites_16x16) {
        fprintf(stderr, "FAIL tables: R1 flags decoded wrong\n");
        failures++;
    }

    /* Graphics I: color table is 32 bytes at R3<<6, patterns at R4<<11. */
    s.regs[0] = 0x00;
    s.regs[3] = 0x20;
    s.regs[4] = 0x01;
    adamvdp_get_tables(&s, &t);
    check_u("G1 color base", t.color.base, 0x0800);
    check_u("G1 color size", t.color.size, 32);
    check_u("G1 color mask", t.color.mask, 0);
    check_u("G1 pattern base", t.pattern.base, 0x0800);
    check_u("G1 pattern size", t.pattern.size, 2048);

    /* Text mode: 960-byte name table, no color table, no sprites. */
    s.regs[1] = 0xF0;
    adamvdp_get_tables(&s, &t);
    if (t.mode != ADAMVDP_MODE_TEXT) {
        fprintf(stderr, "FAIL tables: mode %d want Text\n", t.mode);
        failures++;
    }
    check_u("text name size", t.name.size, 960);
    check_u("text color size", t.color.size, 0);
    check_u("text sprite attr size", t.sprite_attr.size, 0);

    /* Two mode bits at once is not a mode. */
    s.regs[0] = 0x02;
    adamvdp_get_tables(&s, &t);
    if (t.mode != ADAMVDP_MODE_INVALID) {
        fprintf(stderr, "FAIL tables: M1+M3 should be undefined, got %d\n",
                t.mode);
        failures++;
    }
}

static void test_describe(void)
{
    static adamvdp_snapshot s;
    char buf[160];

    memset(&s, 0, sizeof(s));
    s.regs[0] = 0x02;
    s.regs[1] = 0xE2;
    s.regs[3] = 0xFF;
    s.regs[7] = 0x0F;
    s.status = 0x80;

    adamvdp_describe_register(&s, 3, buf, sizeof(buf));
    if (!strstr(buf, "$2000") || !strstr(buf, "$3FF")) {
        fprintf(stderr, "FAIL describe R3: \"%s\"\n", buf);
        failures++;
    }
    adamvdp_describe_register(&s, 7, buf, sizeof(buf));
    if (!strstr(buf, "White")) {
        fprintf(stderr, "FAIL describe R7: \"%s\"\n", buf);
        failures++;
    }
    adamvdp_describe_status(&s, buf, sizeof(buf));
    if (!strstr(buf, "F=1")) {
        fprintf(stderr, "FAIL describe status: \"%s\"\n", buf);
        failures++;
    }
    /* Truncation must still leave a terminated string. */
    adamvdp_describe_register(&s, 1, buf, 8);
    if (strlen(buf) > 7) {
        fprintf(stderr, "FAIL describe truncation: %u chars\n",
                (unsigned)strlen(buf));
        failures++;
    }
}

static void check_poke(const char *text, int want_n, unsigned want_addr,
                       const uint8_t *want_bytes)
{
    uint16_t addr = 0xFFFF;
    uint8_t bytes[8];
    int n = adamvdp_parse_poke(text, &addr, bytes, 8);

    if (n != want_n) {
        fprintf(stderr, "FAIL poke \"%s\": count %d want %d\n", text, n,
                want_n);
        failures++;
        return;
    }
    if (n < 0)
        return;
    if (addr != want_addr) {
        fprintf(stderr, "FAIL poke \"%s\": addr $%04X want $%04X\n", text,
                addr, (unsigned)want_addr);
        failures++;
    }
    if (want_bytes && memcmp(bytes, want_bytes, (size_t)n) != 0) {
        fprintf(stderr, "FAIL poke \"%s\": bytes differ\n", text);
        failures++;
    }
}

static void test_parse_poke(void)
{
    static const uint8_t abc[] = {0x41, 0x42, 0x43};
    static const uint8_t one[] = {0x0F};

    check_poke("1800", 0, 0x1800, NULL);
    check_poke("$1800", 0, 0x1800, NULL);
    check_poke("0x1800", 0, 0x1800, NULL);
    check_poke("  1800 : 41 42 43", 3, 0x1800, abc);
    check_poke("1800=414243", 3, 0x1800, abc);
    check_poke("1800,41,42,43", 3, 0x1800, abc);
    check_poke("0 F", 1, 0x0000, one);      /* a lone digit is a nibble */
    /* VRAM is 16K: addresses wrap rather than running off the array. */
    check_poke("4001", 0, 0x0001, NULL);
    check_poke("", -1, 0, NULL);
    check_poke("zz", -1, 0, NULL);
    check_poke("1800 414", -1, 0, NULL);    /* odd-length run: ambiguous */
    check_poke("1800 41 zz", -1, 0, NULL);  /* junk after the address */
    check_poke("1800 0102030405060708 09", -1, 0, NULL); /* over max */
}

/* The palette view has to give each entry its own outlined square: adjacent
 * entries holding the same color must still be visibly separate. */
static void test_palette_swatches(void)
{
    static adamvdp_snapshot s;
    static uint8_t pal[ADAMVDP_PAL_W * ADAMVDP_PAL_H * 4];
    const int centre = ADAMVDP_PAL_CELL / 2;
    int i;

    memset(&s, 0, sizeof(s));
    for (i = 0; i < 16; i++)
        s.palette565[i] = 0xF800; /* every entry the same red */
    adamvdp_render_palette(&s, pal);

    for (i = 0; i < 16; i++) {
        int cx = (i % ADAMVDP_PAL_COLS) * ADAMVDP_PAL_CELL + centre;
        int cy = (i / ADAMVDP_PAL_COLS) * ADAMVDP_PAL_CELL + centre;
        const uint8_t *px = pal + ((size_t)cy * ADAMVDP_PAL_W + cx) * 4;
        if (px[0] != 0xFF || px[1] != 0 || px[2] != 0) {
            fprintf(stderr, "FAIL palette: entry %d centre not the color\n",
                    i);
            failures++;
        }
    }
    /* The gutter between two horizontally adjacent swatches is not the
     * swatch color. */
    {
        const uint8_t *px =
            pal + ((size_t)centre * ADAMVDP_PAL_W + ADAMVDP_PAL_CELL - 1) * 4;
        if (px[0] == 0xFF && px[1] == 0 && px[2] == 0) {
            fprintf(stderr, "FAIL palette: no separation between swatches\n");
            failures++;
        }
    }
}

int main(void)
{
    test_graphics1();
    test_graphics2();
    test_sprites();
    test_tables();
    test_describe();
    test_parse_poke();
    test_palette_swatches();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("vdp_decode: G1/G2/sprite decoding, tables, poke parsing, "
           "palette ok\n");
    return 0;
}
