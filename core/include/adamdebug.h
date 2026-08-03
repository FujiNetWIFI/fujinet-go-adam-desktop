/*
 * adamdebug -- the toolkit-agnostic debugger engine for FujiNet Go Adam
 * Desktop. Wraps adamcore's debug interface (adamcore_debug.h) with
 * pause/step/run-to execution control, breakpoint management, an
 * instruction-history ring, a Z80 disassembler, EOS/OS7 symbol tables, and
 * VDP visualizer decoders. One engine exists per session
 * (adamsession_debugger()); native GTK/Qt views sit on top.
 *
 * Threading: control calls (pause/resume/step/breakpoints) are safe from
 * the UI thread. The stop callback fires on the EMULATOR thread -- marshal
 * to the UI (g_idle_add / QMetaObject::invokeMethod). State reads are
 * consistent while paused.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAMDEBUG_H
#define ADAMDEBUG_H

#include <stdint.h>

#include "adamcore_debug.h"
#include "adamsession.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ADAMDBG_STOP_PAUSE = 0,   /* explicit pause request */
    ADAMDBG_STOP_BREAKPOINT,  /* PC breakpoint hit (before execution) */
    ADAMDBG_STOP_STEP,        /* step into/over/out completed */
    ADAMDBG_STOP_RUNTO,       /* run-to-address reached */
} adamdebug_stop_reason;

/* ---- execution control --------------------------------------------------- */
void adamdebug_pause(adamdebug *d);   /* halts at the next instruction */
void adamdebug_resume(adamdebug *d);
int  adamdebug_is_paused(adamdebug *d);
void adamdebug_step_into(adamdebug *d);           /* while paused */
void adamdebug_step_over(adamdebug *d);           /* while paused */
void adamdebug_step_out(adamdebug *d);            /* while paused */
void adamdebug_run_to(adamdebug *d, uint16_t addr);

/* Fires on the emulator thread whenever execution stops (and also after
 * each completed step). */
void adamdebug_set_stop_callback(adamdebug *d,
                                 void (*cb)(void *ud,
                                            adamdebug_stop_reason reason,
                                            uint16_t pc),
                                 void *ud);

/* ---- breakpoints ---------------------------------------------------------- */
void adamdebug_bp_toggle(adamdebug *d, uint16_t addr);
void adamdebug_bp_set(adamdebug *d, uint16_t addr);
void adamdebug_bp_clear(adamdebug *d, uint16_t addr);
void adamdebug_bp_clear_all(adamdebug *d);
int  adamdebug_bp_is_set(adamdebug *d, uint16_t addr);
int  adamdebug_bp_list(adamdebug *d, uint16_t *out, int max);

/* ---- CPU / memory (consistent while paused) ------------------------------- */
void adamdebug_get_regs(adamdebug *d, adamcore_z80_regs *out);
void adamdebug_set_regs(adamdebug *d, const adamcore_z80_regs *in);
int  adamdebug_read_mem(adamdebug *d, uint16_t addr, uint8_t *dst, int n);
int  adamdebug_write_mem(adamdebug *d, uint16_t addr, const uint8_t *src,
                         int n);

/* ---- disassembly ---------------------------------------------------------- */
#define ADAMDASM_JUMP     0x01
#define ADAMDASM_CALL     0x02
#define ADAMDASM_RET      0x04
#define ADAMDASM_COND     0x08
#define ADAMDASM_RELATIVE 0x10
#define ADAMDASM_BLOCK    0x20 /* LDIR/CPIR/INIR/OTIR family */
#define ADAMDASM_HALT     0x40

typedef struct {
    uint16_t addr;
    uint8_t len;      /* 1..4 */
    uint8_t bytes[4];
    char text[32];    /* "LD (IX+5),A" */
    uint16_t target;  /* jump/call destination when flags say so */
    uint8_t flags;    /* ADAMDASM_* */
    const char *symbol; /* label at addr from the symbol tables, or NULL */
} adamdasm_line;

/* Disassembles count instructions starting at addr (reading through the
 * current memory map, side-effect free). Returns lines written. */
int adamdebug_disassemble(adamdebug *d, uint16_t addr, int count,
                          adamdasm_line *out);

/* ---- instruction history (trace ring) ------------------------------------- */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl;
    uint8_t len;
    uint8_t bytes[4];
    uint64_t cycles;
} adamtrace_entry;

void adamdebug_trace_enable(adamdebug *d, int enable);
int  adamdebug_trace_enabled(adamdebug *d);
/* Copies up to max entries, newest first. Returns entries written. */
int  adamdebug_trace_read(adamdebug *d, adamtrace_entry *out, int max);
void adamdebug_trace_clear(adamdebug *d);

/* ---- symbols --------------------------------------------------------------
 * .sym format: "HHHH NAME [; comment]" per line, '#' comments. The built-in
 * EOS and OS7 tables (generated from the Drushel EOS-5 disassembly, the
 * eoslib jump-table names, and the os7lib listing) load automatically when
 * the engine is created. */
int adamdebug_symbols_load(adamdebug *d, const char *path, const char *table);
const char *adamdebug_symbol_at(adamdebug *d, uint16_t addr,
                                uint16_t *offset);
int adamdebug_symbol_find(adamdebug *d, const char *name, uint16_t *addr);

/* ---- VDP ------------------------------------------------------------------ */
typedef struct {
    uint8_t vram[0x4000];
    uint8_t regs[8];
    uint8_t status;
    uint16_t addr;
    uint16_t palette565[16];
} adamvdp_snapshot;

void adamdebug_vdp_snapshot(adamdebug *d, adamvdp_snapshot *out);

/* Live VRAM access, addresses wrapping at 16K. Reads are side-effect free;
 * writes go straight into the VDP's RAM array without touching the address
 * register or the read-ahead latch, so poking VRAM cannot desynchronize a
 * transfer the running program is in the middle of. Both are safe from the
 * UI thread (a write racing the beam just shows up on the next line, the
 * same as one from the CPU). */
int adamdebug_vdp_read(adamdebug *d, uint16_t addr, uint8_t *dst, int n);
int adamdebug_vdp_write(adamdebug *d, uint16_t addr, const uint8_t *src,
                        int n);

/* Parses a "poke line": a hex address, then optionally a ':' or '=' and any
 * number of whitespace/comma-separated hex bytes ("1800", "$1800: 41 42",
 * "1800 = 4142FF"). Tokens longer than two digits are taken as a run of
 * bytes so a pasted hex dump works. Returns the byte count (0 when only an
 * address was given, i.e. "go to"), or -1 if the text does not parse. */
int adamvdp_parse_poke(const char *text, uint16_t *addr, uint8_t *bytes,
                       int max);

/* ---- VDP tables / registers ----------------------------------------------- */

typedef enum {
    ADAMVDP_MODE_GRAPHICS1 = 0,
    ADAMVDP_MODE_GRAPHICS2,
    ADAMVDP_MODE_MULTICOLOR,
    ADAMVDP_MODE_TEXT,
    ADAMVDP_MODE_INVALID, /* an M1/M2/M3 combination the VDP does not define */
} adamvdp_mode;

typedef struct {
    uint16_t base;  /* VRAM address the mode fetches this table from */
    uint16_t size;  /* bytes the mode actually fetches (0 = mode unused) */
    uint16_t mask;  /* Graphics II AND-mask over the table index (else 0) */
} adamvdp_table;

typedef struct {
    adamvdp_mode mode;
    const char *mode_name;
    adamvdp_table name, color, pattern, sprite_attr, sprite_pattern;
    int display_on;        /* R1 BLANK */
    int irq_on;            /* R1 IE */
    int vram_16k;          /* R1 M/S: 16K vs 4K RAM */
    int ext_video;         /* R0 EXTVID */
    int sprites_16x16;     /* R1 SIZE */
    int sprites_magnified; /* R1 MAG */
    uint8_t backdrop;      /* R7 low nibble */
    uint8_t text_fg;       /* R7 high nibble (text mode only) */
} adamvdp_tables;

/* Decodes the register set into the addresses the beam is actually using.
 * Tables a mode does not fetch come back with size 0. */
void adamvdp_get_tables(const adamvdp_snapshot *s, adamvdp_tables *out);

/* "Name table", "Sprite attributes", ... for R0-R7. */
const char *adamvdp_register_name(int reg);

/* One line of decoded detail for register reg (0-7): the bit fields by
 * name for R0/R1/R7, the resolved address plus extent (and Graphics II
 * index mask) for the table registers. Returns the length written. */
int adamvdp_describe_register(const adamvdp_snapshot *s, int reg, char *out,
                              int max);

/* Same for the read-only status register: interrupt flag, 5th-sprite and
 * coincidence flags, and the reported sprite number. */
int adamvdp_describe_status(const adamvdp_snapshot *s, char *out, int max);

/* TMS9918A color names, "Transparent" through "White". */
const char *adamvdp_color_name(int index);

/* The whole decode as monospace text -- current mode, the table addresses,
 * then one decoded line per register plus status. Every frontend shows the
 * identical block; 2K is comfortably enough. Returns the length written. */
int adamvdp_format_state(const adamvdp_snapshot *s, char *out, int max);

/* A classic hex dump of VRAM: rows of 16 bytes, "ADDR  xx .. xx  ascii",
 * starting at base (wrapping at 16K). Returns the length written. */
int adamvdp_format_hex(const adamvdp_snapshot *s, uint16_t base, int rows,
                       char *out, int max);

/* Decoders render RGBA8888 into caller-provided buffers; both toolkits just
 * wrap the result in a texture/QImage. Sizes are fixed:
 *   nametable: 256x192   patterns: 256x64 (one 32x8-tile bank)
 *   sprites:   128x64 (32 cells of 16x16 in an 8x4 grid)
 *   palette:   ADAMVDP_PAL_W x ADAMVDP_PAL_H */
void adamvdp_render_nametable(const adamvdp_snapshot *s, uint8_t *rgba);
void adamvdp_render_patterns(const adamvdp_snapshot *s, int bank,
                             uint8_t *rgba);

typedef struct {
    int y, x;        /* SAT position (y is the raw SAT value) */
    int pattern;
    int color;
    int early_clock; /* EC bit */
} adamvdp_sprite;

void adamvdp_render_sprites(const adamvdp_snapshot *s, uint8_t *rgba,
                            adamvdp_sprite info[32]);

/* The palette as 16 separated swatches in an 8x2 grid, each labeled with
 * its hex index and outlined against the gutter so adjacent entries never
 * read as one block. Entry 0 (transparent) is drawn as a checkerboard. */
#define ADAMVDP_PAL_CELL 40
#define ADAMVDP_PAL_COLS 8
#define ADAMVDP_PAL_ROWS 2
#define ADAMVDP_PAL_W (ADAMVDP_PAL_CELL * ADAMVDP_PAL_COLS)
#define ADAMVDP_PAL_H (ADAMVDP_PAL_CELL * ADAMVDP_PAL_ROWS)
void adamvdp_render_palette(const adamvdp_snapshot *s, uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif /* ADAMDEBUG_H */
