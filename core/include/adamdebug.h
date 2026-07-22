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

/* Decoders render RGBA8888 into caller-provided buffers; both toolkits just
 * wrap the result in a texture/QImage. Sizes are fixed:
 *   nametable: 256x192   patterns: 256x64 (one 32x8-tile bank)
 *   sprites:   128x64 (32 cells of 16x16 in an 8x4 grid)
 *   palette:   16x1 */
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
void adamvdp_render_palette(const adamvdp_snapshot *s, uint8_t *rgba);

#ifdef __cplusplus
}
#endif

#endif /* ADAMDEBUG_H */
