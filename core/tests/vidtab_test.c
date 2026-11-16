/*
 * vidtab_test -- the Video tab controller (core/debugger/vidtab.c) on made-up
 * captures, plus a fuzz. No machine, no BIOS, no debugger: the tab is fed
 * captures directly, so it always runs (and is MinGW-safe: no setenv; the
 * ADAM_DEBUGGER_VIDEO_* variables are set by ctest for `vidtab_test env`).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adamvidtab.h"

static int failures;

static void check(const char *what, int passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

/* ---- made-up machines --------------------------------------------------------------------- */

/* distinct, recognisable colours: entry n is a clear step of red and blue */
static void fill_palette(adamvid_capture *cap)
{
    for (int idx = 0; idx < 16; idx++)
        cap->palette565[idx] = (uint16_t)((idx << 11) | ((15 - idx) << 1) | 0x0020);
}

static void set_regs(adamvid_capture *cap, const uint8_t regs[8])
{
    memcpy(cap->regs, regs, 8);
    for (int line = 0; line < ADAMVID_LINES; line++) {
        cap->log[line].valid = 1;
        memcpy(cap->log[line].regs, regs, 8);
    }
    cap->log_valid = 1;
}

/* the picture the machine would publish, drawn by the decoders themselves */
static void draw_pixels(adamvid_capture *cap)
{
    adamvdp_snapshot snap;
    static uint8_t rgba[256 * 192 * 4];

    memset(&snap, 0, sizeof snap);
    memcpy(snap.regs, cap->regs, 8);
    memcpy(snap.vram, cap->vram, sizeof snap.vram);
    memcpy(snap.palette565, cap->palette565, sizeof snap.palette565);
    adamvdp_render_nametable(&snap, rgba);
    for (int row = 0; row < ADAMVID_FB_H; row++)
        for (int col = 0; col < ADAMVID_FB_W; col++) {
            const int line = row - ADAMVID_TOP;
            uint32_t pix = 0x303030;
            if (line >= 0 && line < ADAMVID_ACTIVE) {
                const uint8_t *src = &rgba[((size_t)line * 256 + (size_t)col) * 4];
                pix = (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2];
            }
            cap->pixels[(size_t)row * ADAMVID_FB_W + (size_t)col] = pix;
        }
    cap->fb_valid = 1;
}

static adamvid_capture *new_cap(void)
{
    adamvid_capture *cap = calloc(1, sizeof *cap);
    if (!cap) { fprintf(stderr, "out of memory\n"); exit(1); }
    fill_palette(cap);
    cap->serial = 1;
    cap->frame = 5;
    cap->log_frame = 4;
    cap->beam_line = -1;
    return cap;
}

/* Graphics I: name table $1400 counting up, patterns $0000 = a stripe, colours $2000 */
static adamvid_capture *g1_cap(void)
{
    static const uint8_t regs[8] = { 0x00, 0xC0, 0x05, 0x80, 0x01, 0x20, 0x00, 0x04 };
    adamvid_capture *cap = new_cap();
    set_regs(cap, regs);
    for (int idx = 0; idx < 768; idx++) cap->vram[0x1400 + idx] = (uint8_t)idx;
    for (int idx = 0; idx < 2048; idx++) cap->vram[idx] = (uint8_t)(idx & 1 ? 0xAA : 0x55);
    for (int idx = 0; idx < 32; idx++) cap->vram[0x2000 + idx] = (uint8_t)(0x40 + idx % 12 + 1);
    cap->vram[0x1000] = 0xD0;      /* sprite attribute table: none */
    draw_pixels(cap);
    return cap;
}

static adamvid_capture *mode_cap(int kind)
{
    adamvid_capture *cap = g1_cap();
    uint8_t regs[8];
    memcpy(regs, cap->regs, 8);
    if (kind == 1) { regs[0] |= 0x02; regs[3] = 0xFF; regs[4] = 0x03; }   /* Graphics II */
    if (kind == 2) regs[1] |= 0x08;                                        /* Multicolor */
    if (kind == 3) { regs[1] |= 0x10; regs[7] = 0xF4; }                    /* Text */
    set_regs(cap, regs);
    if (kind == 1) memset(cap->vram + 0x2000, 0x1F, 0x1800);
    draw_pixels(cap);
    return cap;
}

static unsigned refresh(adamvid_tab *tab) { return adamvid_tab_refresh(tab, 1); }

static const char *section_text(adamvid_tab *tab, int section)
{
    static char buf[8192];
    adamvid_tab_text(tab, section, buf, (int)sizeof buf);
    return buf;
}

/* ---- the checks ---------------------------------------------------------------------------- */

/* the picture holds exactly these pixels (opaque) */
static int same_pixels(const uint32_t *pic, const uint32_t *want, size_t count)
{
    for (size_t item = 0; item < count; item++)
        if (pic[item] != (0xff000000u | want[item])) return 0;
    return 1;
}

static int contains(const char *text, const char *word)
{
    return text && strstr(text, word) != NULL;
}

static void test_display(void)
{
    adamvid_capture *cap = g1_cap();
    adamvid_tab *tab = adamvid_tab_new(NULL);
    int wide = 0, high = 0;
    const uint32_t *pic;
    unsigned flags;

    check("tab created", tab != NULL);
    adamvid_tab_set_overlays(tab, 0);
    adamvid_tab_set_zoom(tab, 1);
    adamvid_tab_feed(tab, cap, 0);
    flags = refresh(tab);
    check("a fed capture needs a repaint", (flags & ADAMVID_TAB_PICTURE) && !(flags & ADAMVID_TAB_OFF));
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("Display draws 256 x 212", pic && wide == 256 && high == 212);
    check("Display is the published frame, pixel for pixel", pic && same_pixels(pic, cap->pixels, 256 * 212));

    adamvid_tab_set_zoom(tab, 3);
    refresh(tab);
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("zoom 3 triples the picture", pic && wide == 768 && high == 636 && adamvid_tab_effective_zoom(tab) == 3);

    adamvid_tab_set_zoom(tab, 0);
    adamvid_tab_set_viewport(tab, 600, 460);
    refresh(tab);
    check("Fit picks the largest zoom that fits (2 in 600 x 460)", adamvid_tab_effective_zoom(tab) == 2);
    adamvid_tab_set_viewport(tab, 5000, 5000);
    refresh(tab);
    check("Fit is capped by the largest picture it will build (zoom 8)", adamvid_tab_effective_zoom(tab) == 8);

    free(cap);
    adamvid_tab_free(tab);
}

static void test_modes(void)
{
    static const char *const names[4] = { "Graphics I", "Graphics II", "Multicolor", "Text" };
    for (int kind = 0; kind < 4; kind++) {
        adamvid_capture *cap = mode_cap(kind);
        adamvid_tab *tab = adamvid_tab_new(NULL);
        int wide = 0, high = 0;
        char what[120];
        const uint32_t *pic;

        adamvid_tab_set_overlays(tab, 0);
        adamvid_tab_set_zoom(tab, 1);
        adamvid_tab_feed(tab, cap, 0);
        adamvid_tab_set_view(tab, ADAMVID_V_DECODED);
        adamvid_tab_set_source(tab, ADAMVID_SRC_SCREEN);
        refresh(tab);
        pic = adamvid_tab_picture(tab, &wide, &high);
        snprintf(what, sizeof what, "%s: Decoded Screen is 256 x 192", names[kind]);
        check(what, pic && wide == 256 && high == 192);
        snprintf(what, sizeof what, "%s: Decoded Screen is the display lines of the frame", names[kind]);
        {
            int same = pic != NULL;
            for (int line = 0; pic && line < ADAMVID_ACTIVE; line++)
                for (int col = 0; col < 256; col++)
                    if (pic[line * 256 + col] != (0xff000000u | cap->pixels[(line + ADAMVID_TOP) * 256 + col])) same = 0;
            check(what, same);
        }
        snprintf(what, sizeof what, "%s: the status names the mode", names[kind]);
        check(what, contains(adamvid_tab_status(tab), names[kind]));
        snprintf(what, sizeof what, "%s: the lines table names the mode", names[kind]);
        check(what, contains(section_text(tab, ADAMVID_S_LINES), names[kind]));

        adamvid_tab_set_source(tab, ADAMVID_SRC_PATTERNS);
        refresh(tab);
        pic = adamvid_tab_picture(tab, &wide, &high);
        snprintf(what, sizeof what, "%s: Patterns sheet", names[kind]);
        if (kind == 2) check("Multicolor has no pattern sheet, and says why", !pic && contains(adamvid_tab_why(tab), "Multicolor"));
        else check(what, pic && wide == 256 && high == 64);

        adamvid_tab_set_source(tab, ADAMVID_SRC_SPRITES);
        refresh(tab);
        pic = adamvid_tab_picture(tab, &wide, &high);
        if (kind == 3) check("Text has no sprites, and says why", !pic && contains(adamvid_tab_why(tab), "Text"));
        else check("Sprites sheet is 128 x 64", pic && wide == 128 && high == 64);

        adamvid_tab_set_source(tab, ADAMVID_SRC_PALETTE);
        refresh(tab);
        pic = adamvid_tab_picture(tab, &wide, &high);
        snprintf(what, sizeof what, "%s: Palette", names[kind]);
        check(what, pic && wide == ADAMVDP_PAL_W && high == ADAMVDP_PAL_H);

        adamvid_tab_set_view(tab, ADAMVID_V_VALUES);
        refresh(tab);
        pic = adamvid_tab_picture(tab, &wide, &high);
        snprintf(what, sizeof what, "%s: Values", names[kind]);
        check(what, pic && wide == (kind == 3 ? 240 : 256) && high == 192);
        free(cap);
        adamvid_tab_free(tab);
    }
}

/* the colour fill_palette gives entry idx, as the decoders expand it */
static uint32_t palette_rgb(const adamvid_capture *cap, int idx)
{
    const uint32_t pix = cap->palette565[idx];
    uint32_t red = (pix >> 11) & 0x1F, grn = (pix >> 5) & 0x3F, blu = pix & 0x1F;
    red = red << 3 | red >> 2;
    grn = grn << 2 | grn >> 4;
    blu = blu << 3 | blu >> 2;
    return 0xff000000u | red << 16 | grn << 8 | blu;
}

/* Multicolor worked out by hand from the datasheet, not from the renderer:
 * a name selects 8 pattern table bytes; row & 3 picks a pair, the half of the
 * cell a byte, and its nibbles are the left and right 4 x 4 blocks. */
static void test_multicolor(void)
{
    adamvid_capture *cap = mode_cap(2);
    adamvid_tab *tab = adamvid_tab_new(NULL);
    uint8_t regs[8];
    int wide, high;
    const uint32_t *pic;

    memcpy(regs, cap->regs, 8);
    regs[4] = 0x01;                              /* pattern table $0800 */
    set_regs(cap, regs);
    memset(&cap->vram[0x1400], 0, 768);
    cap->vram[0x1400 + 32 * 1 + 2] = 3;          /* row 1, column 2 holds tile 3 */
    cap->vram[0x0800 + 3 * 8 + (1 & 3) * 2 + 0] = 0x2A;     /* upper half: blocks 2 and 10 */
    cap->vram[0x0800 + 3 * 8 + (1 & 3) * 2 + 1] = 0x5C;     /* lower half: blocks 5 and 12 */
    adamvid_tab_set_overlays(tab, 0);
    adamvid_tab_set_zoom(tab, 1);
    adamvid_tab_set_view(tab, ADAMVID_V_DECODED);
    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("Multicolor decodes", pic != NULL);
    if (pic) {
        const int orgx = 2 * 8, orgy = 1 * 8;
        check("upper left block", pic[(orgy + 1) * 256 + orgx + 1] == palette_rgb(cap, 2));
        check("upper right block", pic[(orgy + 3) * 256 + orgx + 6] == palette_rgb(cap, 10));
        check("lower left block", pic[(orgy + 5) * 256 + orgx + 2] == palette_rgb(cap, 5));
        check("lower right block", pic[(orgy + 7) * 256 + orgx + 7] == palette_rgb(cap, 12));
    }
    adamvid_tab_hover(tab, 2 * 8 + 6, 1 * 8 + 3);
    {
        const adamvid_pixel *pixp = adamvid_tab_pixel(tab);
        /* the picture is the Screen source: its pixel is the VDP pixel */
        check("the inspector names the colour block", pixp && contains(pixp->text, "Colour block Dark yellow (10)"));
    }
    free(cap);
    adamvid_tab_free(tab);
}

static void test_blanked_and_undefined(void)
{
    adamvid_capture *cap = g1_cap();
    adamvid_tab *tab = adamvid_tab_new(NULL);
    uint8_t regs[8];
    int wide, high;
    const uint32_t *pic;

    memcpy(regs, cap->regs, 8);
    regs[1] &= (uint8_t)~0x40;      /* display off */
    set_regs(cap, regs);
    draw_pixels(cap);
    adamvid_tab_set_overlays(tab, 0);
    adamvid_tab_set_zoom(tab, 1);
    adamvid_tab_set_view(tab, ADAMVID_V_DECODED);
    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("a blanked display decodes to the backdrop", pic && pic[0] == pic[256 * 100 + 100]);
    check("the status says BLANKED", contains(adamvid_tab_status(tab), "BLANKED"));

    regs[1] |= 0x40;
    regs[1] |= 0x18;                /* M1 and M2 together */
    set_regs(cap, regs);
    draw_pixels(cap);
    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);
    check("an undefined mode combination is named", contains(adamvid_tab_status(tab), "undefined"));
    check("...and still decodes", adamvid_tab_picture(tab, &wide, &high) != NULL);
    free(cap);
    adamvid_tab_free(tab);
}

static void test_bands(void)
{
    adamvid_capture *cap = g1_cap();
    adamvid_tab *tab = adamvid_tab_new(NULL);
    const adamvid_band *bands;
    const adamvid_reg *regs;
    int count, wide, high;
    const uint32_t *pic;

    /* a split: from line 96 down the backdrop (R7) and the colour table (R3) change */
    for (int line = 96; line < ADAMVID_LINES; line++) {
        cap->log[line].regs[7] = 0x09;
        cap->log[line].regs[3] = 0x81;
    }
    memcpy(cap->regs, cap->log[ADAMVID_LINES - 1].regs, 8);
    adamvid_tab_set_overlays(tab, ADAMVID_OV_SPLITS);
    adamvid_tab_set_zoom(tab, 1);
    adamvid_tab_set_view(tab, ADAMVID_V_DECODED);
    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);
    count = adamvid_tab_lines(tab, &bands);
    check("a split frame has two bands", count == 2);
    check("...the first lines 0-95", count == 2 && bands[0].first == 0 && bands[0].last == 95);
    check("...the second lines 96-191", count == 2 && bands[1].first == 96 && bands[1].last == 191);
    check("...the second band's backdrop is 9", count == 2 && bands[1].backdrop == 9 && bands[0].backdrop == 4);
    check("...and it counts the registers that changed", count == 2 && bands[1].writes == 2);
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("the Screen is drawn band by band", pic && pic[8 * 256 + 5] != pic[(120 + 8) * 256 + 5]);
    count = adamvid_tab_regs(tab, &regs);
    check("registers R0-R7 and the status", count == 9 && strcmp(regs[8].name, "ST") == 0 && strcmp(regs[0].name, "R0") == 0);
    check("R7 changed once down the frame, at line 96", count == 9 && regs[7].writes == 1 && regs[7].first_line == 96);
    check("R0 did not change", count == 9 && regs[0].writes == 0);
    check("the Registers text mentions the change", contains(section_text(tab, ADAMVID_S_REGISTERS), "first at line 96"));
    check("the Lines text lists both bands", contains(section_text(tab, ADAMVID_S_LINES), "lines 96-191"));
    free(cap);
    adamvid_tab_free(tab);
}

static void test_beam(void)
{
    adamvid_capture *cap = g1_cap();
    adamvid_tab *tab = adamvid_tab_new(NULL);
    const uint32_t *pic;
    int wide, high;
    uint32_t above, below;

    adamvid_tab_set_overlays(tab, 0);
    adamvid_tab_set_zoom(tab, 1);
    adamvid_tab_set_view(tab, ADAMVID_V_BEAM);
    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);
    check("Beam needs a stopped machine, and says so", !adamvid_tab_picture(tab, &wide, &high) && contains(adamvid_tab_why(tab), "stopped"));

    cap->live = 1;
    cap->live_pixels_valid = 1;
    memcpy(cap->live_pixels, cap->pixels, sizeof cap->live_pixels);
    cap->beam_line = 50;
    cap->beam_tstate = 100;
    adamvid_tab_feed(tab, cap, 1);
    refresh(tab);
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("Beam draws when stopped", pic && wide == 256 && high == 212);
    if (pic) {
        above = pic[(ADAMVID_TOP + 20) * 256 + 5];
        below = pic[(ADAMVID_TOP + 120) * 256 + 5];
        check("the lines above the beam are as published", above == (0xff000000u | cap->pixels[(ADAMVID_TOP + 20) * 256 + 5]));
        check("the lines below the beam are dimmed", below != (0xff000000u | cap->pixels[(ADAMVID_TOP + 120) * 256 + 5]));
    }
    check("the beam's place in the frame", adamvid_tab_beam_permille(tab) == 50 * 1000 / ADAMVID_LINES);
    check("the status gives the beam", contains(adamvid_tab_status(tab), "beam at line 50"));
    check("the timing gives the beam", contains(section_text(tab, ADAMVID_S_TIMING), "line 50"));

    adamvid_tab_set_view(tab, ADAMVID_V_DISPLAY);
    refresh(tab);
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("Display ignores the beam (the main window's picture)", pic && same_pixels(pic, cap->pixels, 256 * 212));
    free(cap);
    adamvid_tab_free(tab);
}

static void put_sprite(adamvid_capture *cap, int index, int ypos, int xpos, int pattern, int colour)
{
    uint8_t *entry = &cap->vram[0x1000 + index * 4];
    entry[0] = (uint8_t)ypos; entry[1] = (uint8_t)xpos; entry[2] = (uint8_t)pattern; entry[3] = (uint8_t)colour;
}

static void test_inspector(void)
{
    adamvid_capture *cap = g1_cap();
    adamvid_tab *tab = adamvid_tab_new(NULL);
    const adamvid_pixel *pixp;
    int hover[4], pin[4], flags;

    adamvid_tab_set_overlays(tab, 0);
    adamvid_tab_set_zoom(tab, 2);
    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);

    check("no pixel before the pointer arrives", adamvid_tab_pixel(tab) == NULL);
    adamvid_tab_hover(tab, 2 * 40 + 1, 2 * (ADAMVID_TOP + 20) + 1);      /* column 5, row 2 */
    pixp = adamvid_tab_pixel(tab);
    check("hover finds the tile", pixp && pixp->kind == ADAMVID_PK_TILE && pixp->cell == 2 * 32 + 5);
    check("...its name table entry's address", pixp && pixp->vram_addr == 0x1400u + 2 * 32 + 5);
    check("...its display line", pixp && pixp->line == 20 && pixp->vdp_x == 40 && pixp->vdp_y == 20);
    check("...the text names the entry", pixp && contains(pixp->text, "Name table entry 69 (column 5, row 2) at $1445"));
    check("...break targets: name entry, pattern bytes, colour byte", pixp && pixp->nbreak == 3 && pixp->break_kind[0] == ADAMVID_BK_VRAM);
    check("Break on writes needs a debugger", !adamvid_tab_can_break(tab) && adamvid_tab_break_on_pixel(tab) == -1);
    flags = adamvid_tab_marks(tab, hover, pin);
    check("the hover mark is the 8 x 8 cell at the zoom", flags == 1 && hover[0] == 80 && hover[1] == 2 * (ADAMVID_TOP + 16) && hover[2] == 16 && hover[3] == 16);

    adamvid_tab_pin(tab, 2 * 40 + 1, 2 * (ADAMVID_TOP + 20) + 1);
    adamvid_tab_hover(tab, -1, -1);
    pixp = adamvid_tab_pixel(tab);
    check("a pin outlives the pointer", pixp && pixp->cell == 69);
    check("...and the VRAM pane goes to it", adamvid_tab_vram_base(tab) == 0x1440);
    flags = adamvid_tab_marks(tab, hover, pin);
    check("...the pin mark is reported", flags == 2);
    adamvid_tab_pin(tab, 2 * 41, 2 * (ADAMVID_TOP + 17));       /* inside the same cell */
    check("pinning the same cell unpins", adamvid_tab_pixel(tab) == NULL);

    adamvid_tab_hover(tab, 2, 2);
    pixp = adamvid_tab_pixel(tab);
    check("the top border row is the backdrop", pixp && pixp->kind == ADAMVID_PK_BORDER && contains(pixp->text, "Border"));
    check("...with a register target (R7)", pixp && pixp->nbreak == 1 && pixp->break_kind[0] == ADAMVID_BK_REGISTER && pixp->break_lo[0] == 7);

    /* sprites: 8x8 at y=39 (line 40), x=100, pattern 1, light red; the pattern is a block */
    memset(&cap->vram[8], 0xFF, 8);
    put_sprite(cap, 0, 39, 100, 1, 9);
    cap->vram[0x1004] = 0xD0;
    draw_pixels(cap);
    {
        /* the core draws the sprite into the frame; here the made-up frame has none, which is
         * fine: the inspector works from the VDP's tables */
        adamvid_tab_feed(tab, cap, 0);
        refresh(tab);
        adamvid_tab_hover(tab, 2 * 102, 2 * (ADAMVID_TOP + 42));
        pixp = adamvid_tab_pixel(tab);
        check("a sprite pixel finds sprite 0", pixp && pixp->kind == ADAMVID_PK_SPRITE && pixp->sprite == 0);
        check("...its attribute and pattern are break targets", pixp && pixp->nbreak == 2 && pixp->break_lo[0] == 0x1000 && pixp->break_hi[0] == 0x1003 && pixp->break_lo[1] == 0x0008);
        check("...the text names the colour", pixp && contains(pixp->text, "Light red (9)"));
    }
    /* five sprites on one line: the fifth is not drawn */
    for (int idx = 0; idx < 5; idx++) put_sprite(cap, idx, 59, 100, 1, 9);
    cap->vram[0x1000 + 5 * 4] = 0xD0;
    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);
    adamvid_tab_hover(tab, 2 * 102, 2 * (ADAMVID_TOP + 62));
    pixp = adamvid_tab_pixel(tab);
    check("the fifth sprite on a line is reported as not drawn", pixp && contains(pixp->text, "four sprites a line") && contains(pixp->text, " 4"));

    free(cap);
    adamvid_tab_free(tab);
}

static void test_sections(void)
{
    adamvid_capture *cap = g1_cap();
    adamvid_tab *tab = adamvid_tab_new(NULL);
    const adamvid_swatch *swat;
    const char *const *timing;
    char *buf = malloc(8192);
    int count;

    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);
    count = adamvid_tab_swatches(tab, &swat);
    check("16 colours", count == 16 && swat[3].index == 3);
    check("the backdrop's role", contains(swat[4].role, "backdrop"));
    check("transparent's role", contains(swat[0].role, "transparent"));
    count = adamvid_tab_timing(tab, &timing);
    check("timing lines are label TAB value", count > 4 && strchr(timing[0], '\t'));
    check("the timing text states the frame", contains(section_text(tab, ADAMVID_S_TIMING), "262 lines"));
    check("the colours text lists the palette", contains(section_text(tab, ADAMVID_S_COLOURS), "Dark blue (4)"));
    check("the VRAM text is a hex dump", contains(section_text(tab, ADAMVID_S_VRAM), "0000  55 AA 55 AA"));
    adamvid_tab_set_vram_base(tab, 0x1403);
    check("the VRAM base rounds to a row", adamvid_tab_vram_base(tab) == 0x1400);
    check("...and the dump follows it", contains(section_text(tab, ADAMVID_S_VRAM), "1400  00 01 02 03"));
    check("the pixel text with no pointer", contains(section_text(tab, ADAMVID_S_PIXEL), "No pixel"));

    check("section names", adamvid_section_from_name("reg") == ADAMVID_S_REGISTERS && adamvid_section_from_name("regs") == ADAMVID_S_REGISTERS &&
          adamvid_section_from_name("VRAM") == ADAMVID_S_VRAM && adamvid_section_from_name("lin") == ADAMVID_S_LINES &&
          adamvid_section_from_name("zzz") == -1 && adamvid_section_from_name("") == -1 && adamvid_section_from_name(NULL) == -1);
    check("view and source keys", strcmp(adamvid_view_key(ADAMVID_V_BEAM), "beam") == 0 && strcmp(adamvid_source_name(ADAMVID_SRC_SPRITES), "Sprites") == 0);
    {
        int total;
        const adamvid_overlay_info *info = adamvid_overlays(&total);
        check("six overlays, one bit each", total == 6 && info[5].bit == ADAMVID_OV_SPLITS && (info[0].bit | info[1].bit | info[2].bit | info[3].bit | info[4].bit | info[5].bit) == ADAMVID_OV_ALL);
    }
    {
        int total;
        const int *zooms = adamvid_zooms(&total);
        check("zoom steps start at Fit", total == 9 && zooms[0] == 0 && zooms[total - 1] == 16);
    }

    check("poke without a debugger is refused", adamvid_tab_poke(tab, "1800: 41", buf, 8192) == -1 && buf[0]);
    check("a bad poke line is refused", adamvid_tab_poke(tab, "zz", buf, 8192) == -1);
    check("an address alone is a go-to", adamvid_tab_poke(tab, "$2010", buf, 8192) == 0 && adamvid_tab_vram_base(tab) == 0x2010);

    check("the console text with no machine says so", adamvid_tab_text_for(NULL, "", buf, 8192) > 0 && contains(buf, "No machine"));
    check("...an unknown section is named", adamvid_tab_text_for(NULL, "bogus", buf, 8192) > 0 && contains(buf, "unknown section"));
    free(buf);
    free(cap);
    adamvid_tab_free(tab);
}

static void test_heat(void)
{
    adamvid_capture *cap = g1_cap();
    adamvid_tab *tab = adamvid_tab_new(NULL);
    int wide, high;
    const uint32_t *pic;
    uint32_t before;

    adamvid_tab_set_overlays(tab, ADAMVID_OV_HEAT);
    adamvid_tab_set_zoom(tab, 1);
    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);
    pic = adamvid_tab_picture(tab, &wide, &high);
    before = pic ? pic[(ADAMVID_TOP + 3) * 256 + 3] : 0;
    cap->vram[0x1400] = 0x41;           /* the first entry changes */
    cap->serial++;
    adamvid_tab_feed(tab, cap, 0);
    refresh(tab);
    pic = adamvid_tab_picture(tab, &wide, &high);
    check("an entry that changed is tinted", pic && pic[(ADAMVID_TOP + 3) * 256 + 3] != before);
    free(cap);
    adamvid_tab_free(tab);
}

/* ---- fuzz ---------------------------------------------------------------------------------- */

static uint32_t rng_state = 0x9E3779B9u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_fuzz(void)
{
    adamvid_capture *cap = calloc(1, sizeof *cap);
    adamvid_tab *tab = adamvid_tab_new(NULL);
    char *buf = malloc(8192);
    int survived = 0;

    for (int round = 0; round < 400; round++) {
        for (size_t item = 0; item < sizeof cap->vram; item += 4) {
            const uint32_t word = rng();
            memcpy(&cap->vram[item], &word, 4);
        }
        if (round % 3 == 0) memset(cap->vram, 0, 0x1000);
        for (int reg = 0; reg < 8; reg++) cap->regs[reg] = (uint8_t)rng();
        cap->status = (uint8_t)rng();
        for (int idx = 0; idx < 16; idx++) cap->palette565[idx] = (uint16_t)rng();
        cap->fb_valid = (int)(rng() % 4 != 0);
        for (size_t item = 0; item < sizeof cap->pixels / sizeof *cap->pixels; item += 7) cap->pixels[item] = rng() & 0xFFFFFF;
        cap->live = (int)(rng() & 1);
        cap->live_pixels_valid = cap->live;
        cap->beam_line = (int)(rng() % 300) - 20;
        cap->beam_tstate = (int)(rng() % 400);
        cap->log_valid = (int)(rng() & 1);
        for (int line = 0; line < ADAMVID_LINES; line++) {
            cap->log[line].valid = (uint8_t)(rng() & 1);
            for (int reg = 0; reg < 8; reg++)
                cap->log[line].regs[reg] = rng() % 7 == 0 ? (uint8_t)rng() : (line ? cap->log[line - 1].regs[reg] : (uint8_t)rng());
            cap->cur[line] = cap->log[line];
        }
        cap->serial++;
        cap->frame++;
        adamvid_tab_set_view(tab, (int)(rng() % ADAMVID_V_COUNT));
        adamvid_tab_set_source(tab, (int)(rng() % ADAMVID_SRC_COUNT));
        adamvid_tab_set_overlays(tab, rng() & ADAMVID_OV_ALL);
        adamvid_tab_set_zoom(tab, (int)(rng() % 17));
        adamvid_tab_set_bank(tab, (int)(rng() % 3));
        adamvid_tab_set_viewport(tab, 1 + (int)(rng() % 2000), 1 + (int)(rng() % 1500));
        adamvid_tab_feed(tab, cap, (int)(rng() & 1));
        adamvid_tab_refresh(tab, (int)(rng() & 1));
        for (int probe = 0; probe < 6; probe++) {
            adamvid_tab_hover(tab, (int)(rng() % 4200) - 100, (int)(rng() % 3400) - 100);
            if (probe == 2) adamvid_tab_pin(tab, (int)(rng() % 4200) - 100, (int)(rng() % 3400) - 100);
            if (probe == 5) adamvid_tab_unpin(tab);
        }
        {
            int hover[4], pin[4];
            adamvid_tab_marks(tab, hover, pin);
        }
        for (int sec = 0; sec < ADAMVID_S_COUNT; sec++) adamvid_tab_text(tab, sec, buf, (rng() & 1) ? 8192 : 40);
        adamvid_tab_set_vram_base(tab, rng());
        adamvid_tab_refresh(tab, 1);
        survived++;
    }
    check("400 random captures, views, zooms and pointers survived", survived == 400);
    free(buf);
    free(cap);
    adamvid_tab_free(tab);
}

/* ---- env ------------------------------------------------------------------------------------ */

/* run by ctest with ADAM_DEBUGGER_VIDEO_* in the environment */
static int test_env(void)
{
    adamvid_tab *tab = adamvid_tab_new(NULL);
    check("env: view", adamvid_tab_view(tab) == ADAMVID_V_BEAM);
    check("env: source", adamvid_tab_source(tab) == ADAMVID_SRC_PALETTE);
    check("env: overlays", adamvid_tab_overlays(tab) == (ADAMVID_OV_GRID | ADAMVID_OV_SPLITS));
    check("env: zoom", adamvid_tab_zoom(tab) == 4);
    check("env: section", adamvid_tab_env_section(tab) == ADAMVID_S_TIMING);
    adamvid_tab_free(tab);
    if (failures) {
        fprintf(stderr, "vidtab_test env: %d failure(s)\n", failures);
        return 1;
    }
    printf("vidtab_test env: ok\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "env") == 0)
        return test_env();

    test_display();
    test_modes();
    test_multicolor();
    test_blanked_and_undefined();
    test_bands();
    test_beam();
    test_inspector();
    test_sections();
    test_heat();
    test_fuzz();

    if (failures) {
        fprintf(stderr, "vidtab_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("vidtab_test: ok\n");
    return 0;
}
