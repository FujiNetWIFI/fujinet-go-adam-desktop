/*
 * vidtab.c -- the Video tab's controller (core/include/adamvidtab.h).
 *
 * Everything here works from an adamvid_capture: the finished frame, the
 * VDP's registers and VRAM at its end, and the per-line log of the registers
 * as each line finished. The Decoded views rebuild their picture from VRAM
 * and the registers through the vdpview renderers, band by band where the log
 * says the registers changed during the frame, so on an unchanged frame the
 * tiles are the tiles the machine drew.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adamvidtab.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>


#define ADAMVID_MAX_BREAKPOINTS 128     /* the engine's breakpoint table size */
#define MAXPIX 6000000L         /* the largest picture we will build */
#define MAX_TIMING 20
#define MAX_BANDS 64
#define MAX_BAND_RENDERS 32
#define HEAT_FRAMES 24          /* changes are drawn this many captures */
#define NT_MAX 960
#define SPRITES 32
#define SPRITE_TILE 16
#define SPRITE_PITCH 16
#define N_REGS 9                /* R0-R7 and the status register */
#define STATUS_SLOT 8
#define LINE_TSTATES 228
#define CPU_HZ 3579545

#define VDP_WIDE 256            /* the display is 256 pixels across ... */
#define TEXT_XORG 8             /* ... Text mode's 240 sit 8 in from the left */

static const int k_zooms[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };

enum { BK_FRAME = 0, BK_SCREEN, BK_VALUES, BK_PATTERNS, BK_SPRITES, BK_PALETTE };   /* what the base picture is */

enum { MODE_G1 = 0, MODE_G2, MODE_MULTI, MODE_TEXT };

#define COL_BLACK 0xff000000u
#define COL_PANEL 0xff202428u
#define NO_ADDR 0xFFFFFFFFu

typedef struct {
    int kind;                       /* MODE_* */
    int undefined;                  /* more than one of M1, M2, M3 set */
    char name[40];
} mode_rec;

typedef struct {
    int first, last;                /* display lines, inclusive */
    uint8_t regs[8];
    int writes;                     /* registers that changed on its first line */
} band_rec;

struct adamvid_tab {
    adamdebug *dbg;
    int view, source, zoom, vieww, viewh, bank;
    unsigned ovl;
    int env_section, capture_on;
    uint32_t vram_base;

    /* the machine's state as presented */
    adamvid_capture *cap;         /* working capture */
    adamvid_capture *last;        /* last captured by the emulator thread */
    adamvdp_snapshot *snap;       /* the VDP part of cap in vdpview's form */
    mode_rec mode;                  /* of the registers as they stand */
    uint64_t last_serial, cap_serial;
    unsigned live_gen;              /* the debugger generation cap's live part was read at */
    int refill;                     /* read the parked machine again whatever the generation */
    int have, has_fb, has_live, stopped, fed;
    uint64_t sig, tsig;
    int live_valid;                 /* cap holds a live read of the parked machine */

    /* the bands of the frame */
    band_rec bands[MAX_BANDS];
    int nbands;

    /* what changed lately */
    uint8_t prev_regs[N_REGS];
    int reg_age[N_REGS];
    int tracked;
    uint8_t prev_nt[NT_MAX];
    int heat[NT_MAX];
    int nt_cells, nt_cols;
    uint32_t nt_base;

    /* the picture */
    uint32_t *base, *pic;
    size_t base_cap, pic_cap;
    int basew, baseh, picw, pich, effzoom, kind;
    int tile_w;                     /* width of one name table cell in the picture's pixels (6 in Text, else 8) */
    int dirty;
    char why[200];

    /* tables */
    adamvid_reg regs[N_REGS];
    char reg_names[N_REGS][8];
    int nregs;
    adamvid_swatch swat[16];
    char swat_names[16][32];
    adamvid_band lines[MAX_BANDS];
    int nlines;
    char status[400];
    char timing_buf[MAX_TIMING][112];
    const char *timing_ptr[MAX_TIMING];
    int ntiming;
    int reg_writes[N_REGS], reg_first[N_REGS];

    /* pixels */
    int hovx, hovy, pinx, piny;     /* -1: none */
    adamvid_pixel hover, pin;
};

/* ---- tables of names ----------------------------------------------------------------- */

static const adamvid_overlay_info k_overlays[] = {
    { ADAMVID_OV_GRID,    "grid",    "Tile grid" },
    { ADAMVID_OV_SPRITES, "sprites", "Sprite boxes" },
    { ADAMVID_OV_BORDER,  "border",  "Border and blanking" },
    { ADAMVID_OV_BEAM,    "beam",    "Beam" },
    { ADAMVID_OV_HEAT,    "heat",    "Recent writes" },
    { ADAMVID_OV_SPLITS,  "splits",  "Register changes" },
};
#define N_OVERLAYS ((int)(sizeof k_overlays / sizeof *k_overlays))

const adamvid_overlay_info *adamvid_overlays(int *count)
{
    if (count) *count = N_OVERLAYS;
    return k_overlays;
}

static const char *const k_view_names[] = { "Display", "Decoded", "Beam", "Values" };
static const char *const k_view_keys[] = { "display", "decoded", "beam", "values" };
static const char *const k_src_names[] = { "Screen", "Patterns", "Sprites", "Palette" };
static const char *const k_src_keys[] = { "screen", "patterns", "sprites", "palette" };
static const char *const k_sec_names[] = { "Pixel", "Registers", "Colours", "Lines", "Timing", "VRAM" };
static const char *const k_sec_keys[] = { "pixel", "registers", "colours", "lines", "timing", "vram" };

const char *adamvid_view_name(int val) { return val >= 0 && val < ADAMVID_V_COUNT ? k_view_names[val] : ""; }
const char *adamvid_view_key(int val) { return val >= 0 && val < ADAMVID_V_COUNT ? k_view_keys[val] : ""; }
const char *adamvid_source_name(int val) { return val >= 0 && val < ADAMVID_SRC_COUNT ? k_src_names[val] : ""; }
const char *adamvid_source_key(int val) { return val >= 0 && val < ADAMVID_SRC_COUNT ? k_src_keys[val] : ""; }
const char *adamvid_section_name(int sec) { return sec >= 0 && sec < ADAMVID_S_COUNT ? k_sec_names[sec] : ""; }
const char *adamvid_section_key(int sec) { return sec >= 0 && sec < ADAMVID_S_COUNT ? k_sec_keys[sec] : ""; }

int adamvid_section_from_name(const char *name)
{
    if (!name || !*name) return -1;
    if (!strcasecmp(name, "regs")) return ADAMVID_S_REGISTERS;
    for (int sec = 0; sec < ADAMVID_S_COUNT; sec++)
        if (!strncasecmp(name, k_sec_keys[sec], strlen(name))) return sec;
    return -1;
}

const int *adamvid_zooms(int *count)
{
    if (count) *count = (int)(sizeof k_zooms / sizeof *k_zooms);
    return k_zooms;
}

static const char *colour_name(int index)
{
    static const char *const names[16] = {
        "Transparent", "Black", "Medium green", "Light green", "Dark blue", "Light blue",
        "Dark red", "Cyan", "Medium red", "Light red", "Dark yellow", "Light yellow",
        "Dark green", "Magenta", "Grey", "White" };
    return names[index & 15];
}

/* ---- small helpers ---------------------------------------------------------------------- */

static uint64_t fnv(uint64_t high, const void *datap, size_t num)
{
    const uint8_t *byte = datap;
    while (num--) { high ^= *byte++; high *= 1099511628211ull; }
    return high;
}

static uint32_t blend(uint32_t dst, uint32_t rgb, int a256)
{
    const uint32_t dstr = (dst >> 16) & 255, dstg = (dst >> 8) & 255, dstb = dst & 255;
    const uint32_t srcr = (rgb >> 16) & 255, srcg = (rgb >> 8) & 255, srcb = rgb & 255;
    const uint32_t ret = (dstr * (uint32_t)(256 - a256) + srcr * (uint32_t)a256) >> 8;
    const uint32_t grn = (dstg * (uint32_t)(256 - a256) + srcg * (uint32_t)a256) >> 8;
    const uint32_t blu = (dstb * (uint32_t)(256 - a256) + srcb * (uint32_t)a256) >> 8;
    return 0xff000000u | ret << 16 | grn << 8 | blu;
}

static int fits_zoom(int wide, int high, int scale)
{
    return (long)wide * scale * (long)high * scale <= MAXPIX;
}

static void why_set(adamvid_tab *tab, const char *str)
{
    snprintf(tab->why, sizeof tab->why, "%s", str);
}

static int ensure(uint32_t **buf, size_t *cap, size_t need)
{
    if (need > *cap) {
        uint32_t *grown = realloc(*buf, need * sizeof **buf);
        if (!grown) return 0;
        *buf = grown;
        *cap = need;
    }
    return 1;
}

static void rgba_to_pixels(const uint8_t *rgba, uint32_t *dst, size_t count)
{
    for (size_t item = 0; item < count; item++)
        dst[item] = 0xff000000u | (uint32_t)rgba[item * 4] << 16 | (uint32_t)rgba[item * 4 + 1] << 8 | rgba[item * 4 + 2];
}

/* RGB565 -> 0xFFRRGGBB, the same expansion the frame capture and vdpview use */
static uint32_t rgb_of_565(uint16_t pix)
{
    uint32_t red = (pix >> 11) & 0x1F, grn = (pix >> 5) & 0x3F, blu = pix & 0x1F;
    red = red << 3 | red >> 2;
    grn = grn << 2 | grn >> 4;
    blu = blu << 3 | blu >> 2;
    return 0xff000000u | red << 16 | grn << 8 | blu;
}

/* the false colour of a byte: red climbs with the value, green is a triangle
 * of it, blue falls with the high nibble */
static uint32_t value_colour(unsigned byte)
{
    const int low = (int)(byte & 255), high = (int)(byte >> 4);
    const int tri = low < 128 ? low * 2 : (255 - low) * 2;
    return 0xff000000u | (uint32_t)(40 + low * 215 / 255) << 16 |
           (uint32_t)(40 + tri * 160 / 255) << 8 | (uint32_t)(255 - high * 200 / 15);
}

/* the colour index the display shows for a transparent backdrop is black */
static int backdrop_of(const uint8_t *regs)
{
    const int idx = regs[7] & 15;
    return idx ? idx : 1;
}

/* The mode the core draws: Text beats Multicolor beats Graphics II, whatever
 * else is set (tms_render_line). */
static void mode_of(const uint8_t *regs, mode_rec *out)
{
    const int text_bit = (regs[1] & 0x10) != 0, multi_bit = (regs[1] & 0x08) != 0, graphics2_bit = (regs[0] & 0x02) != 0;
    const char *base;

    memset(out, 0, sizeof *out);
    out->kind = text_bit ? MODE_TEXT : multi_bit ? MODE_MULTI : graphics2_bit ? MODE_G2 : MODE_G1;
    out->undefined = text_bit + multi_bit + graphics2_bit > 1;
    base = out->kind == MODE_TEXT ? "Text" : out->kind == MODE_MULTI ? "Multicolor"
         : out->kind == MODE_G2 ? "Graphics II" : "Graphics I";
    if (out->undefined)
        snprintf(out->name, sizeof out->name, "%s (undefined: M1=%d M2=%d M3=%d)", base, text_bit, multi_bit, graphics2_bit);
    else
        snprintf(out->name, sizeof out->name, "%s", base);
}

static int is_text(const mode_rec *mode) { return mode->kind == MODE_TEXT; }
static int has_sprites(const uint8_t *regs) { return !(regs[1] & 0x10); }
static int display_on(const uint8_t *regs) { return (regs[1] & 0x40) != 0; }

/* ---- geometry: the display's place in the published frame ------------------------------ */

/* Display line 0 is picture row ADAMVID_TOP; the display is always 256 wide and
 * 192 lines, and starts the frame, so its lines are the frame's lines 0-191. */

/* ---- the bands of the frame ---------------------------------------------------------------- */

static int regs_differ(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, 8) != 0;
}

static int regs_changes(const uint8_t *left, const uint8_t *right)
{
    int count = 0;
    for (int reg = 0; reg < 8; reg++)
        if (left[reg] != right[reg]) count++;
    return count;
}

/* Walk the display lines through the log: a new band starts where a register
 * differs from the line before. With no log the registers as they stand are
 * one band. */
static void compute_bands(adamvid_tab *tab)
{
    const adamvid_capture *cap = tab->cap;

    tab->nbands = 0;
    for (int line = 0; line < ADAMVID_ACTIVE; line++) {
        const uint8_t *regs = cap->log_valid ? cap->log[line].regs : cap->regs;
        if (tab->nbands == 0 || (regs_differ(tab->bands[tab->nbands - 1].regs, regs) && tab->nbands < MAX_BANDS)) {
            band_rec *band = &tab->bands[tab->nbands];
            band->writes = tab->nbands ? regs_changes(tab->bands[tab->nbands - 1].regs, regs) : 0;
            memcpy(band->regs, regs, sizeof band->regs);
            band->first = line;
            band->last = line;
            tab->nbands++;
        } else {
            tab->bands[tab->nbands - 1].last = line;
        }
    }
}

static const band_rec *band_of_line(const adamvid_tab *tab, int line)
{
    for (int bnd = 0; bnd < tab->nbands; bnd++)
        if (line >= tab->bands[bnd].first && line <= tab->bands[bnd].last) return &tab->bands[bnd];
    return &tab->bands[tab->nbands ? tab->nbands - 1 : 0];
}

/* ---- new / free / settings -------------------------------------------------------------- */

adamvid_tab *adamvid_tab_new(adamdebug *dbg)
{
    adamvid_tab *tab = calloc(1, sizeof *tab);
    const char *envv;
    if (!tab) return NULL;
    tab->dbg = dbg;
    tab->cap = calloc(1, sizeof *tab->cap);
    tab->last = calloc(1, sizeof *tab->last);
    tab->snap = calloc(1, sizeof *tab->snap);
    if (!tab->cap || !tab->last || !tab->snap) {
        adamvid_tab_free(tab);
        return NULL;
    }
    tab->view = ADAMVID_V_DISPLAY;
    tab->source = ADAMVID_SRC_SCREEN;
    tab->ovl = ADAMVID_OV_SPRITES;
    tab->zoom = 0;
    tab->vieww = ADAMVID_FB_W;
    tab->viewh = ADAMVID_FB_H;
    tab->hovx = tab->hovy = tab->pinx = tab->piny = -1;
    tab->dirty = 1;

    if ((envv = getenv("ADAM_DEBUGGER_VIDEO_VIEW")) && *envv) {
        for (int item = 0; item < ADAMVID_V_COUNT; item++)
            if (!strncasecmp(envv, k_view_keys[item], strlen(envv))) { tab->view = item; break; }
    }
    if ((envv = getenv("ADAM_DEBUGGER_VIDEO_SOURCE")) && *envv) {
        for (int item = 0; item < ADAMVID_SRC_COUNT; item++)
            if (!strncasecmp(envv, k_src_keys[item], strlen(envv))) { tab->source = item; break; }
    }
    if ((envv = getenv("ADAM_DEBUGGER_VIDEO_OVERLAYS"))) {
        char buf[200], *tok;
        unsigned ovl = 0;
        snprintf(buf, sizeof buf, "%s", envv);
        for (tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
            if (!strcasecmp(tok, "all")) ovl |= ADAMVID_OV_ALL;
            else if (!strcasecmp(tok, "none")) ovl = 0;
            else
                for (int item = 0; item < N_OVERLAYS; item++)
                    if (!strcasecmp(tok, k_overlays[item].name)) ovl |= k_overlays[item].bit;
        }
        tab->ovl = ovl;
    }
    if ((envv = getenv("ADAM_DEBUGGER_VIDEO_ZOOM")) && *envv) {
        if (!strcasecmp(envv, "fit")) tab->zoom = 0;
        else {
            const int scale = atoi(envv);
            for (size_t item = 1; item < sizeof k_zooms / sizeof *k_zooms; item++)
                if (k_zooms[item] == scale) tab->zoom = scale;
        }
    }
    tab->env_section = ADAMVID_S_PIXEL;
    if ((envv = getenv("ADAM_DEBUGGER_VIDEO_SECTION")) && *envv) {
        const int sec = adamvid_section_from_name(envv);
        if (sec >= 0) tab->env_section = sec;
    }
    adam_host_capture_enable(1);
    tab->capture_on = 1;
    return tab;
}

void adamvid_tab_free(adamvid_tab *tab)
{
    if (!tab) return;
    if (tab->capture_on)
        adam_host_capture_enable(0);
    free(tab->cap);
    free(tab->last);
    free(tab->snap);
    free(tab->base);
    free(tab->pic);
    free(tab);
}

void adamvid_tab_set_view(adamvid_tab *tab, int val)
{
    if (tab && val >= 0 && val < ADAMVID_V_COUNT && val != tab->view) { tab->view = val; tab->dirty = 1; }
}
int adamvid_tab_view(const adamvid_tab *tab) { return tab ? tab->view : 0; }
void adamvid_tab_set_source(adamvid_tab *tab, int val)
{
    if (tab && val >= 0 && val < ADAMVID_SRC_COUNT && val != tab->source) { tab->source = val; tab->dirty = 1; }
}
int adamvid_tab_source(const adamvid_tab *tab) { return tab ? tab->source : 0; }
void adamvid_tab_set_overlays(adamvid_tab *tab, unsigned ovl)
{
    if (tab && (ovl & ADAMVID_OV_ALL) != tab->ovl) { tab->ovl = ovl & ADAMVID_OV_ALL; tab->dirty = 1; }
}
unsigned adamvid_tab_overlays(const adamvid_tab *tab) { return tab ? tab->ovl : 0; }
void adamvid_tab_set_zoom(adamvid_tab *tab, int scale)
{
    if (tab && scale >= 0 && scale <= 16 && scale != tab->zoom) { tab->zoom = scale; tab->dirty = 1; }
}
int adamvid_tab_zoom(const adamvid_tab *tab) { return tab ? tab->zoom : 0; }
void adamvid_tab_set_bank(adamvid_tab *tab, int bank)
{
    if (tab && bank >= 0 && bank < 3 && bank != tab->bank) { tab->bank = bank; tab->dirty = 1; }
}
int adamvid_tab_bank(const adamvid_tab *tab) { return tab ? tab->bank : 0; }
void adamvid_tab_set_vram_base(adamvid_tab *tab, uint32_t addr)
{
    if (tab) tab->vram_base = (addr & (ADAMVID_VRAM - 1)) & ~15u;
}
uint32_t adamvid_tab_vram_base(const adamvid_tab *tab) { return tab ? tab->vram_base : 0; }
void adamvid_tab_set_viewport(adamvid_tab *tab, int wide, int high)
{
    if (!tab) return;
    if (wide < 1) wide = 1;
    if (high < 1) high = 1;
    if (wide != tab->vieww || high != tab->viewh) {
        tab->vieww = wide; tab->viewh = high;
        if (tab->zoom == 0) tab->dirty = 1;
    }
}
int adamvid_tab_env_section(const adamvid_tab *tab) { return tab ? tab->env_section : 0; }

/* ---- feeding ----------------------------------------------------------------------------- */

/* Rebuild what the decoders read from the working capture. */
static void present(adamvid_tab *tab)
{
    const adamvid_capture *cap = tab->cap;
    adamvdp_snapshot *snap = tab->snap;

    memcpy(snap->regs, cap->regs, sizeof snap->regs);
    snap->status = cap->status;
    snap->addr = cap->addr;
    memcpy(snap->vram, cap->vram, sizeof snap->vram);
    memcpy(snap->palette565, cap->palette565, sizeof snap->palette565);
    mode_of(snap->regs, &tab->mode);
    compute_bands(tab);
}

void adamvid_tab_feed(adamvid_tab *tab, const adamvid_capture *cap, int stopped)
{
    if (!tab || !cap) return;
    memcpy(tab->cap, cap, sizeof *tab->cap);
    tab->fed = 1;
    tab->have = 1;
    tab->has_fb = cap->fb_valid || cap->live_pixels_valid;
    tab->stopped = stopped != 0;
    tab->has_live = tab->stopped;
    present(tab);
}

static int dbg_stopped(const adamvid_tab *tab)
{
    return tab->dbg && adamdebug_is_stopped(tab->dbg);
}


/* Take what the machine has. 0 if there is no machine or no frame yet. */
static int acquire(adamvid_tab *tab)
{
    int changed, copied = 0;

    if (tab->fed)
        return tab->have;
    tab->have = 0;
    if (!adam_host_capture_running())
        return 0;
    changed = adam_host_capture_get(tab->last, &tab->last_serial);
    (void)changed;
    tab->stopped = dbg_stopped(tab);
    tab->has_live = tab->stopped;

    if (tab->last->serial != 0 && (tab->cap_serial != tab->last->serial || (tab->live_valid && !tab->stopped))) {
        memcpy(tab->cap, tab->last, sizeof *tab->cap);
        tab->cap_serial = tab->last->serial;
        tab->live_valid = 0;
        copied = 1;
    }
    if (tab->stopped) {
        /* A parked machine does not change on its own: read it once per stop
         * (the generation moves when anything ran or was edited). The last
         * finished frame stays what the Display view shows. */
        const unsigned gen = tab->dbg ? adamdebug_generation(tab->dbg) : 0;
        if (copied || !tab->live_valid || tab->refill || gen != tab->live_gen) {
            if (!adam_host_capture_fill(tab->cap))
                return 0;
            tab->live_valid = 1;
            tab->live_gen = gen;
            tab->refill = 0;
            copied = 1;
        }
    } else if (tab->cap_serial == 0) {
        return 0;
    }
    tab->has_fb = tab->cap->fb_valid || tab->cap->live_pixels_valid;
    tab->have = 1;
    if (copied || tab->dirty)
        present(tab);
    return 1;
}

/* The picture and what it is made from. */
static uint64_t picture_signature(const adamvid_tab *tab)
{
    const adamvid_capture *cap = tab->cap;
    uint64_t high = 1469598103934665603ull;
    high = fnv(high, cap->regs, sizeof cap->regs);
    high = fnv(high, &cap->status, sizeof cap->status);
    high = fnv(high, cap->palette565, sizeof cap->palette565);
    high = fnv(high, cap->vram, sizeof cap->vram);
    high = fnv(high, &cap->fb_valid, sizeof cap->fb_valid);
    high = fnv(high, &cap->live_pixels_valid, sizeof cap->live_pixels_valid);
    if (cap->fb_valid) high = fnv(high, cap->pixels, sizeof cap->pixels);
    if (cap->live_pixels_valid) high = fnv(high, cap->live_pixels, sizeof cap->live_pixels);
    high = fnv(high, &cap->log_valid, sizeof cap->log_valid);
    if (cap->log_valid) high = fnv(high, cap->log, sizeof cap->log);
    high = fnv(high, &cap->beam_line, sizeof cap->beam_line);
    high = fnv(high, &cap->beam_tstate, sizeof cap->beam_tstate);
    high = fnv(high, &cap->live, sizeof cap->live);
    high = fnv(high, &tab->has_live, sizeof tab->has_live);
    high = fnv(high, &tab->has_fb, sizeof tab->has_fb);
    return high;
}

/* Everything the tables show. */
static uint64_t table_signature(const adamvid_tab *tab, uint64_t picture)
{
    uint64_t high = picture;
    high = fnv(high, &tab->cap->frame, sizeof tab->cap->frame);
    high = fnv(high, &tab->cap->nmi_count, sizeof tab->cap->nmi_count);
    high = fnv(high, &tab->cap->cycles, sizeof tab->cap->cycles);
    return high;
}

/* The name table: where it is and how many entries the mode has. */
static void name_table(const adamvid_tab *tab, uint32_t *base, int *cols, int *rows)
{
    *base = (uint32_t)(tab->snap->regs[2] & 0x0F) << 10;
    *cols = is_text(&tab->mode) ? 40 : 32;
    *rows = 24;
}

/* What changed since the capture before (registers, name table entries), for
 * the heat overlay and the Registers section. Called once per new capture. */
static void track(adamvid_tab *tab)
{
    const adamvid_capture *cap = tab->cap;
    uint32_t base;
    int cols, rows, cells;

    for (int reg = 0; reg < N_REGS; reg++) {
        const uint8_t value = reg == STATUS_SLOT ? cap->status : cap->regs[reg];
        if (!tab->tracked) tab->reg_age[reg] = ADAMVID_REG_FADE;
        else if (value != tab->prev_regs[reg]) tab->reg_age[reg] = 0;
        else if (tab->reg_age[reg] < ADAMVID_REG_FADE) tab->reg_age[reg]++;
        tab->prev_regs[reg] = value;
    }
    memset(tab->reg_writes, 0, sizeof tab->reg_writes);
    for (int reg = 0; reg < N_REGS; reg++) tab->reg_first[reg] = -1;
    if (cap->log_valid)
        for (int line = 1; line < ADAMVID_LINES; line++)
            for (int reg = 0; reg < 8; reg++)
                if (cap->log[line].regs[reg] != cap->log[line - 1].regs[reg]) {
                    tab->reg_writes[reg]++;
                    if (tab->reg_first[reg] < 0) tab->reg_first[reg] = line;
                }

    name_table(tab, &base, &cols, &rows);
    cells = cols * rows;
    if (cells > NT_MAX) cells = NT_MAX;
    if (cells != tab->nt_cells || base != tab->nt_base || cols != tab->nt_cols) {
        memset(tab->heat, 0, sizeof tab->heat);
        tab->nt_cells = cells;
        tab->nt_base = base;
        tab->nt_cols = cols;
        for (int item = 0; item < cells; item++) tab->prev_nt[item] = cap->vram[(base + (uint32_t)item) % ADAMVID_VRAM];
        tab->tracked = 1;
        return;
    }
    for (int item = 0; item < cells; item++) {
        const uint8_t now = cap->vram[(base + (uint32_t)item) % ADAMVID_VRAM];
        if (tab->tracked && now != tab->prev_nt[item]) tab->heat[item] = HEAT_FRAMES;
        else if (tab->heat[item] > 0) tab->heat[item]--;
        tab->prev_nt[item] = now;
    }
    tab->tracked = 1;
}

/* ---- the base picture (1x) -------------------------------------------------------------------- */

/* a snapshot with the registers of one band */
static void apply_band(adamvid_tab *tab, const band_rec *band)
{
    memcpy(tab->snap->regs, band->regs, sizeof band->regs);
}

static void restore_state(adamvid_tab *tab)
{
    memcpy(tab->snap->regs, tab->cap->regs, sizeof tab->snap->regs);
}

/* The Screen source: each band rendered with its own registers, so a split
 * screen or a mode change down the frame shows as it was drawn. */
static int build_screen(adamvid_tab *tab)
{
    uint8_t *scratch;
    uint32_t *dst;
    int renders = 0;

    scratch = malloc((size_t)VDP_WIDE * ADAMVID_ACTIVE * 4);
    if (!scratch || !ensure(&tab->base, &tab->base_cap, (size_t)VDP_WIDE * ADAMVID_ACTIVE)) {
        free(scratch);
        return 0;
    }
    dst = tab->base;
    for (int item = 0; item < VDP_WIDE * ADAMVID_ACTIVE; item++) dst[item] = COL_PANEL;
    for (int bnd = 0; bnd < tab->nbands; bnd++) {
        const band_rec *band = &tab->bands[bnd];
        if (renders >= MAX_BAND_RENDERS) break;
        renders++;
        apply_band(tab, band);
        adamvdp_render_nametable(tab->snap, scratch);
        for (int line = band->first; line <= band->last && line < ADAMVID_ACTIVE; line++)
            rgba_to_pixels(scratch + (size_t)line * VDP_WIDE * 4, dst + (size_t)line * VDP_WIDE, VDP_WIDE);
    }
    free(scratch);
    restore_state(tab);
    tab->basew = VDP_WIDE; tab->baseh = ADAMVID_ACTIVE;
    {
        mode_rec first;
        mode_of(tab->bands[0].regs, &first);
        tab->tile_w = is_text(&first) ? 6 : 8;
    }
    tab->kind = BK_SCREEN;
    return 1;
}

static int build_decoded(adamvid_tab *tab)
{
    uint8_t *rgba;
    const adamvdp_snapshot *snap = tab->snap;

    switch (tab->source) {
    case ADAMVID_SRC_PATTERNS:
        if (tab->mode.kind == MODE_MULTI) {
            why_set(tab, "Multicolor has no pattern sheet: its pattern table holds the block colours, two to a byte.");
            return 0;
        }
        rgba = malloc((size_t)256 * 64 * 4);
        if (!rgba || !ensure(&tab->base, &tab->base_cap, (size_t)256 * 64)) { free(rgba); return 0; }
        adamvdp_render_patterns(snap, tab->bank, rgba);
        rgba_to_pixels(rgba, tab->base, (size_t)256 * 64);
        free(rgba);
        tab->basew = 256; tab->baseh = 64;
        tab->kind = BK_PATTERNS;
        return 1;
    case ADAMVID_SRC_SPRITES:
        if (!has_sprites(snap->regs)) {
            why_set(tab, "Text mode has no sprites.");
            return 0;
        }
        rgba = malloc((size_t)128 * 64 * 4);
        if (!rgba || !ensure(&tab->base, &tab->base_cap, (size_t)128 * 64)) { free(rgba); return 0; }
        adamvdp_render_sprites(snap, rgba, NULL);
        for (int item = 0; item < 128 * 64; item++) {
            const uint8_t *src = &rgba[item * 4];
            tab->base[item] = src[3] ? (0xff000000u | (uint32_t)src[0] << 16 | (uint32_t)src[1] << 8 | src[2]) : COL_PANEL;
        }
        free(rgba);
        tab->basew = 128; tab->baseh = 64;
        tab->kind = BK_SPRITES;
        return 1;
    case ADAMVID_SRC_PALETTE:
        rgba = malloc((size_t)ADAMVDP_PAL_W * ADAMVDP_PAL_H * 4);
        if (!rgba || !ensure(&tab->base, &tab->base_cap, (size_t)ADAMVDP_PAL_W * ADAMVDP_PAL_H)) { free(rgba); return 0; }
        adamvdp_render_palette(snap, rgba);
        rgba_to_pixels(rgba, tab->base, (size_t)ADAMVDP_PAL_W * ADAMVDP_PAL_H);
        free(rgba);
        tab->basew = ADAMVDP_PAL_W; tab->baseh = ADAMVDP_PAL_H;
        tab->kind = BK_PALETTE;
        return 1;
    default:
        return build_screen(tab);
    }
}

/* The false colour of each name table entry. */
static int build_values(adamvid_tab *tab)
{
    const adamvdp_snapshot *snap = tab->snap;
    uint32_t base;
    int cols, rows;

    name_table(tab, &base, &cols, &rows);
    {
        const int cell_w = is_text(&tab->mode) ? 6 : 8;
        if (!ensure(&tab->base, &tab->base_cap, (size_t)cols * (size_t)cell_w * (size_t)rows * 8)) return 0;
        for (int cell = 0; cell < cols * rows; cell++) {
            const uint32_t col = value_colour(snap->vram[(base + (uint32_t)cell) % ADAMVID_VRAM]);
            const int orgx = (cell % cols) * cell_w, orgy = (cell / cols) * 8;
            for (int pixy = 0; pixy < 8; pixy++)
                for (int pixx = 0; pixx < cell_w; pixx++)
                    tab->base[(size_t)(orgy + pixy) * (size_t)(cols * cell_w) + (size_t)(orgx + pixx)] = col;
        }
        tab->basew = cols * cell_w; tab->baseh = rows * 8;
        tab->tile_w = cell_w;
        tab->kind = BK_VALUES;
    }
    return 1;
}

/* the picture row below which the beam has not drawn yet */
static int beam_row(const adamvid_capture *cap)
{
    if (cap->beam_line < 0) return 1 << 30;
    if (cap->beam_line >= ADAMVID_ACTIVE) return ADAMVID_FB_H;
    return cap->beam_line + ADAMVID_TOP + 1;
}

static int build_base(adamvid_tab *tab)
{
    const adamvid_capture *cap = tab->cap;

    if (!tab->has_fb) {
        why_set(tab, "No frame has been published yet. Let the machine run for a moment.");
        return 0;
    }
    switch (tab->view) {
    case ADAMVID_V_DISPLAY:
    case ADAMVID_V_BEAM: {
        const uint32_t *src = cap->pixels;
        int below = 1 << 30;
        if (tab->view == ADAMVID_V_BEAM) {
            if (!tab->has_live || !cap->live_pixels_valid) {
                why_set(tab, "The Beam view needs the machine stopped: it shows the frame buffer as it stands with the lines the beam has not reached yet dimmed.");
                return 0;
            }
            src = cap->live_pixels;
            below = beam_row(cap);
        } else if (!cap->fb_valid) {
            src = cap->live_pixels;     /* nothing finished yet: the frame buffer as it stands */
        }
        if (!ensure(&tab->base, &tab->base_cap, (size_t)ADAMVID_FB_W * ADAMVID_FB_H)) return 0;
        for (int posy = 0; posy < ADAMVID_FB_H; posy++)
            for (int posx = 0; posx < ADAMVID_FB_W; posx++) {
                const uint32_t col = 0xff000000u | src[(size_t)posy * ADAMVID_FB_W + (size_t)posx];
                tab->base[(size_t)posy * ADAMVID_FB_W + (size_t)posx] = posy < below ? col : blend(col, 0x000000, 150);
            }
        tab->basew = ADAMVID_FB_W; tab->baseh = ADAMVID_FB_H;
        tab->kind = BK_FRAME;
        return 1;
    }
    case ADAMVID_V_DECODED:
        return build_decoded(tab);
    default:
        return build_values(tab);
    }
}

/* ---- overlays ---------------------------------------------------------------------------- */

typedef struct { uint32_t *pixp; int wide, high; } canvas;

static void px_blend(canvas *cnv, int posx, int posy, uint32_t rgb, int alpha)
{
    if (posx >= 0 && posy >= 0 && posx < cnv->wide && posy < cnv->high)
        cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx] = blend(cnv->pixp[(size_t)posy * (size_t)cnv->wide + (size_t)posx], rgb, alpha);
}
static void hline(canvas *cnv, int posy, int minx, int maxx, uint32_t rgb, int alpha)
{
    for (int posx = minx; posx < maxx; posx++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void vline(canvas *cnv, int posx, int starty, int endy, uint32_t rgb, int alpha)
{
    for (int posy = starty; posy < endy; posy++) px_blend(cnv, posx, posy, rgb, alpha);
}
static void fill_rect(canvas *cnv, int posx, int posy, int wide, int high, uint32_t rgb, int alpha)
{
    for (int offy = 0; offy < high; offy++)
        for (int offx = 0; offx < wide; offx++) px_blend(cnv, posx + offx, posy + offy, rgb, alpha);
}
static void frame_rect(canvas *cnv, int posx, int posy, int wide, int high, int thick, uint32_t rgb, int alpha)
{
    for (int edge = 0; edge < thick; edge++) {
        hline(cnv, posy + edge, posx, posx + wide, rgb, alpha);
        hline(cnv, posy + high - 1 - edge, posx, posx + wide, rgb, alpha);
        vline(cnv, posx + edge, posy, posy + high, rgb, alpha);
        vline(cnv, posx + wide - 1 - edge, posy, posy + high, rgb, alpha);
    }
}

/* ---- sprites ------------------------------------------------------------------------------- */

typedef struct {
    int live;                   /* before the end marker */
    int xleft, ytop, size;      /* VDP pixels, size = the box edge */
    int pattern, colour, early;
    int attr;                   /* the Y byte as stored */
} sprite_rec;

static void sprite_geometry(const adamvdp_snapshot *snap, int index, sprite_rec *out)
{
    const uint32_t sat = (uint32_t)(snap->regs[5] & 0x7F) << 7;
    const int size16 = (snap->regs[1] & 2) != 0, mag = (snap->regs[1] & 1) != 0;
    int live = 1;

    for (int item = 0; item < index && live; item++)
        if (snap->vram[(sat + (uint32_t)item * 4) % ADAMVID_VRAM] == 0xD0) live = 0;
    {
        const uint8_t *entry = &snap->vram[(sat + (uint32_t)index * 4) % ADAMVID_VRAM];
        int ypos = entry[0];
        memset(out, 0, sizeof *out);
        if (entry[0] == 0xD0) live = 0;
        out->attr = entry[0];
        if (ypos > 0xE0) ypos -= 256;      /* as the core: above $E0 the sprite bleeds in from the top */
        out->live = live;
        out->ytop = ypos + 1;
        out->early = (entry[3] & 0x80) != 0;
        out->xleft = entry[1] - (out->early ? 32 : 0);
        out->pattern = size16 ? (entry[2] & 0xFC) : entry[2];
        out->colour = entry[3] & 15;
        out->size = (size16 ? 16 : 8) * (mag ? 2 : 1);
    }
}

/* the bit of the sprite's pattern at box offset (offx, offy): 1 lit */
static int sprite_bit(const adamvdp_snapshot *snap, const sprite_rec *spr, int offx, int offy, uint32_t *addr)
{
    const uint32_t spg = (uint32_t)(snap->regs[6] & 0x07) << 11;
    const int size16 = (snap->regs[1] & 2) != 0, mag = (snap->regs[1] & 1) != 0;
    const int stepx = mag ? offx / 2 : offx, stepy = mag ? offy / 2 : offy;
    /* a 16 x 16 sprite is four 8 x 8 patterns: upper left, lower left, upper right, lower right */
    const uint32_t quad = size16 ? (uint32_t)((stepx >> 3) * 2 + (stepy >> 3)) : 0u;
    const uint32_t vaddr = spg + ((uint32_t)spr->pattern + quad) * 8 + (uint32_t)(stepy & 7);
    if (addr) *addr = vaddr;
    return (snap->vram[vaddr % ADAMVID_VRAM] & (0x80 >> (stepx & 7))) != 0;
}

/* The sprites that reach VDP pixel (vdpx, vdpy): front is the one drawn
 * (the lowest-numbered lit one among the first four on its line), mask every
 * box that holds the pixel, hidden the sprites the line has no room for (the
 * fifth and later, which the VDP does not draw). */
static int sprites_at(const adamvid_tab *tab, int vdpx, int vdpy, int *front, uint32_t *mask, uint32_t *hidden, uint32_t *pat_addr)
{
    int any = 0, shown = 0;
    *front = -1; *mask = 0; *hidden = 0; *pat_addr = NO_ADDR;
    if (!has_sprites(tab->snap->regs) || !display_on(tab->snap->regs)) return 0;
    for (int item = 0; item < SPRITES; item++) {
        sprite_rec spr;
        sprite_geometry(tab->snap, item, &spr);
        if (!spr.live) break;
        if (vdpy < spr.ytop || vdpy >= spr.ytop + spr.size) continue;
        if (++shown > 4) {
            if (vdpx >= spr.xleft && vdpx < spr.xleft + spr.size) *hidden |= 1u << item;
            break;
        }
        if (vdpx < spr.xleft || vdpx >= spr.xleft + spr.size) continue;
        *mask |= 1u << item;
        any = 1;
        {
            uint32_t vaddr;
            const int lit = sprite_bit(tab->snap, &spr, vdpx - spr.xleft, vdpy - spr.ytop, &vaddr);
            if (*front < 0 && lit && spr.colour) { *front = item; *pat_addr = vaddr; }
        }
    }
    return any;
}

/* ---- drawing the overlays ------------------------------------------------------------------ */

typedef struct { int orgx, stepx, orgy, stepy; } mapping;     /* picture pixel = origin + VDP coordinate * scale */

static mapping picture_mapping(const adamvid_tab *tab)
{
    mapping map = { 0, 1, 0, 1 };
    if (tab->kind == BK_FRAME)
        map.orgy = ADAMVID_TOP;
    return map;
}

static void draw_overlays(adamvid_tab *tab, canvas *cnv, int scale)
{
    const adamvid_capture *cap = tab->cap;
    const int framed = tab->kind == BK_FRAME;
    const int screened = tab->kind == BK_SCREEN;
    const int valued = tab->kind == BK_VALUES;
    const mapping map = picture_mapping(tab);
    const int text = is_text(&tab->mode);

    if (!framed && !screened && !valued)
        return;
    if (framed && (tab->ovl & ADAMVID_OV_BORDER)) {
        const uint32_t tint = 0xff6040c0;
        const int top = map.orgy * scale;
        const int high = ADAMVID_ACTIVE * scale;
        fill_rect(cnv, 0, 0, ADAMVID_FB_W * scale, top, tint, 70);
        fill_rect(cnv, 0, top + high, ADAMVID_FB_W * scale, ADAMVID_FB_H * scale - top - high, tint, 70);
        if (text) {
            fill_rect(cnv, 0, top, TEXT_XORG * scale, high, tint, 70);
            fill_rect(cnv, (TEXT_XORG + 240) * scale, top, (ADAMVID_FB_W - TEXT_XORG - 240) * scale, high, tint, 70);
        }
        if (!display_on(tab->snap->regs)) fill_rect(cnv, 0, top, ADAMVID_FB_W * scale, high, tint, 90);
    }
    if ((tab->ovl & ADAMVID_OV_HEAT) && !valued && tab->nt_cols > 0 && tab->tracked) {
        const int cell_w = text ? 6 : 8;
        const int xorg = text ? TEXT_XORG : 0;
        for (int cell = 0; cell < tab->nt_cells; cell++)
            if (tab->heat[cell] > 0)
                fill_rect(cnv, (map.orgx + (xorg + (cell % tab->nt_cols) * cell_w) * map.stepx) * scale,
                          (map.orgy + (cell / tab->nt_cols) * 8 * map.stepy) * scale,
                          cell_w * map.stepx * scale, 8 * map.stepy * scale, 0xffff8020, 60 + tab->heat[cell] * 6);
    }
    if ((tab->ovl & ADAMVID_OV_HEAT) && valued && tab->nt_cols > 0 && tab->tracked) {
        for (int cell = 0; cell < tab->nt_cells; cell++)
            if (tab->heat[cell] > 0)
                fill_rect(cnv, (cell % tab->nt_cols) * tab->tile_w * scale, (cell / tab->nt_cols) * 8 * scale,
                          tab->tile_w * scale, 8 * scale, 0xffff8020, 60 + tab->heat[cell] * 6);
    }
    if ((tab->ovl & ADAMVID_OV_GRID) && scale >= 1 && tab->nt_cols > 0) {
        const int cell_w = valued ? tab->tile_w : (text ? 6 : 8);
        const int xorg = (!valued && text) ? TEXT_XORG : 0;
        const int rows = tab->nt_cells / tab->nt_cols;
        const mapping use = valued ? (mapping){ 0, 1, 0, 1 } : map;
        for (int col = 0; col <= tab->nt_cols; col++)
            vline(cnv, (use.orgx + (xorg + col * cell_w) * use.stepx) * scale - (col == tab->nt_cols), use.orgy * scale,
                  (use.orgy + rows * 8 * use.stepy) * scale, 0xffffffff, 60);
        for (int row = 0; row <= rows; row++)
            hline(cnv, (use.orgy + row * 8 * use.stepy) * scale - (row == rows), (use.orgx + xorg * use.stepx) * scale,
                  (use.orgx + (xorg + tab->nt_cols * cell_w) * use.stepx) * scale, 0xffffffff, 60);
    }
    if ((tab->ovl & ADAMVID_OV_SPLITS) && (framed || screened) && tab->nbands > 1) {
        for (int bnd = 1; bnd < tab->nbands; bnd++) {
            const int posy = (map.orgy + tab->bands[bnd].first * map.stepy) * scale;
            hline(cnv, posy, map.orgx * scale, (map.orgx + VDP_WIDE * map.stepx) * scale, 0xff40ff80, 220);
        }
    }
    if ((tab->ovl & ADAMVID_OV_SPRITES) && (framed || screened) && has_sprites(tab->snap->regs) && display_on(tab->snap->regs)) {
        for (int item = 0; item < SPRITES; item++) {
            sprite_rec spr;
            sprite_geometry(tab->snap, item, &spr);
            if (!spr.live) break;
            frame_rect(cnv, (map.orgx + spr.xleft * map.stepx) * scale, (map.orgy + spr.ytop * map.stepy) * scale,
                       spr.size * map.stepx * scale, spr.size * map.stepy * scale, scale < 3 ? 1 : 2,
                       0xff000000u | (uint32_t)(60 + (item * 37) % 196) << 16 | (uint32_t)(255 - (item * 53) % 180) << 8 | (uint32_t)(90 + (item * 71) % 166), 220);
        }
    }
    if ((tab->ovl & ADAMVID_OV_BEAM) && tab->has_live && framed && cap->beam_line >= 0) {
        const int posy = cap->beam_line < ADAMVID_ACTIVE ? cap->beam_line + ADAMVID_TOP : ADAMVID_FB_H - 1;
        if (posy >= 0 && posy < ADAMVID_FB_H) hline(cnv, posy * scale, 0, tab->basew * scale, 0xffff4040, 230);
    }
}

static int pick_zoom(const adamvid_tab *tab)
{
    int scale = tab->zoom;
    if (scale == 0) {
        scale = 1;
        for (size_t item = 1; item < sizeof k_zooms / sizeof *k_zooms; item++)
            if (tab->basew * k_zooms[item] <= tab->vieww && tab->baseh * k_zooms[item] <= tab->viewh)
                scale = k_zooms[item];
    }
    while (scale > 1 && !fits_zoom(tab->basew, tab->baseh, scale)) {
        int lower = 1;
        for (size_t item = 1; item < sizeof k_zooms / sizeof *k_zooms; item++)
            if (k_zooms[item] < scale) lower = k_zooms[item];
        scale = lower;
    }
    return scale;
}

static void build_picture(adamvid_tab *tab)
{
    int scale;
    canvas cnv;

    tab->why[0] = '\0';
    tab->picw = tab->pich = 0;
    if (!build_base(tab))
        return;
    scale = pick_zoom(tab);
    tab->effzoom = scale;
    tab->picw = tab->basew * scale;
    tab->pich = tab->baseh * scale;
    if (!ensure(&tab->pic, &tab->pic_cap, (size_t)tab->picw * (size_t)tab->pich)) {
        tab->picw = tab->pich = 0;
        why_set(tab, "Out of memory.");
        return;
    }
    if (scale == 1)
        memcpy(tab->pic, tab->base, (size_t)tab->basew * (size_t)tab->baseh * sizeof *tab->pic);
    else
        for (int posy = 0; posy < tab->pich; posy++) {
            const uint32_t *src = &tab->base[(size_t)(posy / scale) * (size_t)tab->basew];
            uint32_t *dst = &tab->pic[(size_t)posy * (size_t)tab->picw];
            for (int posx = 0; posx < tab->picw; posx++)
                dst[posx] = src[posx / scale];
        }
    cnv.pixp = tab->pic; cnv.wide = tab->picw; cnv.high = tab->pich;
    draw_overlays(tab, &cnv, scale);
}

/* ---- the inspector ----------------------------------------------------------------------- */

static void add_break(adamvid_pixel *outp, int kind, unsigned lowest, unsigned highest)
{
    if (outp->nbreak < 3) {
        outp->break_kind[outp->nbreak] = kind;
        outp->break_lo[outp->nbreak] = lowest;
        outp->break_hi[outp->nbreak] = highest;
        outp->nbreak++;
    }
}

typedef struct {
    int valid;
    int col, row, cols;
    int name;
    uint32_t name_addr, pattern_addr, tile_lo, tile_hi, colour_addr;
    int forec, backc, lit, colour;
    int cell_x, cell_w;         /* the cell's VDP x and width */
} tile_rec;

/* the tile under VDP pixel (vdpx, vdpy) in the mode of regs */
static void tile_at(const adamvid_tab *tab, const uint8_t *regs, int vdpx, int vdpy, tile_rec *out)
{
    const uint8_t *vram = tab->snap->vram;
    mode_rec mode;
    uint32_t nametab;
    int cols, rows, col, row, name, xin, cell_w, xorg;

    memset(out, 0, sizeof *out);
    out->name = -1;
    out->colour = -1;
    mode_of(regs, &mode);
    nametab = (uint32_t)(regs[2] & 0x0F) << 10;
    cols = mode.kind == MODE_TEXT ? 40 : 32;
    rows = 24;
    cell_w = mode.kind == MODE_TEXT ? 6 : 8;
    xorg = mode.kind == MODE_TEXT ? TEXT_XORG : 0;
    if (vdpx < xorg || vdpx >= xorg + cols * cell_w || vdpy < 0 || vdpy >= rows * 8) return;
    col = (vdpx - xorg) / cell_w;
    row = vdpy / 8;
    xin = (vdpx - xorg) % cell_w;
    name = vram[(nametab + (uint32_t)(row * cols + col)) % ADAMVID_VRAM];
    out->valid = 1;
    out->col = col; out->row = row; out->cols = cols;
    out->name = name;
    out->name_addr = nametab + (uint32_t)(row * cols + col);
    out->cell_x = xorg + col * cell_w; out->cell_w = cell_w;
    {
        const uint32_t patgen7 = (uint32_t)(regs[4] & 0x07) << 11;
        uint8_t bits = 0, colour = 0;
        if (mode.kind == MODE_G2) {
            const uint32_t patgen = (uint32_t)(regs[4] & 0x04) << 11;
            const uint32_t coltab = (uint32_t)(regs[3] & 0x80) << 6;
            const uint32_t pg_mask = ((uint32_t)(regs[4] & 0x03) << 8) | 0xFF;
            const uint32_t ct_mask = ((uint32_t)(regs[3] & 0x7F) << 3) | 0x07;
            const uint32_t bank = (uint32_t)(row / 8) * 256;
            out->tile_lo = patgen + ((bank + (uint32_t)name) & pg_mask) * 8;
            out->tile_hi = out->tile_lo + 7;
            out->pattern_addr = out->tile_lo + (uint32_t)(vdpy & 7);
            out->colour_addr = coltab + (((bank + (uint32_t)name) & ct_mask) * 8) + (uint32_t)(vdpy & 7);
            bits = vram[out->pattern_addr % ADAMVID_VRAM];
            colour = vram[out->colour_addr % ADAMVID_VRAM];
            out->forec = colour >> 4; out->backc = colour & 15;
            out->lit = (bits & (0x80 >> xin)) != 0;
        } else if (mode.kind == MODE_MULTI) {
            out->tile_lo = patgen7 + (uint32_t)name * 8;
            out->tile_hi = out->tile_lo + 7;
            out->pattern_addr = out->tile_lo + (uint32_t)(row & 3) * 2 + (uint32_t)((vdpy & 7) >> 2);
            colour = vram[out->pattern_addr % ADAMVID_VRAM];
            out->colour_addr = out->pattern_addr;
            out->forec = xin < 4 ? colour >> 4 : colour & 15;
            out->backc = out->forec;
            out->lit = 1;
        } else {
            out->tile_lo = patgen7 + (uint32_t)name * 8;
            out->tile_hi = out->tile_lo + 7;
            out->pattern_addr = out->tile_lo + (uint32_t)(vdpy & 7);
            bits = vram[out->pattern_addr % ADAMVID_VRAM];
            if (mode.kind == MODE_TEXT) {
                out->colour_addr = NO_ADDR;
                out->forec = regs[7] >> 4; out->backc = regs[7] & 15;
            } else {
                out->colour_addr = ((uint32_t)regs[3] << 6) + (uint32_t)name / 8;
                colour = vram[out->colour_addr % ADAMVID_VRAM];
                out->forec = colour >> 4; out->backc = colour & 15;
            }
            out->lit = (bits & (0x80 >> xin)) != 0;
        }
        out->colour = out->lit ? out->forec : out->backc;
        if (out->colour == 0) out->colour = backdrop_of(regs);     /* transparent: the backdrop shows */
    }
}

static int text_add(char *dst, size_t size, int num, const char *fmt, ...)
{
    va_list args;
    int add;
    if (num >= (int)size - 1) return num;
    va_start(args, fmt);
    add = vsnprintf(dst + num, size - (size_t)num, fmt, args);
    va_end(args);
    if (add < 0) return num;
    return num + add < (int)size ? num + add : (int)size - 1;
}

/* The VDP pixel (vdpx, vdpy) of the display: what it is made of, appended to
 * the pixel's text, and the break targets. vdpy is the display line. */
static int describe_vdp_pixel(adamvid_tab *tab, int vdpx, int vdpy, adamvid_pixel *outp, int num)
{
    const adamvdp_snapshot *snap = tab->snap;
    char *dst = outp->text;
    const size_t size = sizeof outp->text;
    const band_rec *band;
    int front;
    uint32_t mask, hidden, spr_addr;
    mode_rec mode;

    outp->vdp_x = vdpx; outp->vdp_y = vdpy;
    outp->line = vdpy;
    if (vdpx < 0 || vdpy < 0 || vdpx >= VDP_WIDE || vdpy >= ADAMVID_ACTIVE) {
        outp->kind = ADAMVID_PK_BORDER;
        outp->vdp_x = outp->vdp_y = outp->line = -1;
        num = text_add(dst, size, num, "Border: the backdrop colour %s (%d) from R7. (The border rows are painted at the end of the frame; they are not timed lines.)\n",
                       colour_name(backdrop_of(snap->regs)), backdrop_of(snap->regs));
        outp->colour = backdrop_of(snap->regs);
        add_break(outp, ADAMVID_BK_REGISTER, 7, 7);
        return num;
    }
    band = band_of_line(tab, vdpy);
    mode_of(band->regs, &mode);
    num = text_add(dst, size, num, "VDP pixel %d,%d = display line %d (%s)\n", vdpx, vdpy, vdpy, mode.name);
    if (!display_on(band->regs)) {
        outp->kind = ADAMVID_PK_BORDER;
        outp->colour = backdrop_of(band->regs);
        num = text_add(dst, size, num, "Display blanked (R1 bit 6 clear): the backdrop %s (%d) from R7\n",
                       colour_name(outp->colour), outp->colour);
        add_break(outp, ADAMVID_BK_REGISTER, 1, 1);
        add_break(outp, ADAMVID_BK_REGISTER, 7, 7);
        return num;
    }
    sprites_at(tab, vdpx, vdpy, &front, &mask, &hidden, &spr_addr);
    outp->sprite = front;
    outp->sprite_mask = mask;
    if (hidden) {
        num = text_add(dst, size, num, "Not drawn, the line is full (four sprites a line):");
        for (int item = 0; item < SPRITES; item++)
            if (hidden & (1u << item)) num = text_add(dst, size, num, " %d", item);
        num = text_add(dst, size, num, "\n");
    }
    if (front >= 0) {
        sprite_rec spr;
        const unsigned sat = (unsigned)(snap->regs[5] & 0x7F) << 7;
        const unsigned spg = (unsigned)(snap->regs[6] & 0x07) << 11;
        sprite_geometry(snap, front, &spr);
        outp->kind = ADAMVID_PK_SPRITE;
        outp->colour = spr.colour;
        outp->markx = spr.xleft; outp->marky = spr.ytop; outp->markw = spr.size; outp->markh = spr.size;
        outp->vram_addr = spr_addr;
        num = text_add(dst, size, num, "Sprite %d in front (attribute y=%d x=%d pattern %d colour %s (%d)), pattern byte at $%04X\n",
                       front, spr.attr, spr.xleft + (spr.early ? 32 : 0), spr.pattern, colour_name(spr.colour), spr.colour, spr_addr);
        if (mask & ~(1u << front)) {
            num = text_add(dst, size, num, "Also under the pointer:");
            for (int item = 0; item < SPRITES; item++)
                if ((mask & (1u << item)) && item != front) num = text_add(dst, size, num, " %d", item);
            num = text_add(dst, size, num, "\n");
        }
        add_break(outp, ADAMVID_BK_VRAM, sat + (unsigned)front * 4, sat + (unsigned)front * 4 + 3);
        add_break(outp, ADAMVID_BK_VRAM, spg + (unsigned)spr.pattern * 8,
                  spg + (unsigned)spr.pattern * 8 + ((snap->regs[1] & 2) ? 31u : 7u));
        return num;
    }
    {
        tile_rec tile;
        tile_at(tab, band->regs, vdpx, vdpy, &tile);
        if (tile.valid) {
            outp->kind = ADAMVID_PK_TILE;
            outp->cell = tile.row * tile.cols + tile.col;
            outp->name = tile.name;
            outp->colour = tile.colour;
            outp->vram_addr = tile.name_addr;
            outp->markx = tile.cell_x; outp->marky = tile.row * 8; outp->markw = tile.cell_w; outp->markh = 8;
            num = text_add(dst, size, num, "Name table entry %d (column %d, row %d) at $%04X holds tile %d ($%02X)\n",
                           outp->cell, tile.col, tile.row, tile.name_addr, tile.name, tile.name);
            if (mode.kind == MODE_MULTI)
                num = text_add(dst, size, num, "Colour block %s (%d) from the pattern table byte at $%04X\n",
                               colour_name(tile.forec), tile.forec, tile.pattern_addr);
            else
                num = text_add(dst, size, num, "Pattern row at $%04X (the tile's 8 bytes $%04X-$%04X), pixel %s\n",
                               tile.pattern_addr, tile.tile_lo, tile.tile_hi, tile.lit ? "set" : "clear");
            if (tile.colour_addr != NO_ADDR && mode.kind != MODE_MULTI)
                num = text_add(dst, size, num, "Colours from $%04X: foreground %s (%d), background %s (%d)\n",
                               tile.colour_addr, colour_name(tile.forec), tile.forec, colour_name(tile.backc), tile.backc);
            else if (mode.kind == MODE_TEXT)
                num = text_add(dst, size, num, "Text colours from R7: foreground %s (%d), background %s (%d)\n",
                               colour_name(tile.forec), tile.forec, colour_name(tile.backc), tile.backc);
            num = text_add(dst, size, num, "Colour %s (%d)\n", colour_name(tile.colour), tile.colour);
            add_break(outp, ADAMVID_BK_VRAM, tile.name_addr, tile.name_addr);
            add_break(outp, ADAMVID_BK_VRAM, tile.tile_lo, tile.tile_hi);
            if (tile.colour_addr != NO_ADDR && mode.kind != MODE_MULTI)
                add_break(outp, ADAMVID_BK_VRAM, mode.kind == MODE_G2 ? (tile.colour_addr & ~7u) : tile.colour_addr,
                          mode.kind == MODE_G2 ? (tile.colour_addr & ~7u) + 7 : tile.colour_addr);
            return num;
        }
    }
    outp->kind = ADAMVID_PK_BORDER;
    outp->colour = backdrop_of(band->regs);
    num = text_add(dst, size, num, "Inside the display but outside the text columns: the backdrop %s (%d)\n",
                   colour_name(outp->colour), outp->colour);
    add_break(outp, ADAMVID_BK_REGISTER, 7, 7);
    return num;
}

static void locate(adamvid_tab *tab, int posx, int posy, adamvid_pixel *outp)
{
    const adamvdp_snapshot *snap = tab->snap;
    const int scale = tab->effzoom ? tab->effzoom : 1;
    int picx, picy, num;

    memset(outp, 0, sizeof *outp);
    outp->vdp_x = outp->vdp_y = outp->line = outp->colour = outp->cell = outp->name = outp->sprite = -1;
    outp->vram_addr = NO_ADDR;
    if (!tab->picw || posx < 0 || posy < 0 || posx >= tab->picw || posy >= tab->pich)
        return;
    picx = posx / scale; picy = posy / scale;
    outp->valid = 1;
    outp->picx = picx; outp->picy = picy;
    outp->markx = picx; outp->marky = picy; outp->markw = 1; outp->markh = 1;
    outp->rgb = tab->base ? tab->base[(size_t)picy * (size_t)tab->basew + (size_t)picx] : COL_BLACK;

    num = snprintf(outp->text, sizeof outp->text, "Picture pixel %d,%d\n", picx, picy);
    switch (tab->kind) {
    case BK_FRAME:
    case BK_SCREEN: {
        const mapping map = picture_mapping(tab);
        const int vdpx = picx - map.orgx, vdpy = picy - map.orgy;
        num = describe_vdp_pixel(tab, vdpx, vdpy, outp, num);
        /* the marks were set in VDP pixels; the picture wants its own */
        if (outp->kind == ADAMVID_PK_BORDER || outp->vdp_x < 0 || outp->vdp_y < 0) {
            outp->markx = picx; outp->marky = picy; outp->markw = 1; outp->markh = 1;
        } else {
            outp->markx += map.orgx; outp->marky += map.orgy;
        }
        break;
    }
    case BK_VALUES: {
        uint32_t base;
        int cols, rows;
        name_table(tab, &base, &cols, &rows);
        {
            const int col = picx / tab->tile_w, row = picy / 8;
            const uint32_t addr = base + (uint32_t)(row * cols + col);
            outp->kind = ADAMVID_PK_VALUE;
            outp->cell = row * cols + col;
            outp->vram_addr = addr;
            outp->markx = col * tab->tile_w; outp->marky = row * 8; outp->markw = tab->tile_w; outp->markh = 8;
            outp->name = snap->vram[addr % ADAMVID_VRAM];
            num = text_add(outp->text, sizeof outp->text, num, "Name table entry %d (column %d, row %d) at $%04X = $%02X (%d)\n",
                           outp->cell, col, row, addr, outp->name, outp->name);
            add_break(outp, ADAMVID_BK_VRAM, addr, addr);
        }
        break;
    }
    case BK_PATTERNS: {
        const int tile = (picy / 8) * 32 + picx / 8;
        const int graphic2 = tab->mode.kind == MODE_G2;
        const uint32_t patgen = graphic2 ? (uint32_t)(snap->regs[4] & 0x04) << 11 : (uint32_t)(snap->regs[4] & 0x07) << 11;
        const uint32_t pg_mask = graphic2 ? ((uint32_t)(snap->regs[4] & 0x03) << 8) | 0xFF : 0xFF;
        const uint32_t entry = ((uint32_t)(graphic2 ? tab->bank : 0) * 256 + (uint32_t)tile) & pg_mask;
        const uint32_t addr = patgen + entry * 8;
        outp->kind = ADAMVID_PK_PATTERN;
        outp->name = tile;
        outp->vram_addr = addr;
        outp->markx = (picx / 8) * 8; outp->marky = (picy / 8) * 8; outp->markw = 8; outp->markh = 8;
        num = text_add(outp->text, sizeof outp->text, num, "Tile %d ($%02X)%s: pattern bytes $%04X-$%04X\n", tile, tile,
                       graphic2 ? " (bank shown)" : "", addr, addr + 7);
        add_break(outp, ADAMVID_BK_VRAM, addr, addr + 7);
        break;
    }
    case BK_SPRITES: {
        const int idx = (picy / SPRITE_PITCH) * 8 + picx / SPRITE_PITCH;
        sprite_rec spr;
        outp->kind = ADAMVID_PK_SPRITE;
        outp->sprite = idx;
        outp->markx = (picx / SPRITE_PITCH) * SPRITE_PITCH; outp->marky = (picy / SPRITE_PITCH) * SPRITE_PITCH;
        outp->markw = SPRITE_TILE; outp->markh = SPRITE_TILE;
        if (idx >= 0 && idx < SPRITES) {
            const unsigned sat = (unsigned)(snap->regs[5] & 0x7F) << 7;
            sprite_geometry(snap, idx, &spr);
            num = text_add(outp->text, sizeof outp->text, num, "Sprite %d: y=%d x=%d pattern %d colour %s (%d)%s%s\n", idx,
                           spr.attr, spr.xleft + (spr.early ? 32 : 0), spr.pattern, colour_name(spr.colour), spr.colour,
                           spr.early ? ", early clock" : "", spr.live ? "" : ", past the end marker (not shown)");
            outp->vram_addr = sat + (unsigned)idx * 4;
            add_break(outp, ADAMVID_BK_VRAM, sat + (unsigned)idx * 4, sat + (unsigned)idx * 4 + 3);
        }
        break;
    }
    case BK_PALETTE: {
        const int idx = (picy / ADAMVDP_PAL_CELL) * ADAMVDP_PAL_COLS + picx / ADAMVDP_PAL_CELL;
        const uint32_t rgb = rgb_of_565(snap->palette565[idx & 15]);
        outp->kind = ADAMVID_PK_PALETTE;
        outp->colour = idx & 15;
        outp->markx = (picx / ADAMVDP_PAL_CELL) * ADAMVDP_PAL_CELL; outp->marky = (picy / ADAMVDP_PAL_CELL) * ADAMVDP_PAL_CELL;
        outp->markw = ADAMVDP_PAL_CELL; outp->markh = ADAMVDP_PAL_CELL;
        num = text_add(outp->text, sizeof outp->text, num, "Colour %s (%d): R %d, G %d, B %d (fixed by the TMS9918A)\n", colour_name(idx), idx,
                       (int)((rgb >> 16) & 255), (int)((rgb >> 8) & 255), (int)(rgb & 255));
        break;
    }
    default:
        break;
    }
    (void)num;
    if (outp->colour >= 0 && outp->kind != ADAMVID_PK_VALUE && tab->kind != BK_FRAME && tab->kind != BK_SCREEN)
        outp->rgb = rgb_of_565(snap->palette565[outp->colour & 15]);
}

int adamvid_tab_hover(adamvid_tab *tab, int posx, int posy)
{
    adamvid_pixel before;
    if (!tab) return 0;
    before = tab->hover;
    tab->hovx = posx; tab->hovy = posy;
    locate(tab, posx, posy, &tab->hover);
    return tab->hover.valid != before.valid || tab->hover.picx != before.picx || tab->hover.picy != before.picy ||
           strcmp(tab->hover.text, before.text) != 0;
}

void adamvid_tab_pin(adamvid_tab *tab, int posx, int posy)
{
    adamvid_pixel probe;
    if (!tab) return;
    locate(tab, posx, posy, &probe);
    if (tab->pin.valid && probe.valid && tab->pin.markx == probe.markx && tab->pin.marky == probe.marky &&
        tab->pin.markw == probe.markw && tab->pin.markh == probe.markh) {
        adamvid_tab_unpin(tab);
        return;
    }
    tab->pinx = posx; tab->piny = posy;
    tab->pin = probe;
    if (probe.valid && probe.vram_addr != NO_ADDR) tab->vram_base = (probe.vram_addr & (ADAMVID_VRAM - 1)) & ~15u;
}

void adamvid_tab_unpin(adamvid_tab *tab)
{
    if (!tab) return;
    tab->pinx = tab->piny = -1;
    memset(&tab->pin, 0, sizeof tab->pin);
}

const adamvid_pixel *adamvid_tab_pixel(const adamvid_tab *tab)
{
    if (!tab) return NULL;
    if (tab->pin.valid) return &tab->pin;
    if (tab->hover.valid) return &tab->hover;
    return NULL;
}

int adamvid_tab_marks(const adamvid_tab *tab, int hover[4], int pin[4])
{
    const int scale = tab && tab->effzoom ? tab->effzoom : 1;
    int ret = 0;
    if (!tab) return 0;
    if (tab->hover.valid && hover) {
        hover[0] = tab->hover.markx * scale; hover[1] = tab->hover.marky * scale;
        hover[2] = tab->hover.markw * scale; hover[3] = tab->hover.markh * scale;
        ret |= 1;
    }
    if (tab->pin.valid && pin) {
        pin[0] = tab->pin.markx * scale; pin[1] = tab->pin.marky * scale;
        pin[2] = tab->pin.markw * scale; pin[3] = tab->pin.markh * scale;
        ret |= 2;
    }
    return ret;
}

/* ---- Break on writes ------------------------------------------------------------------------- */

int adamvid_tab_can_break(const adamvid_tab *tab)
{
    const adamvid_pixel *pixp = adamvid_tab_pixel(tab);
    return tab && tab->dbg && pixp && pixp->nbreak > 0;
}

/* The VDP is not on the bus the engine's hook sees, so a write to it is an OUT
 * breakpoint on the VDP ports with a condition over the VDP's own state, which
 * the engine reads inside the hook (adamcore_vdp_state, adamcore_vdp_latch_state):
 *   vdpaddr    the VRAM address counter, before the write lands
 *   vdpsecond  1 when a control-port write is the second byte of a pair
 * plus the ordinary names addr (the port) and value (the byte written). An
 * identical breakpoint already in the table is found and reused. */
static int break_out(adamvid_tab *tab, unsigned port_lo, unsigned port_hi, const char *cond)
{
    adamdebug_breakpoint list[ADAMVID_MAX_BREAKPOINTS];
    char err[128];
    const int count = adamdebug_breakpoint_list(tab->dbg, list, ADAMVID_MAX_BREAKPOINTS);

    for (int item = 0; item < count && item < ADAMVID_MAX_BREAKPOINTS; item++)
        if (list[item].type == ADAMDEBUG_BP_OUT && list[item].start == port_lo && list[item].end == port_hi &&
            strcmp(list[item].condition, cond) == 0)
            return list[item].id;
    return adamdebug_breakpoint_add(tab->dbg, ADAMDEBUG_BP_OUT, (uint16_t)port_lo, (uint16_t)port_hi, cond,
                                      err, (int)sizeof err);
}

/* The conditions Break on writes hands the engine, for the pixel's targets. */
static void break_condition(int kind, unsigned lowest, unsigned highest, char *cond, size_t size, unsigned *port_lo, unsigned *port_hi)
{
    *port_lo = 0xA0;
    *port_hi = 0xBF;
    if (kind == ADAMVID_BK_REGISTER) {
        /* the second byte of a control-port pair, $80 + the register */
        snprintf(cond, size, "(addr&1)==1&&vdpsecond&&value==0x%02X", 0x80u + lowest);
    } else if (lowest == highest) {
        snprintf(cond, size, "(addr&1)==0&&vdpaddr==0x%04X", lowest);
    } else {
        snprintf(cond, size, "(addr&1)==0&&vdpaddr>=0x%04X&&vdpaddr<=0x%04X", lowest, highest);
    }
}

int adamvid_tab_break_on_pixel(adamvid_tab *tab)
{
    const adamvid_pixel *pixp = adamvid_tab_pixel(tab);
    int first = -1;
    if (!tab || !tab->dbg || !pixp || pixp->nbreak == 0) return -1;
    for (int item = 0; item < pixp->nbreak; item++) {
        char cond[160];
        unsigned port_lo, port_hi;
        int bpid;
        break_condition(pixp->break_kind[item], pixp->break_lo[item], pixp->break_hi[item], cond, sizeof cond, &port_lo, &port_hi);
        bpid = break_out(tab, port_lo, port_hi, cond);
        if (first < 0) first = bpid;
    }
    return first;
}

/* ---- the tables ---------------------------------------------------------------------------- */

static void fill_status(adamvid_tab *tab)
{
    const adamvid_capture *cap = tab->cap;
    const adamvdp_snapshot *snap = tab->snap;
    int num = 0;

    num = snprintf(tab->status, sizeof tab->status, "%s, %d lines, NTSC %d Hz, display %s",
                   tab->mode.name, ADAMVID_ACTIVE, 60, display_on(snap->regs) ? "on" : "BLANKED");
    if (has_sprites(snap->regs))
        num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, ", sprites %s%s",
                        (snap->regs[1] & 2) ? "16x16" : "8x8", (snap->regs[1] & 1) ? " magnified" : "");
    else
        num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, ", no sprites");
    num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, "\n");
    if (tab->has_live) {
        num += snprintf(tab->status + num, sizeof tab->status - (size_t)num, "Stopped; %llu frames complete",
                        (unsigned long long)cap->frame);
        if (cap->beam_line >= 0)
            snprintf(tab->status + num, sizeof tab->status - (size_t)num, "; beam at line %d of %d, T-state %d of %d",
                     cap->beam_line, ADAMVID_LINES, cap->beam_tstate, LINE_TSTATES);
    } else {
        snprintf(tab->status + num, sizeof tab->status - (size_t)num, "Running; frame %llu, %s",
                 (unsigned long long)cap->frame, cap->log_valid ? "the line log is recorded" : "waiting for a frame's line log");
    }
}

const char *adamvid_tab_status(const adamvid_tab *tab) { return tab ? tab->status : ""; }

static void reg_entry(adamvid_tab *tab, int index, const char *meaning)
{
    adamvid_reg *reg = &tab->regs[tab->nregs];
    char *buf = tab->reg_names[tab->nregs];

    if (index == STATUS_SLOT) snprintf(buf, sizeof tab->reg_names[0], "ST");
    else snprintf(buf, sizeof tab->reg_names[0], "R%d", index);
    reg->name = buf;
    snprintf(reg->value, sizeof reg->value, "%02X", index == STATUS_SLOT ? tab->cap->status : tab->cap->regs[index]);
    reg->age = tab->reg_age[index];
    reg->writes = tab->reg_writes[index];
    reg->first_line = tab->reg_first[index];
    snprintf(reg->meaning, sizeof reg->meaning, "%s", meaning);
    tab->nregs++;
}

static void fill_regs(adamvid_tab *tab)
{
    char detail[160], text[200];

    tab->nregs = 0;
    for (int reg = 0; reg < 8; reg++) {
        detail[0] = '\0';
        adamvdp_describe_register(tab->snap, reg, detail, (int)sizeof detail);
        snprintf(text, sizeof text, "%s: %s", adamvdp_register_name(reg), detail);
        reg_entry(tab, reg, text);
    }
    detail[0] = '\0';
    adamvdp_describe_status(tab->snap, detail, (int)sizeof detail);
    snprintf(text, sizeof text, "Status (read-only): %s", detail);
    reg_entry(tab, STATUS_SLOT, text);
}

int adamvid_tab_regs(const adamvid_tab *tab, const adamvid_reg **out)
{
    if (!tab) return 0;
    if (out) *out = tab->regs;
    return tab->nregs;
}

static void fill_swatches(adamvid_tab *tab)
{
    const adamvid_capture *cap = tab->cap;
    const adamvid_pixel *pixp = adamvid_tab_pixel(tab);
    const uint32_t *frame = cap->fb_valid ? cap->pixels : cap->live_pixels;
    const int have_frame = cap->fb_valid || cap->live_pixels_valid;
    uint32_t rgb[16];
    long uses[16];

    memset(uses, 0, sizeof uses);
    for (int idx = 0; idx < 16; idx++) rgb[idx] = rgb_of_565(tab->snap->palette565[idx]);
    if (have_frame)
        for (size_t item = 0; item < (size_t)ADAMVID_FB_W * ADAMVID_FB_H; item++) {
            const uint32_t pix = 0xff000000u | frame[item];
            for (int idx = 1; idx < 16; idx++)
                if (pix == rgb[idx]) { uses[idx]++; break; }
        }
    for (int idx = 0; idx < 16; idx++) {
        adamvid_swatch *swat = &tab->swat[idx];
        snprintf(tab->swat_names[idx], sizeof tab->swat_names[0], "%s (%d)", colour_name(idx), idx);
        swat->index = idx;
        swat->name = tab->swat_names[idx];
        swat->rgb = rgb[idx];
        swat->uses = uses[idx];
        swat->role[0] = '\0';
        swat->inspected = pixp && pixp->colour == idx;
    }
    {
        const int back = backdrop_of(tab->snap->regs);
        if (is_text(&tab->mode)) {
            snprintf(tab->swat[tab->snap->regs[7] >> 4].role, sizeof tab->swat[0].role, "text foreground (R7)");
            if (back != tab->snap->regs[7] >> 4)
                snprintf(tab->swat[back].role, sizeof tab->swat[0].role, "backdrop, text background (R7)");
        } else {
            snprintf(tab->swat[back].role, sizeof tab->swat[0].role, "backdrop (R7)");
        }
        if (tab->swat[0].role[0] == '\0')
            snprintf(tab->swat[0].role, sizeof tab->swat[0].role, "transparent (shows the backdrop)");
    }
}

int adamvid_tab_swatches(const adamvid_tab *tab, const adamvid_swatch **out)
{
    if (!tab) return 0;
    if (out) *out = tab->swat;
    return 16;
}

static void fill_lines(adamvid_tab *tab)
{
    tab->nlines = 0;
    for (int bnd = 0; bnd < tab->nbands; bnd++) {
        const band_rec *band = &tab->bands[bnd];
        adamvid_band *line = &tab->lines[tab->nlines++];
        mode_rec mode;
        char tables[120];
        memset(line, 0, sizeof *line);
        line->first = band->first;
        line->last = band->last;
        line->abs_first = band->first;
        line->writes = band->writes;
        line->backdrop = backdrop_of(band->regs);
        mode_of(band->regs, &mode);
        snprintf(line->mode, sizeof line->mode, "%s", mode.kind == MODE_TEXT ? "Text" : mode.kind == MODE_MULTI ? "Multicolor"
                                                    : mode.kind == MODE_G2 ? "Graphics II" : "Graphics I");
        line->name_table = (unsigned)(band->regs[2] & 0x0F) << 10;
        line->sprite_attr = (unsigned)(band->regs[5] & 0x7F) << 7;
        line->sprite_pattern = (unsigned)(band->regs[6] & 0x07) << 11;
        if (mode.kind == MODE_G2) {
            line->pattern_table = (unsigned)(band->regs[4] & 0x04) << 11;
            line->colour_table = (unsigned)(band->regs[3] & 0x80) << 6;
        } else {
            line->pattern_table = (unsigned)(band->regs[4] & 0x07) << 11;
            line->colour_table = mode.kind == MODE_G1 ? (unsigned)band->regs[3] << 6 : 0;
        }
        if (mode.kind == MODE_TEXT)
            snprintf(tables, sizeof tables, "names $%04X, patterns $%04X, no sprites", line->name_table, line->pattern_table);
        else if (mode.kind == MODE_MULTI)
            snprintf(tables, sizeof tables, "names $%04X, colour blocks $%04X, sprites $%04X/$%04X",
                     line->name_table, line->pattern_table, line->sprite_attr, line->sprite_pattern);
        else
            snprintf(tables, sizeof tables, "names $%04X, patterns $%04X, colours $%04X, sprites $%04X/$%04X",
                     line->name_table, line->pattern_table, line->colour_table, line->sprite_attr, line->sprite_pattern);
        {
            char changed[40] = "", mode_name[24];
            snprintf(mode_name, sizeof mode_name, "%s", line->mode);
            if (band->writes) snprintf(changed, sizeof changed, " [%d register%s changed here]", band->writes, band->writes == 1 ? "" : "s");
            snprintf(line->text, sizeof line->text, "lines %d-%d: %s%s, %s, backdrop %d%s",
                     band->first, band->last, mode_name, display_on(band->regs) ? "" : " (display blanked)", tables,
                     line->backdrop, changed);
        }
    }
}

int adamvid_tab_lines(const adamvid_tab *tab, const adamvid_band **out)
{
    if (!tab) return 0;
    if (out) *out = tab->lines;
    return tab->nlines;
}

static void timing_add(adamvid_tab *tab, const char *label, const char *fmt, ...)
{
    char value[96];
    va_list args;
    if (tab->ntiming >= MAX_TIMING) return;
    va_start(args, fmt);
    vsnprintf(value, sizeof value, fmt, args);
    va_end(args);
    snprintf(tab->timing_buf[tab->ntiming], sizeof tab->timing_buf[0], "%s\t%s", label, value);
    tab->timing_ptr[tab->ntiming] = tab->timing_buf[tab->ntiming];
    tab->ntiming++;
}

static void fill_timing(adamvid_tab *tab)
{
    const adamvid_capture *cap = tab->cap;

    tab->ntiming = 0;
    timing_add(tab, "Standard", "NTSC, 59.92 Hz, %d lines a frame", ADAMVID_LINES);
    timing_add(tab, "Line", "%d Z80 clocks at %.6f MHz = %.1f us", LINE_TSTATES, CPU_HZ / 1e6, LINE_TSTATES * 1e6 / CPU_HZ);
    timing_add(tab, "Frame", "%d Z80 clocks = %.2f ms", ADAMVID_LINES * LINE_TSTATES, ADAMVID_LINES * LINE_TSTATES * 1e3 / CPU_HZ);
    timing_add(tab, "Display", "lines 0-%d of the frame (R1 display %s); the picture adds %d border rows above and below",
               ADAMVID_ACTIVE - 1, display_on(tab->snap->regs) ? "on" : "BLANKED", ADAMVID_TOP);
    timing_add(tab, "Vertical blank", "status F is set and the NMI is taken before line %d (R1 IE %s)", ADAMVID_ACTIVE,
               (tab->snap->regs[1] & 0x20) ? "on: the NMI will fire" : "off: no NMI");
    timing_add(tab, "Line log", "%s%s", cap->log_valid ? "frame " : "none yet",
               cap->log_valid ? (cap->log_frame < 1000000000ull ? "recorded line by line" : "recorded") : "");
    if (cap->log_valid)
        timing_add(tab, "Logged frame", "%llu (a register write shows from the line it lands on)", (unsigned long long)cap->log_frame);
    if (tab->has_live) {
        int done = 0;
        for (int line = 0; line < ADAMVID_LINES; line++)
            if (cap->cur[line].valid) done++;
        timing_add(tab, "Frame in progress", "%d lines recorded so far", done);
        timing_add(tab, "NMIs", "%llu VBlank NMIs since power-on", (unsigned long long)cap->nmi_count);
        timing_add(tab, "Cycles", "%llu Z80 clocks since power-on", (unsigned long long)cap->cycles);
        if (cap->beam_line >= 0)
            timing_add(tab, "Beam", "line %d (%s), T-state %d of %d", cap->beam_line,
                       cap->beam_line < ADAMVID_ACTIVE ? "display" : "blanking", cap->beam_tstate, LINE_TSTATES);
        else
            timing_add(tab, "Beam", "unknown");
    } else {
        timing_add(tab, "Beam", "(stopped only)");
    }
}

int adamvid_tab_timing(const adamvid_tab *tab, const char *const **lines)
{
    if (!tab) return 0;
    if (lines) *lines = tab->timing_ptr;
    return tab->ntiming;
}

int adamvid_tab_beam_permille(const adamvid_tab *tab)
{
    int per;
    if (!tab || !tab->has_live || tab->cap->beam_line < 0) return 0;
    per = (int)((long)tab->cap->beam_line * 1000 / ADAMVID_LINES);
    return per > 1000 ? 1000 : per < 0 ? 0 : per;
}

/* ---- refresh --------------------------------------------------------------------------------- */

unsigned adamvid_tab_refresh(adamvid_tab *tab, int force)
{
    unsigned flags = 0;
    uint64_t sig, tsig;
    if (!tab) return 0;
    if (!acquire(tab)) {
        tab->picw = tab->pich = 0;
        why_set(tab, "No machine is running, or no frame has been captured yet.");
        tab->status[0] = '\0';
        tab->sig = 0;
        return ADAMVID_TAB_OFF | ADAMVID_TAB_PICTURE | ADAMVID_TAB_TABLES;
    }
    sig = picture_signature(tab);
    tsig = table_signature(tab, sig);
    if (force || tab->dirty || sig != tab->sig) {
        track(tab);
        tab->sig = sig;
        tab->tsig = tsig;
        tab->dirty = 0;
        build_picture(tab);
        fill_status(tab);
        fill_regs(tab);
        fill_swatches(tab);
        fill_lines(tab);
        fill_timing(tab);
        if (tab->hovx >= 0) locate(tab, tab->hovx, tab->hovy, &tab->hover);
        if (tab->pinx >= 0) locate(tab, tab->pinx, tab->piny, &tab->pin);
        flags |= ADAMVID_TAB_PICTURE | ADAMVID_TAB_TABLES;
        if (!tab->picw) flags |= ADAMVID_TAB_OFF;
    } else if (tsig != tab->tsig) {
        tab->tsig = tsig;
        fill_status(tab);
        fill_timing(tab);
        flags |= ADAMVID_TAB_TABLES;
    }
    return flags;
}

const uint32_t *adamvid_tab_picture(adamvid_tab *tab, int *wide, int *high)
{
    if (!tab || !tab->picw) return NULL;
    if (wide) *wide = tab->picw;
    if (high) *high = tab->pich;
    return tab->pic;
}

const char *adamvid_tab_why(const adamvid_tab *tab) { return tab ? tab->why : ""; }
int adamvid_tab_effective_zoom(const adamvid_tab *tab) { return tab ? tab->effzoom : 1; }

/* ---- text -------------------------------------------------------------------------------------- */

int adamvid_tab_text(adamvid_tab *tab, int section, char *dst, int dstsz)
{
    int num = 0;
    const size_t size = (size_t)dstsz;
    if (!tab || !dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    switch (section) {
    case ADAMVID_S_REGISTERS:
        num = text_add(dst, size, num, "%s\n", tab->status);
        for (int item = 0; item < tab->nregs; item++) {
            const adamvid_reg *reg = &tab->regs[item];
            num = text_add(dst, size, num, "%-4s %-4s %s", reg->name, reg->value, reg->meaning);
            if (reg->writes) num = text_add(dst, size, num, "  [changed %d time%s down the frame, first at line %d]", reg->writes, reg->writes == 1 ? "" : "s", reg->first_line);
            else if (reg->age < ADAMVID_REG_FADE) num = text_add(dst, size, num, "  [changed %d refresh%s ago]", reg->age, reg->age == 1 ? "" : "es");
            num = text_add(dst, size, num, "\n");
        }
        break;
    case ADAMVID_S_COLOURS:
        for (int item = 0; item < 16; item++) {
            const adamvid_swatch *swat = &tab->swat[item];
            num = text_add(dst, size, num, "%2d %-20s %06X  %ld px%s%s\n", item, swat->name, swat->rgb & 0xFFFFFFu, swat->uses,
                           swat->role[0] ? "  " : "", swat->role);
        }
        break;
    case ADAMVID_S_LINES:
        for (int item = 0; item < tab->nlines; item++) num = text_add(dst, size, num, "%s\n", tab->lines[item].text);
        break;
    case ADAMVID_S_TIMING:
        for (int item = 0; item < tab->ntiming; item++) {
            const char *label = tab->timing_ptr[item], *value = strchr(label, '\t');
            if (value) num = text_add(dst, size, num, "%.*s: %s\n", (int)(value - label), label, value + 1);
            else num = text_add(dst, size, num, "%s\n", label);
        }
        break;
    case ADAMVID_S_VRAM: {
        char hex[2048];
        adamvdp_format_hex(tab->snap, (uint16_t)tab->vram_base, 16, hex, (int)sizeof hex);
        num = text_add(dst, size, num, "%s", hex);
        break;
    }
    default: {
        const adamvid_pixel *pixp = adamvid_tab_pixel(tab);
        if (pixp) num = text_add(dst, size, num, "%s", pixp->text);
        else num = text_add(dst, size, num, "No pixel under the pointer.\n");
        break;
    }
    }
    return num;
}

static void nap_ms(long millis)
{
    struct timespec nap;
    nap.tv_sec = millis / 1000;
    nap.tv_nsec = (millis % 1000) * 1000000L;
    nanosleep(&nap, NULL);
}

int adamvid_tab_text_for(adamdebug *dbg, const char *section, char *dst, int dstsz)
{
    adamvid_tab *tab;
    int sec = -1, num = 0, waited;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (section && *section) {
        sec = adamvid_section_from_name(section);
        if (sec < 0) return snprintf(dst, (size_t)dstsz, "unknown section \"%s\" (pixel, registers, colours, lines, timing, vram)\n", section);
    }
    tab = adamvid_tab_new(dbg);
    if (!tab) return snprintf(dst, (size_t)dstsz, "out of memory\n");
    /* A running machine publishes a frame within a moment, and records the
     * line log from the frame after that; a parked one is read at once. */
    for (waited = 0; waited < 600; waited += 20) {
        const unsigned flags = adamvid_tab_refresh(tab, 1);
        if (!(flags & ADAMVID_TAB_OFF) && (tab->has_live || tab->cap->log_valid || waited >= 200))
            break;
        nap_ms(20);
    }
    if (!tab->have || !tab->picw) {
        /* tables can exist without a picture (a view that cannot be drawn) */
        if (!tab->have)
            num = snprintf(dst, (size_t)dstsz, "No machine is running, or no frame has been captured yet.\n");
    }
    if (tab->have) {
        /* the status is the first line of the registers text */
        num = adamvid_tab_text(tab, sec < 0 ? ADAMVID_S_REGISTERS : sec, dst, dstsz);
    }
    adamvid_tab_free(tab);
    return num;
}

/* ---- the console's `video` ---------------------------------------------------------------------- */

static adamdebug *s_console_dbg;
static char s_console_text[8192];

static const char *console_text(const char *section)
{
    if (adamvid_tab_text_for(s_console_dbg, section, s_console_text, (int)sizeof s_console_text) <= 0)
        return NULL;
    return s_console_text;
}

void adamvid_tab_register_console(adamdebug *dbg)
{
    s_console_dbg = dbg;
    adamdebug_set_video_text(dbg ? console_text : NULL);
}

/* ---- poke --------------------------------------------------------------------------------------- */

int adamvid_tab_poke(adamvid_tab *tab, const char *line, char *err, int errsz)
{
    uint8_t bytes[256];
    uint16_t addr = 0;
    int count;

    if (err && errsz > 0) err[0] = '\0';
    if (!tab || !line) return -1;
    count = adamvdp_parse_poke(line, &addr, bytes, (int)sizeof bytes);
    if (count < 0) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "Not a poke line. Use an address, then optionally \":\" and hex bytes (1800: 41 42).");
        return -1;
    }
    if (count == 0) {
        tab->vram_base = addr & ~15u;
        return 0;
    }
    if (!tab->dbg) {
        if (err && errsz > 0) snprintf(err, (size_t)errsz, "No debugger to write through.");
        return -1;
    }
    adamdebug_vdp_write(tab->dbg, addr, bytes, count);
    tab->vram_base = addr & ~15u;
    tab->refill = 1;
    tab->dirty = 1;
    return count;
}
