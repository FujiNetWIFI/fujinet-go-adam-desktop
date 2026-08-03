/*
 * VDP visualizer decoders: pure functions over an adamvdp_snapshot that
 * render the TMS9918 tables (nametable, pattern generators, sprites,
 * palette) into RGBA8888 buffers for the native debugger views, plus the
 * register/table decoding the views print beside them.
 *
 * Table addressing per the TI TMS9918A datasheet: Graphics II selects the
 * pattern/color banks through the mask bits in R3/R4; the nametable view
 * honors that banking so what it shows is what the beam would fetch.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
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

/* ---- table / register decoding -------------------------------------------- */

void adamvdp_get_tables(const adamvdp_snapshot *s, adamvdp_tables *out)
{
    const int m1 = (s->regs[1] & 0x10) != 0;
    const int m2 = (s->regs[1] & 0x08) != 0;
    const int m3 = (s->regs[0] & 0x02) != 0;

    memset(out, 0, sizeof(*out));
    out->display_on = (s->regs[1] & 0x40) != 0;
    out->irq_on = (s->regs[1] & 0x20) != 0;
    out->vram_16k = (s->regs[1] & 0x80) != 0;
    out->ext_video = (s->regs[0] & 0x01) != 0;
    out->sprites_16x16 = (s->regs[1] & 0x02) != 0;
    out->sprites_magnified = (s->regs[1] & 0x01) != 0;
    out->backdrop = (uint8_t)(s->regs[7] & 0x0F);
    out->text_fg = (uint8_t)((s->regs[7] >> 4) & 0x0F);

    /* Only one mode bit may be set; the datasheet leaves the rest
     * undefined, and real programs that land there have a bug worth
     * seeing rather than a mode worth guessing at. */
    if (m1 + m2 + m3 > 1)
        out->mode = ADAMVDP_MODE_INVALID;
    else if (m1)
        out->mode = ADAMVDP_MODE_TEXT;
    else if (m2)
        out->mode = ADAMVDP_MODE_MULTICOLOR;
    else if (m3)
        out->mode = ADAMVDP_MODE_GRAPHICS2;
    else
        out->mode = ADAMVDP_MODE_GRAPHICS1;

    out->name.base = (uint16_t)((s->regs[2] & 0x0F) << 10);
    out->sprite_attr.base = (uint16_t)((s->regs[5] & 0x7F) << 7);
    out->sprite_attr.size = 128;
    out->sprite_pattern.base = (uint16_t)((s->regs[6] & 0x07) << 11);
    out->sprite_pattern.size = 2048;

    switch (out->mode) {
    case ADAMVDP_MODE_TEXT:
        out->name.size = 40 * 24;
        out->pattern.base = (uint16_t)((s->regs[4] & 0x07) << 11);
        out->pattern.size = 2048;
        /* Text mode has no color table and no sprites at all. */
        out->sprite_attr.size = 0;
        out->sprite_pattern.size = 0;
        break;
    case ADAMVDP_MODE_MULTICOLOR:
        out->name.size = 32 * 24;
        out->pattern.base = (uint16_t)((s->regs[4] & 0x07) << 11);
        out->pattern.size = 1536; /* 4-row groups, 6 bytes per name */
        break;
    case ADAMVDP_MODE_GRAPHICS2:
        out->name.size = 32 * 24;
        /* The bank bits select a half of VRAM and the low bits become an
         * AND-mask over the index, so a "base" of 0 with mask 0x0FF is the
         * classic one-bank-repeated setup rather than a 6K table. */
        out->pattern.base = (uint16_t)((s->regs[4] & 0x04) << 11);
        out->pattern.size = 6144;
        out->pattern.mask = (uint16_t)(((s->regs[4] & 0x03) << 8) | 0xFF);
        out->color.base = (uint16_t)((s->regs[3] & 0x80) << 6);
        out->color.size = 6144;
        out->color.mask = (uint16_t)(((s->regs[3] & 0x7F) << 3) | 0x07);
        break;
    case ADAMVDP_MODE_GRAPHICS1:
    default:
        out->name.size = 32 * 24;
        out->pattern.base = (uint16_t)((s->regs[4] & 0x07) << 11);
        out->pattern.size = 2048;
        out->color.base = (uint16_t)(s->regs[3] << 6);
        out->color.size = 32; /* one byte per group of 8 characters */
        break;
    }

    switch (out->mode) {
    case ADAMVDP_MODE_GRAPHICS1:  out->mode_name = "Graphics I"; break;
    case ADAMVDP_MODE_GRAPHICS2:  out->mode_name = "Graphics II"; break;
    case ADAMVDP_MODE_MULTICOLOR: out->mode_name = "Multicolor"; break;
    case ADAMVDP_MODE_TEXT:       out->mode_name = "Text"; break;
    default:                      out->mode_name = "undefined"; break;
    }
}

const char *adamvdp_color_name(int index)
{
    static const char *const names[16] = {
        "Transparent", "Black",       "Medium Green", "Light Green",
        "Dark Blue",   "Light Blue",  "Dark Red",     "Cyan",
        "Medium Red",  "Light Red",   "Dark Yellow",  "Light Yellow",
        "Dark Green",  "Magenta",     "Gray",         "White",
    };
    return (index >= 0 && index < 16) ? names[index] : "?";
}

const char *adamvdp_register_name(int reg)
{
    static const char *const names[8] = {
        "Mode / EXTVID",  "Mode / control",     "Name table",
        "Color table",    "Pattern generator",  "Sprite attributes",
        "Sprite patterns", "Colors",
    };
    return (reg >= 0 && reg < 8) ? names[reg] : "?";
}

/* Appends "$XXXX, N bytes" (and the Graphics II index mask when there is
 * one) for a table, or notes that the current mode never fetches it. */
static int describe_table(const adamvdp_table *t, uint16_t raw_base,
                          char *out, int max)
{
    if (t->size == 0)
        return snprintf(out, (size_t)max, "$%04X (unused in this mode)",
                        raw_base);
    if (t->mask)
        return snprintf(out, (size_t)max,
                        "$%04X, %u bytes, index mask $%03X", t->base,
                        (unsigned)t->size, (unsigned)t->mask);
    return snprintf(out, (size_t)max, "$%04X, %u bytes", t->base,
                    (unsigned)t->size);
}

int adamvdp_describe_register(const adamvdp_snapshot *s, int reg, char *out,
                              int max)
{
    adamvdp_tables t;
    int n;

    if (reg < 0 || reg > 7 || max <= 0) {
        if (max > 0) out[0] = '\0';
        return 0;
    }
    adamvdp_get_tables(s, &t);

    switch (reg) {
    case 0:
        n = snprintf(out, (size_t)max, "M3=%d  EXTVID %s",
                     (s->regs[0] & 0x02) ? 1 : 0,
                     t.ext_video ? "on" : "off");
        break;
    case 1: {
        /* The SIZE/MAG bits still read back in text mode, where the VDP
         * fetches no sprites at all -- say so rather than describing
         * sprites that will never appear. */
        char sprites[48];
        if (t.sprite_attr.size == 0)
            snprintf(sprites, sizeof(sprites), "no sprites in this mode");
        else
            snprintf(sprites, sizeof(sprites), "sprites %s%s",
                     t.sprites_16x16 ? "16x16" : "8x8",
                     t.sprites_magnified ? ", magnified" : "");
        /* No mode name here: the state block already leads with it, and
         * the line has to stay narrow enough to read without scrolling. */
        n = snprintf(out, (size_t)max,
                     "%s, display %s, VBlank IRQ %s, M1=%d M2=%d, %s",
                     t.vram_16k ? "16K" : "4K",
                     t.display_on ? "on" : "BLANKED",
                     t.irq_on ? "on" : "off",
                     (s->regs[1] & 0x10) ? 1 : 0, (s->regs[1] & 0x08) ? 1 : 0,
                     sprites);
        break;
    }
    case 2:
        n = describe_table(&t.name, (uint16_t)((s->regs[2] & 0x0F) << 10),
                           out, max);
        break;
    case 3:
        n = describe_table(&t.color, (uint16_t)(s->regs[3] << 6), out, max);
        break;
    case 4:
        n = describe_table(&t.pattern,
                           (uint16_t)((s->regs[4] & 0x07) << 11), out, max);
        break;
    case 5:
        n = describe_table(&t.sprite_attr,
                           (uint16_t)((s->regs[5] & 0x7F) << 7), out, max);
        break;
    case 6:
        n = describe_table(&t.sprite_pattern,
                           (uint16_t)((s->regs[6] & 0x07) << 11), out, max);
        break;
    default:
        if (t.mode == ADAMVDP_MODE_TEXT)
            n = snprintf(out, (size_t)max, "text FG %u (%s), backdrop %u (%s)",
                         t.text_fg, adamvdp_color_name(t.text_fg),
                         t.backdrop, adamvdp_color_name(t.backdrop));
        else
            n = snprintf(out, (size_t)max, "backdrop %u (%s)", t.backdrop,
                         adamvdp_color_name(t.backdrop));
        break;
    }
    return (n < 0) ? 0 : (n >= max ? max - 1 : n);
}

int adamvdp_describe_status(const adamvdp_snapshot *s, char *out, int max)
{
    int n = snprintf(out, (size_t)max,
                     "F=%d (VBlank IRQ %s)  5S=%d  C=%d  5th/last sprite %u",
                     (s->status & 0x80) ? 1 : 0,
                     (s->status & 0x80) ? "pending" : "clear",
                     (s->status & 0x40) ? 1 : 0, (s->status & 0x20) ? 1 : 0,
                     (unsigned)(s->status & 0x1F));
    return (n < 0) ? 0 : (n >= max ? max - 1 : n);
}

/* ---- text views ------------------------------------------------------------ */

/* Bounded append: *pos tracks how much of out is used, and every writer
 * below can ignore truncation because the cursor never passes max-1. */
static void appendf(char *out, int max, int *pos, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*pos >= max - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(out + *pos, (size_t)(max - *pos), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    *pos += (n >= max - *pos) ? (max - *pos - 1) : n;
}

static void append_table_row(char *out, int max, int *pos, const char *label,
                             const adamvdp_table *t)
{
    if (t->size == 0) {
        appendf(out, max, pos, "  %-18s --     (not used in this mode)\n",
                label);
        return;
    }
    appendf(out, max, pos, "  %-18s $%04X  %5u bytes", label, t->base,
            (unsigned)t->size);
    if (t->mask)
        appendf(out, max, pos, "  index mask $%03X", (unsigned)t->mask);
    appendf(out, max, pos, "\n");
}

int adamvdp_format_state(const adamvdp_snapshot *s, char *out, int max)
{
    adamvdp_tables t;
    int pos = 0, i;

    if (max <= 0)
        return 0;
    out[0] = '\0';
    adamvdp_get_tables(s, &t);

    appendf(out, max, &pos, "Mode  %s%s\n", t.mode_name,
            t.display_on ? "" : "   (display blanked)");
    appendf(out, max, &pos,
            "VRAM address counter $%04X    backdrop %u (%s)\n\n", s->addr,
            t.backdrop, adamvdp_color_name(t.backdrop));

    appendf(out, max, &pos, "Tables\n");
    append_table_row(out, max, &pos, "Name table", &t.name);
    append_table_row(out, max, &pos, "Color table", &t.color);
    append_table_row(out, max, &pos, "Pattern generator", &t.pattern);
    append_table_row(out, max, &pos, "Sprite attributes", &t.sprite_attr);
    append_table_row(out, max, &pos, "Sprite patterns", &t.sprite_pattern);

    appendf(out, max, &pos, "\nRegisters\n");
    for (i = 0; i < 8; i++) {
        char detail[160];
        adamvdp_describe_register(s, i, detail, (int)sizeof(detail));
        appendf(out, max, &pos, "  R%d $%02X  %-18s %s\n", i, s->regs[i],
                adamvdp_register_name(i), detail);
    }
    {
        char detail[160];
        adamvdp_describe_status(s, detail, (int)sizeof(detail));
        appendf(out, max, &pos, "  ST $%02X  %-18s %s\n", s->status,
                "Status (read-only)", detail);
    }
    return pos;
}

int adamvdp_format_hex(const adamvdp_snapshot *s, uint16_t base, int rows,
                       char *out, int max)
{
    int pos = 0, row, col;

    if (max <= 0)
        return 0;
    out[0] = '\0';
    for (row = 0; row < rows; row++) {
        uint16_t addr = (uint16_t)((base + row * 16) & 0x3FFF);
        appendf(out, max, &pos, "%04X  ", addr);
        for (col = 0; col < 16; col++)
            appendf(out, max, &pos, "%02X ",
                    s->vram[(addr + col) & 0x3FFF]);
        appendf(out, max, &pos, " ");
        for (col = 0; col < 16; col++) {
            uint8_t c = s->vram[(addr + col) & 0x3FFF];
            appendf(out, max, &pos, "%c",
                    (c >= 0x20 && c <= 0x7E) ? (char)c : '.');
        }
        appendf(out, max, &pos, "\n");
    }
    return pos;
}

/* ---- poke-line parsing ----------------------------------------------------- */

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *skip_seps(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == ',')
        p++;
    return p;
}

int adamvdp_parse_poke(const char *text, uint16_t *addr, uint8_t *bytes,
                       int max)
{
    const char *p = skip_seps(text);
    unsigned long v = 0;
    int digits = 0, n = 0;

    if (*p == '$')
        p++;
    else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    while (hex_digit((unsigned char)*p) >= 0) {
        v = v * 16 + (unsigned long)hex_digit((unsigned char)*p++);
        digits++;
    }
    if (digits == 0 || digits > 4)
        return -1;
    *addr = (uint16_t)(v & 0x3FFF);

    p = skip_seps(p);
    if (*p == ':' || *p == '=')
        p++;
    p = skip_seps(p);

    while (*p) {
        /* One token: any even run of hex digits, so a pasted dump row and
         * hand-typed "41 42" both come through as the same bytes. */
        int td = 0;
        while (hex_digit((unsigned char)*p) >= 0) {
            int hi = hex_digit((unsigned char)*p++);
            int lo;
            if (td == 0 && hex_digit((unsigned char)*p) < 0) {
                /* A lone digit is a nibble value: "F" means 0x0F. */
                if (n >= max) return -1;
                bytes[n++] = (uint8_t)hi;
                td += 2;
                break;
            }
            lo = hex_digit((unsigned char)*p);
            if (lo < 0) return -1; /* odd-length run: ambiguous, reject */
            p++;
            if (n >= max) return -1;
            bytes[n++] = (uint8_t)((hi << 4) | lo);
            td += 2;
        }
        if (td == 0)
            return -1; /* trailing junk that is not hex */
        p = skip_seps(p);
    }
    return n;
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

/* 3x5 hex digits for the swatch labels, one bit per pixel, MSB left. */
static const uint8_t digit_font[16][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 2, 2, 2},
    {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {7, 5, 7, 5, 5}, {6, 5, 6, 5, 6},
    {7, 4, 4, 4, 7}, {6, 5, 5, 5, 6}, {7, 4, 7, 4, 7}, {7, 4, 7, 4, 4},
};

static void fill_rect(uint8_t *rgba, int x0, int y0, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int x, y;
    for (y = y0; y < y0 + h; y++) {
        uint8_t *row = rgba + ((size_t)y * ADAMVDP_PAL_W + x0) * 4;
        for (x = 0; x < w; x++, row += 4) {
            row[0] = r;
            row[1] = g;
            row[2] = b;
            row[3] = 0xFF;
        }
    }
}

void adamvdp_render_palette(const adamvdp_snapshot *s, uint8_t *rgba)
{
    const int inset = 3, scale = 2;
    int i;

    /* Gutter: every swatch sits in its own outlined tile, so two entries
     * that happen to hold the same color still read as two entries. */
    fill_rect(rgba, 0, 0, ADAMVDP_PAL_W, ADAMVDP_PAL_H, 0x1E, 0x1E, 0x22);

    for (i = 0; i < 16; i++) {
        const int cx = (i % ADAMVDP_PAL_COLS) * ADAMVDP_PAL_CELL + inset;
        const int cy = (i / ADAMVDP_PAL_COLS) * ADAMVDP_PAL_CELL + inset;
        const int side = ADAMVDP_PAL_CELL - inset * 2;
        uint8_t rgb[4];
        uint8_t lr, lg, lb;
        int x, y, row;

        put_rgba(rgb, s->palette565[i]);
        fill_rect(rgba, cx - 1, cy - 1, side + 2, side + 2, 0x6E, 0x6E, 0x78);
        fill_rect(rgba, cx, cy, side, side, rgb[0], rgb[1], rgb[2]);
        if (i == 0) {
            /* Entry 0 is transparent, not black: checker it so the two are
             * never confused at a glance. */
            for (y = 0; y < side; y++)
                for (x = 0; x < side; x++)
                    if (((x / 5) + (y / 5)) & 1)
                        fill_rect(rgba, cx + x, cy + y, 1, 1, 0x4A, 0x4A, 0x52);
        }

        /* Label in whichever of black/white stays legible on the swatch
         * (Rec. 601 luma, the same rule the sprite view uses for gray). */
        {
            int luma = (rgb[0] * 299 + rgb[1] * 587 + rgb[2] * 114) / 1000;
            uint8_t v = (uint8_t)(luma > 128 ? 0x00 : 0xFF);
            lr = lg = lb = v;
        }
        for (row = 0; row < 5; row++) {
            for (x = 0; x < 3; x++) {
                if (!(digit_font[i][row] & (0x04 >> x)))
                    continue;
                fill_rect(rgba, cx + 3 + x * scale, cy + 3 + row * scale,
                          scale, scale, lr, lg, lb);
            }
        }
    }
}
