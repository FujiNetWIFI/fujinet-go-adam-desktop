/*
 * adamdebug -- the debugger contract: a Z80 debugger engine over adamcore
 * (core/debugger/) behind a C API the native debugger windows share. Same
 * shape as the SMS and MSX siblings' smsdebug.h / msxdebug.h, so the windows
 * carry over; the engine is this repository's own, built on adamcore's
 * debug interface (adamcore_debug.h): the instruction-stepped run loop, the
 * PC breakpoint bitmap, the per-instruction exec hook and the bus hook.
 *
 * The engine only engages while a window holds it attached:
 * adamdebug_attach() when the window opens -- which also stops the machine,
 * as on every sibling -- and adamdebug_detach() when it closes, which takes
 * every breakpoint, the exec hook and the bus hook out of the core and lets
 * the machine run on at full speed on the plain frame path. Breakpoints are
 * kept for the next attach. The FujiNet trace is the exception: it records
 * from session start, so it shows what happened before anyone opened a
 * window.
 *
 * Stopping parks the emulation thread between instructions; "stopped" means
 * it is parked. Everything that changes the machine (registers, step, run)
 * needs it stopped; reads are always allowed and answer with whatever is
 * current while it runs. A window learns of stops, breakpoint hits and edits
 * by polling adamdebug_generation() (every 100 ms is plenty); the optional
 * stop callback fires on the emulator thread.
 *
 * Numbers in the console and in conditions: $C000, 0xC000, C000h (a name
 * wins over a hex literal), 49152 (decimal), %1010.
 *
 * Calls are safe from the UI thread at any time, including while the session
 * restarts (the engine, its breakpoints and its attach state survive a power
 * cycle or media change; an attached engine stops the new machine at its
 * first instruction). Reads of registers and memory while the machine runs
 * are not synchronised with it: they answer with whatever is current.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAMDEBUG_H
#define ADAMDEBUG_H

#include <stdint.h>

#include "adamsession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The engine, lazily created by adamsession_debugger(s) (adamsession.h) and
 * alive as long as the session object. */

/* ---- attach / stop / go ---------------------------------------------------- */
/* Engage the engine and stop at the next instruction. The emulation thread
 * notices within a frame (<= ~17 ms); poll adamdebug_is_stopped() or the
 * generation. Idempotent. Before the session has started it stops the
 * machine as soon as it exists. Call it when the debugger window opens. */
void adamdebug_attach(adamdebug *d);
/* Disengage and let the machine run. Breakpoints are kept for the next
 * attach. Returns once nothing is installed in the core any more (bounded
 * wait). Call it when the debugger window closes. */
void adamdebug_detach(adamdebug *d);
int  adamdebug_is_attached(adamdebug *d);

/* 1 once the emulation thread is parked. 0 while running, stepping, before
 * the session starts, and right after resume/step/detach. */
int  adamdebug_is_stopped(adamdebug *d);
void adamdebug_stop(adamdebug *d);       /* break at the next instruction */
void adamdebug_resume(adamdebug *d);     /* "run" */
/* Why the machine last stopped ("stopped", "breakpoint: exec $8033",
 * "write $6000 = $03", "read $6000 = $03", "out $FF <- $9F", "in $FC -> $1F",
 * "step", "step over", "step out", "run to", "scanline", "frame") and the PC
 * it stopped at; "running" and -1 while it runs. Returns the length. */
int  adamdebug_stop_reason(adamdebug *d, char *dst, int dstsz, int *address);
/* Bumped whenever the machine stopped or resumed, a command ran, a register,
 * memory or label was edited, or the breakpoint list changed, so a window
 * knows when to refresh. */
unsigned adamdebug_generation(adamdebug *d);

/* Optional push for tests: fires on the EMULATOR thread, with no engine lock
 * held, each time the machine parks (after the state is visible to the
 * calls above; it may call resume/step). Windows should poll the generation
 * instead. */
typedef enum {
    ADAMDBG_STOP_PAUSE = 0,   /* stop request, or attach */
    ADAMDBG_STOP_BREAKPOINT,  /* an execute breakpoint (before it runs) */
    ADAMDBG_STOP_WATCH,       /* a read/write/IN/OUT breakpoint (after the
                                 accessing instruction) */
    ADAMDBG_STOP_STEP,        /* step, over, out, scanline or frame done */
    ADAMDBG_STOP_RUNTO        /* run-to address reached */
} adamdebug_stop_kind;
void adamdebug_set_stop_callback(adamdebug *d,
                                 void (*cb)(void *ud, int kind, uint16_t pc),
                                 void *ud);

/* ---- the console ------------------------------------------------------------ */
/* Run one command line. "help" lists them: step [n], over, out, run, stop,
 * runto <addr>, scanline [n], frame [n], break <addr>[-<end>] [cond],
 * bpr/bpw <addr>[-<end>] [cond], bpin/bpout <port>[-<end>] [cond],
 * delete <id|addr>, enable/disable <id>, clear, breaks, print <expr>, regs,
 * set <reg> <expr>, mem <addr> [len], poke <addr> <val...>, vram <addr>
 * [len], vpoke <addr> <val...>, vdp, io (the memory map and ports), fujinet,
 * label <addr> <name>, labels [file], disasm [addr] [n], trace
 * [on|off|clear|n], save <kind> <file>. Expressions: numbers (see above),
 * registers (a f b c d e h l af bc de hl ix iy sp pc i r im iff1 iff2, primed
 * af' bc' de' hl'), flags (cf nf pf hf zf sf yf xf), [addr] byte and {addr}
 * word reads, labels, line frame cycles, and in a breakpoint condition
 * `value` and `addr` for the access; C operators with C precedence.
 * The stepping commands wait up to a quarter second for the machine to stop
 * again and then say where it is; run/go return at once.
 * Output text into dst; returns its length. */
int  adamdebug_command(adamdebug *d, const char *command, char *dst, int dstsz);
/* Completions for the end of a line (commands, registers, labels, save
 * kinds), one per line. Returns the count. */
int  adamdebug_completions(adamdebug *d, const char *prefix, char *dst,
                           int dstsz);
/* Evaluate an expression (the grammar above, no `value`/`addr`): for an
 * address field that should take labels and arithmetic. 0 and *value on
 * success, -1 with the reason in err. */
int  adamdebug_eval(adamdebug *d, const char *text, long *value, char *err,
                    int errsz);

/* ---- stepping shortcuts (the toolbar) -------------------------------------- */
/* All need the machine stopped (they do nothing otherwise) and return at
 * once; the machine runs and stops again, bumping the generation. The first
 * instruction after a stop always runs, whatever sits at the PC. Breakpoints
 * and watchpoints stop any of them early, with their own reason. */
void adamdebug_step(adamdebug *d);        /* F7: one instruction */
void adamdebug_step_over(adamdebug *d);   /* F8: over a CALL/RST/DJNZ/block
                                             repeat/HALT; else one step */
void adamdebug_step_out(adamdebug *d);    /* Shift+F8: to the RET (the return
                                             address on the stack, with an SP
                                             guard for recursion) */
/* n scanlines (the beam line wraps 261 -> 0) / n frames: the end of the
 * frame in progress is the first, so "frame" always lands on a frame
 * boundary with the whole picture drawn. */
void adamdebug_scanline(adamdebug *d, int n);
void adamdebug_frame(adamdebug *d, int n);
/* Run until PC == addr. */
void adamdebug_run_to(adamdebug *d, uint16_t addr);

/* ---- CPU --------------------------------------------------------------------- */
typedef struct {
    int pc, sp, af, bc, de, hl, ix, iy, wz;
    int af2, bc2, de2, hl2;                /* the shadow set */
    int i, r, im, iff1, iff2, halted;
    int sf, zf, yf, hf, xf, pf, nf, cf;    /* the flags in F */
    uint64_t cycles;                       /* T-states since power-on */
    int line;                              /* beam scanline 0-261 of the frame in
                                              progress (it can lag one line right
                                              after the line's last instruction) */
    uint32_t frame;                        /* frames completed since power-on */
} adamdebug_cpu;
void adamdebug_cpu_get(adamdebug *d, adamdebug_cpu *out);
typedef enum {
    ADAM_REG_PC = 0, ADAM_REG_SP, ADAM_REG_AF, ADAM_REG_BC, ADAM_REG_DE,
    ADAM_REG_HL, ADAM_REG_IX, ADAM_REG_IY, ADAM_REG_AF2, ADAM_REG_BC2,
    ADAM_REG_DE2, ADAM_REG_HL2, ADAM_REG_I, ADAM_REG_R, ADAM_REG_IM,
    ADAM_REG_IFF1, ADAM_REG_IFF2,
    ADAM_REG_A, ADAM_REG_F, ADAM_REG_B, ADAM_REG_C, ADAM_REG_D, ADAM_REG_E,
    ADAM_REG_H, ADAM_REG_L,
    ADAM_FLAG_S, ADAM_FLAG_Z, ADAM_FLAG_H, ADAM_FLAG_PV, ADAM_FLAG_N,
    ADAM_FLAG_C,
    ADAM_REG_COUNT
} adamdebug_reg;
/* Needs the machine stopped. Setting PC redirects the next instruction. */
void adamdebug_cpu_set(adamdebug *d, int reg, int value);

/* ---- memory ------------------------------------------------------------------- */
/* CPU-bus reads through the live bank map, without side effects. Returns n
 * (0 with the session not running). */
int  adamdebug_read(adamdebug *d, uint16_t addr, uint8_t *dst, int n);
/* A debugger edit through the same map: RAM takes it, ROM ignores it, and a
 * cartridge bank register is not touched. */
void adamdebug_write(adamdebug *d, uint16_t addr, uint8_t value);

/* What is mapped where, from ports $7F (bank control) and $3F (net control).
 * Lower 32K: bank 0 SmartWriter ROM (EOS ROM over $6000-$7FFF when
 * eos_rom), 1 RAM, 2 expansion RAM, 3 OS7 ROM at $0000-$1FFF with RAM above
 * (24K on an ADAM, a mirrored 1K at $6000 on a ColecoVision). Upper 32K:
 * bank 0 RAM, 1 the empty expansion ROM socket, 2 expansion RAM,
 * 3 the cartridge. */
typedef struct {
    uint8_t mem_ctrl, net_ctrl;
    int lower, upper;        /* bank selects 0-3 (mem_ctrl D1-D0, D3-D2) */
    const char *lower_name, *upper_name;
    int eos_rom;             /* net_ctrl D1: EOS ROM visible at $6000-$7FFF in lower bank 0 */
    int net_reset;           /* net_ctrl D0 */
    int game_mode;           /* the last reset was a game (ColecoVision) reset */
    int cv_console;          /* the session started as a bare ColecoVision */
} adamdebug_memmap;
void adamdebug_memmap_get(adamdebug *d, adamdebug_memmap *out);
/* The memory map, the ports and the VDP registers as monospace text lines
 * (the Memory Map & I/O tab; ~2K is plenty). Returns the length. */
int  adamdebug_io_state(adamdebug *d, char *dst, int dstsz);

/* ---- disassembly -------------------------------------------------------------- */
#define ADAMDASM_JUMP     0x01
#define ADAMDASM_CALL     0x02
#define ADAMDASM_RET      0x04
#define ADAMDASM_COND     0x08
#define ADAMDASM_RELATIVE 0x10
#define ADAMDASM_BLOCK    0x20 /* LDIR/CPIR/INIR/OTIR family */
#define ADAMDASM_HALT     0x40

typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;      /* an execute breakpoint here, enabled or not */
    int breakpoint_enabled;
    int length;
    char bytes[16];          /* "CD 50 80" */
    char label[48];          /* the label at this address, or "" */
    char disasm[64];         /* "CALL $8050" */
    char comment[64];        /* a jump/call target's label, an IN/OUT port's name */
    uint16_t target;         /* jump/call destination when flags say so */
    uint8_t flags;           /* ADAMDASM_* */
} adamdebug_line;
/* Up to `max` lines starting at `addr`, through the current memory map.
 * Returns the count; *pc_line is the index of the PC's line in `out`, -1
 * when not among them (may be NULL). */
int  adamdebug_disassemble(adamdebug *d, uint16_t addr, adamdebug_line *out,
                           int max, int *pc_line);
/* The address `rows` disassembly rows before (negative) or after `addr` --
 * for scrolling, and for "Follow PC" (PC a third of the way down).
 * Backwards is a best guess, as on every Z80 debugger. Clicks in the view
 * should map through the lines' `address`, never through a row number. */
int  adamdebug_row_address(adamdebug *d, uint16_t addr, int rows);

/* ---- symbols and labels -------------------------------------------------------
 * .sym format: "HHHH NAME [; comment]" per line, '#' comments; also read:
 * WLA-DX ("BB:AAAA name"), z88dk .map ("name = $AAAA ; ...") and SDCC .noi
 * ("DEF name 0xAAAA"). The built-in EOS and OS7 tables (generated from the
 * Drushel EOS-5 disassembly, the eoslib jump-table names and the os7lib
 * listing) load when the engine is created. */
/* Load a symbol file as table "file" (replacing the last one). With path
 * NULL, <cartridge>.sym/.map/.noi next to the opened cartridge. Message into
 * msg; returns labels loaded or -1. */
int  adamdebug_load_symbols(adamdebug *d, const char *path, char *msg,
                            int msgsz);
/* Address of a label, or -1. Label at an address into dst (exact match;
 * empty if none). A label set here goes into table "user". */
int  adamdebug_label_address(adamdebug *d, const char *label);
int  adamdebug_address_label(adamdebug *d, uint16_t addr, char *dst,
                             int dstsz);
/* Label addr (NULL or "" removes the user label there). Returns 0. */
int  adamdebug_set_label(adamdebug *d, uint16_t addr, const char *label);
/* Lower-level: load a file into a named table; the nearest label at or
 * below addr with its offset; a name's address (1 found). */
int  adamdebug_symbols_load(adamdebug *d, const char *path, const char *table);
const char *adamdebug_symbol_at(adamdebug *d, uint16_t addr,
                                uint16_t *offset);
int  adamdebug_symbol_find(adamdebug *d, const char *name, uint16_t *addr);

/* ---- breakpoints -------------------------------------------------------------- */
typedef enum {
    ADAMDEBUG_BP_EXEC = 1, ADAMDEBUG_BP_READ = 2, ADAMDEBUG_BP_WRITE = 4,
    ADAMDEBUG_BP_IN = 8, ADAMDEBUG_BP_OUT = 16
} adamdebug_bp_type;
typedef struct {
    int id;                  /* never reused */
    int type;                /* adamdebug_bp_type bits */
    uint16_t start, end;     /* an address range, or a port range (0-255) for
                                IN/OUT */
    int enabled;
    unsigned hits;           /* times it fired (its condition true) */
    char condition[128];     /* an expression, "" for none */
} adamdebug_breakpoint;
/* READ ignores opcode and operand fetches. WRITE, READ, IN and OUT stop
 * after the accessing instruction; EXEC stops before the instruction runs.
 * A condition that fails to evaluate at run time stops rather than hides.
 * Conditions see the registers as they are in the middle of the instruction
 * for the access kinds. */
/* Execute breakpoint at addr: added if absent, removed if present. Returns
 * 1 when one is now set. */
int  adamdebug_breakpoint_toggle(adamdebug *d, uint16_t addr);
/* 1 when an execute breakpoint is at addr (enabled or not). */
int  adamdebug_breakpoint_check(adamdebug *d, uint16_t addr);
/* type: any combination of the bits (IN/OUT with ports only). Returns the
 * new id, or -1 with the reason in err (a condition that does not parse or
 * names something unknown, a port above 255, a full table). */
int  adamdebug_breakpoint_add(adamdebug *d, int type, uint16_t start,
                              uint16_t end, const char *condition, char *err,
                              int errsz);
void adamdebug_breakpoint_remove(adamdebug *d, int id);
void adamdebug_breakpoint_enable(adamdebug *d, int id, int enabled);
/* Copies up to max, in creation order. Returns the count. */
int  adamdebug_breakpoint_list(adamdebug *d, adamdebug_breakpoint *out,
                               int max);
void adamdebug_breakpoint_clear(adamdebug *d);

/* ---- the instruction trace ring ---------------------------------------------- */
/* Records only while attached with tracing on (it needs the exec hook). */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl;
    uint8_t len, bytes[4];
    uint64_t cycles;
    int16_t line;
} adamdebug_trace;
void adamdebug_trace_enable(adamdebug *d, int on);
int  adamdebug_trace_enabled(adamdebug *d);
/* Newest first; returns entries copied. */
int  adamdebug_trace_read(adamdebug *d, adamdebug_trace *out, int max);
void adamdebug_trace_clear(adamdebug *d);

/* ---- files -------------------------------------------------------------------- */
/* Save with the path a native file picker supplied. kind: "dis" (the
 * disassembly of $0000-$FFFF as mapped now), "mem" (the 64K the CPU sees),
 * "vram" (the VDP's 16K), "regs" (CPU and VDP registers as text). Message
 * into msg; returns 0 or -1. */
int  adamdebug_save(adamdebug *d, const char *kind, const char *path,
                    char *msg, int msgsz);

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

/* ---- FujiNet: every AdamNet packet on the BoIP link ----------------------
 * Always on from session start, kept in a 512-entry ring. An exchange (a
 * request and its answers) that repeats the one before it byte for byte --
 * the 300 ms READY keepalive, a poll -- is kept once, with a count. */
typedef struct {
    uint64_t serial;    /* changes whenever the entry does */
    int      to_host;   /* 1: FujiNet -> ADAM, 0: ADAM -> FujiNet */
    int      device;    /* AdamNet address 0-15, -1 if not a packet */
    int      command;   /* the Fuji/network command it carries or answers,
                           else -1 */
    int      length;    /* SEND/DATA payload length, else -1 */
    int      nbytes;
    uint8_t  data[64];  /* the packet's first bytes */
    char     text[96];  /* "ADAM → FujiNet 6F SEND E8 GET ADAPTERCONFIG
                           (1 B, ck ok)" */
    uint32_t repeat;    /* times the exchange ran in a row (1 = once) */
} adam_fuji_packet;

/* The newest max packets, oldest first. Returns the count. */
int  adamdebug_fujinet_trace(adamdebug *d, adam_fuji_packet *out, int max);
void adamdebug_fujinet_trace_clear(adamdebug *d);
/* The BoIP link: its port and state, packet counts, and each address seen
 * with its last STATUS, as text lines. Returns the length written. */
int  adamdebug_fujinet_state(adamdebug *d, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* ADAMDEBUG_H */
