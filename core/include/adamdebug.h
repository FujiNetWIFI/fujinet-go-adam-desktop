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
 * wins over a hex literal), 49152 (decimal), %1010. In the console only, an
 * address argument that is a bare 1-4 digit token ("8024") is hex unless it
 * names a label; adamdebug_eval itself reads bare digits as decimal.
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

#include "adamcore_debug.h"
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
 * bpr/bpw/bpa <addr>[-<end>] [cond] (read / write / either),
 * bpin/bpout <port>[-<end>] [cond], delete <id|addr>, enable/disable <id>,
 * clear, breaks, hits [clear], print <expr>, regs, set <reg> <expr>,
 * mem <addr> [len], poke <addr> <val...>, vram <addr> [len], vpoke <addr>
 * <val...>, vdp, io (memory map text, ports, controllers, VDP, CPU, FujiNet),
 * map (the memory map by region), cart, sound, video [section], fujinet,
 * label <addr> <name>, labels [file] (load symbols), symbols [text] (list),
 * disasm [addr] [n], trace [on|off|clear|n], watch [expr|lo-hi],
 * unwatch <n|all>, find [hex] <bytes> | find text <string>,
 * save <kind> <file>. Expressions: numbers (see above), registers (a f b c d e
 * h l af bc de hl ix iy sp pc i r im iff1 iff2 ixh ixl iyh iyl, primed af'
 * bc' de' hl'), flags (cf nf pf hf zf sf yf xf), [addr] byte and {addr} word
 * reads, labels, line vline frame cycles vdpaddr vdpsecond, and in a
 * breakpoint condition `value`, `old` and `addr` for the access; C operators
 * with C precedence.
 * The stepping commands wait up to a second and a half for the machine to stop
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
/* The memory map, the ports, the VDP registers, the controllers, the CPU's
 * interrupt state and FujiNet as monospace text lines (the Cart & I/O tab and
 * the console's `io`; 4K is plenty). Returns the length. */
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
    ADAMDEBUG_BP_IN = 8, ADAMDEBUG_BP_OUT = 16,
    /* Read and write as one entry ("Access"): READ | WRITE, one id, one hit
     * count. */
    ADAMDEBUG_BP_ACCESS = 6
} adamdebug_bp_type;
#define ADAMDEBUG_MAX_BREAKPOINTS 128
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
/* Records only while attached with tracing on (it needs the exec hook). The
 * ring (ADAMDEBUG_TRACE_SIZE records) is allocated on the first enable. Trace
 * and History share it: it records while either asked for it. */
/* One record per executed instruction: the registers are the state BEFORE the
 * instruction at pc ran (a breakpoint or pause stop never records an
 * instruction that did not run). The ring stores raw registers and the four
 * bytes at pc; len is worked out when a record is read. A HALT that is
 * already halted folds into the previous record (repeat counts the extra
 * 4-cycle spins). */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl;
    uint8_t len, bytes[4];
    uint64_t cycles;
    int16_t line;
    /* added with the newer debugger */
    uint16_t ix, iy, af2, bc2, de2, hl2;
    uint8_t reg_i, reg_r, im;
    uint8_t iff;        /* bit 0 IFF1, bit 1 IFF2 */
    uint8_t halted;     /* the CPU was already in HALT when this record began */
    uint32_t repeat;    /* HALT spins folded into this record */
} adamdebug_trace;
#define ADAMDEBUG_TRACE_SIZE 65536
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

/* ===========================================================================
 * The newer family debugger: stops in words, breakpoints of every kind with
 * condition expressions, the command line, registers, memory map, machine and
 * sound state, search, symbols, trace and history. Same shape as the MSX,
 * Intellivision and ColecoVision desktops' engines (colecodebug.h), with the
 * adamdebug_ prefix; everything above keeps working. Calls are safe from any
 * thread; inspection is meant for the stopped state and answers with whatever
 * is current while the machine runs.
 * ======================================================================== */

/* ---- the stop, in words ----------------------------------------------------
 * "paused at $4010", "step at $4010", "frame at $0038", "scanline at $4010",
 * "stopped at $4013" (a one-shot from Over/Out/Run to), "breakpoint: exec
 * $4010", "wrote $7000 = $12 (was $11) at $4A3C", "read $7000 = $12 at
 * $4A3C", "in $FC = $FF at $4A3C", "out $A1 = $12 at $4A3C". While running it
 * says "running" and *address is -1. A watch stop is reported at the
 * instruction AFTER the access (the machine stops at the next instruction
 * boundary). The older adamdebug_stop_reason keeps its terse wording. Returns
 * the length. */
int  adamdebug_stop_text(adamdebug *dbg, char *dst, int dstsz, int *address);

enum {
    ADAMDEBUG_STOP_NONE = 0,
    ADAMDEBUG_STOP_PAUSE,
    ADAMDEBUG_STOP_STEP,
    ADAMDEBUG_STOP_BREAKPOINT,   /* an EXEC entry */
    ADAMDEBUG_STOP_WATCH_READ,
    ADAMDEBUG_STOP_WATCH_WRITE,
    ADAMDEBUG_STOP_WATCH_IN,
    ADAMDEBUG_STOP_WATCH_OUT,
    ADAMDEBUG_STOP_ONESHOT,      /* step over / out / run to */
    ADAMDEBUG_STOP_FRAME,
    ADAMDEBUG_STOP_SCANLINE
};

typedef struct {
    int      reason;        /* ADAMDEBUG_STOP_* */
    int      bp_id;         /* the entry that fired, 0 if none */
    uint16_t addr;          /* the byte or port touched (watch), or the PC */
    int      value;         /* the byte moved, -1 when not a watch */
    int      old_value;     /* the byte a write replaced, -1 when not known */
    uint16_t pc;            /* the PC at the stop */
    int      from_pc;       /* the instruction that made the access, -1 unless a watch */
    uint64_t serial;        /* adamdebug_stop_serial at the stop */
} adamdebug_stop_info;
/* Returns 1 while the machine is stopped, 0 while it runs (out then holds
 * the last stop). */
int  adamdebug_last_stop(adamdebug *dbg, adamdebug_stop_info *out);
/* Bumped by every stop (pause, breakpoint, step); frontends recentre their
 * views on the PC when it changes. */
uint64_t adamdebug_stop_serial(adamdebug *dbg);

/* Everything about the current stop read in one critical section, so a
 * poller never sees a stop with the old serial or the old reason: stopped,
 * the serial, the stop info and its text agree. paused is 1 only once the
 * machine has really stopped (adamdebug_is_stopped). Returns paused. */
typedef struct {
    int      paused;
    uint64_t serial;
    adamdebug_stop_info stop;
    char     text[160];
    int      address;       /* the PC to show, -1 when running */
} adamdebug_snapshot;
int  adamdebug_stop_snapshot(adamdebug *dbg, adamdebug_snapshot *out);

/* ---- expressions --------------------------------------------------------------
 * The one grammar for conditions, print, set, watches and every address the
 * console takes: numbers ($C000 0xC000 C000h 49152 %1010), the Z80
 * registers (a f b c d e h l af bc de hl ix iy sp pc i r im iff1 iff2 af' bc'
 * de' hl' ixh ixl iyh iyl), the flags (cf nf pf xf hf yf zf sf), labels,
 * [addr] the byte and {addr} the word at addr (as the CPU sees them, no side
 * effects), frame (frames completed), cycles (Z80 clocks), line / vline (the
 * VDP line in the frame), vdpaddr / vdpsecond (the VDP's address register and
 * control-port latch) and, for an access, value, addr and old; C's operators
 * with C's precedence. In a breakpoint condition `value`/`addr`/`old` name the
 * byte the access moved (old = the byte a write replaced; for a read the value
 * itself). With the machine stopped at a watch, value/addr/old name that
 * access. (adamdebug_eval, above, is this grammar.) */
/* Register names the grammar knows, NULL-terminated, for completion. */
extern const char *const adamdebug_reg_names[];

/* ---- registers, in bulk ---------------------------------------------------------
 * Writes every register of `regs` (the main and shadow sets, ix iy sp pc, i r,
 * im, iff1/iff2 and halted; cycles is the machine's timebase and is ignored).
 * Only while stopped: returns 1, or 0 with a reason in err (running, IM above
 * 2, a flag that is not 0 or 1, no machine). On success err may hold a NOTE
 * for the user: "PC changed while the CPU is halted: it stays halted until
 * HALT is cleared or an interrupt arrives". */
int  adamdebug_regs_set(adamdebug *dbg, const adamcore_z80_regs *regs, char *err,
                        int errsz);
/* One register or flag by the names of the expression grammar (a..l, af..hl,
 * ix iy sp pc, shadows, i r im iff1 iff2, sf zf yf hf xf pf nf cf, ixh...).
 * Same rules and notes as adamdebug_regs_set. */
int  adamdebug_register_set(adamdebug *dbg, const char *name, long value,
                            char *err, int errsz);

/* ---- breakpoints, more ---------------------------------------------------------
 * Execute breakpoints ride adamcore's PC map; the others ride its bus hook and
 * stop at the next instruction boundary (an opcode fetch never counts as a
 * READ). ADAMDEBUG_BP_ACCESS (READ | WRITE) is one entry with one id. */
/* Replaces the condition of an entry; "" removes it. 1, or 0 with err. */
int  adamdebug_breakpoint_set_condition(adamdebug *dbg, int id,
                                        const char *condition, char *err,
                                        int errsz);
void adamdebug_breakpoint_clear_hits(adamdebug *dbg);

/* ---- symbols, for a Symbols tab ---------------------------------------------------
 * The built-in EOS and OS7 names, loaded files and user labels as one list. */
int  adamdebug_symbols_count(adamdebug *dbg);
/* Entry `index` (0 .. count-1). Returns 1, or 0 past the end. */
int  adamdebug_symbols_get(adamdebug *dbg, int index, uint16_t *addr, char *name,
                           int namesz);
/* Indices of the entries whose name contains `text` (case-insensitive; empty
 * matches all), ordered by address then name. Returns the count written. */
int  adamdebug_symbols_filter(adamdebug *dbg, const char *text, int *out,
                              int max);
/* Bumped when symbols are loaded, labelled or cleared. */
unsigned adamdebug_symbols_generation(adamdebug *dbg);
/* Drops every loaded file and user label; the built-ins stay. */
void adamdebug_clear_symbols(adamdebug *dbg);

/* ---- search and the memory map ------------------------------------------------------ */
#define ADAMDEBUG_SEARCH_NOCASE 1
/* Finds up to max start addresses in from..to of the CPU's 64K as mapped now,
 * where (mem & mask) == (pat & mask) for all count bytes. Returns the count,
 * or -1 while the machine is running or on an error. */
int  adamdebug_search(adamdebug *dbg, const uint8_t *pat, int count,
                      uint8_t mask, int flags, uint16_t from, uint16_t last,
                      uint16_t *out, int max);
/* The next match after `from` (before it if backward), wrapping once.
 * Returns 1 and *found, 0 for none, -1 while running. */
int  adamdebug_search_next(adamdebug *dbg, const uint8_t *pat, int count,
                           uint8_t mask, int flags, uint16_t from,
                           int backward, uint16_t *found);

typedef struct {
    uint16_t low, high;     /* inclusive */
    int      readable, writable;
    char     name[64];      /* "SmartWriter ROM", "RAM (24K)", "Cartridge (32768 bytes)" */
} adamdebug_region;
/* Ordered and gap-free from $0000 to $FFFF as the machine is mapped now, from
 * the memory configuration (port $7F lower and upper bank, port $3F EOS ROM)
 * and, on a bare ColecoVision, its 1K RAM and any Super Game Module RAM; the
 * name says which configuration and bank. Returns how many (up to max; at most
 * 10). */
int  adamdebug_memory_map(adamdebug *dbg, adamdebug_region *out, int max);

/* ---- trace and history ------------------------------------------------------------------
 * Trace (newest first, live) and History (what ran before the machine
 * stopped, oldest first) read the ring described at adamdebug_trace. Both are
 * safe while running. */
/* Entries held. */
int  adamdebug_trace_count(adamdebug *dbg);
/* Records pushed since the last clear: a tab polls this to see new lines. */
uint64_t adamdebug_trace_total(adamdebug *dbg);
/* "0006  IN A,($DC)  AF=.... BC=.... DE=.... HL=.... SP=....". Returns the
 * length. */
int  adamdebug_trace_format(const adamdebug_trace *rec, char *dst, int dstsz);
enum { ADAMDEBUG_TRACE_ALL = 0, ADAMDEBUG_TRACE_BRANCHES, ADAMDEBUG_TRACE_RANGE };
/* 1 if the entry passes the filter (BRANCHES: jumps, calls, returns;
 * RANGE: pc in low..high). */
int  adamdebug_trace_match(const adamdebug_trace *rec, int filter,
                           uint16_t low, uint16_t high);
typedef adamdebug_trace adamhist_entry;
void adamdebug_history_enable(adamdebug *dbg, int on);
int  adamdebug_history_enabled(adamdebug *dbg);
void adamdebug_history_clear(adamdebug *dbg);
uint64_t adamdebug_history_count(adamdebug *dbg);
/* Up to max of the newest entries, oldest first; returns how many. */
int  adamdebug_history_copy(adamdebug *dbg, adamhist_entry *out, int max);
/* "50CD  CALL $50D5   AF=.... BC=.... DE=.... HL=.... IX=.... IY=....
 * SP=....  SZ.H.PNC". */
int  adamdebug_history_format(const adamhist_entry *rec, char *dst, int dstsz);
int  adamdebug_history_match(const adamhist_entry *rec, int filter,
                             uint16_t low, uint16_t high);

/* ---- the machine: Cart & I/O ---------------------------------------------------------
 * Read from adamcore's accessors (the mirrors the emulator thread keeps);
 * valid while stopped, a stale value while running is only a stale display. */
typedef struct {
    int      valid;                 /* a machine is running */
    /* the controllers, as the pads sent them */
    uint8_t  joy_mode;              /* the strobe: 1 = joystick half ($C0), 0 = keypad half ($80) */
    struct {
        uint16_t raw;               /* the word given to adamcore_set_joystick */
        uint8_t  joystick_byte;     /* what the console reads in the joystick half (active low) */
        uint8_t  keypad_byte;       /* ... in the keypad half */
        int      up, right, down, left, fire_left, fire_right;   /* 1 = pressed */
        int      keypad;            /* 0-9, 10 '*', 11 '#', -1 none */
        char     keypad_name[4];    /* "", "0".."9", "*", "#" */
    } pad[2];
    /* the VDP's host side */
    uint8_t  vdp_regs[8], vdp_status;
    uint16_t vdp_addr;
    uint8_t  vdp_latch, vdp_latched, vdp_read_ahead, vdp_int_line;
    /* the CPU's side */
    uint8_t  cpu_q, ei_pending, irq_pending, nmi_pending;
    uint64_t nmi_count;
    int      beam_line;             /* scanline 0..261 of the frame in progress */
    int      beam_tstate;           /* T-states into it */
    uint64_t frames;                /* frames completed */
    /* the memory configuration */
    uint8_t  mem_ctrl, net_ctrl;    /* ports $7F and $3F */
    int      game_mode;             /* the last reset was a game (ColecoVision) reset */
    int      lower, upper;          /* bank selects 0-3 */
    const char *lower_name, *upper_name;
    int      eos_rom;               /* EOS ROM visible at $6000-$7FFF in lower bank 0 */
    int      net_reset;             /* port $3F D0 */
    int      cv_console;            /* the session started as a bare ColecoVision */
    /* the Opcode Super Game Module (ColecoVision only) */
    int      sgm;                   /* fitted */
    int      sgm_ram_en, sgm_bios_off;
    uint8_t  ay_addr, ay_regs[16];
    /* FujiNet and AdamNet */
    int      fujinet_runtime;       /* the in-process or external FujiNet is on */
    int      fujinet_link;          /* its BoIP client is connected */
    uint32_t fujinet_packets[2];    /* [0] ADAM -> FujiNet, [1] FujiNet -> ADAM, since the session started */
} adamdebug_io;
int  adamdebug_io_get(adamdebug *dbg, adamdebug_io *out);
/* (adamdebug_io_state, declared with the memory map above, is the same as
 * text lines for the Cart & I/O tab and the `io` command.) */

typedef struct {
    int      valid;                 /* a machine is running */
    char     machine[32];           /* "ADAM" or "ColecoVision" */
    int      cv_console;            /* started as a ColecoVision */
    int      sgm;                   /* an Opcode Super Game Module is fitted */
    int      cartridge;             /* a cartridge image is open */
    char     cart_path[256];        /* its file ("" when none) */
    long     cart_size;             /* bytes on disk, -1 when unreadable */
    int      cart_header;           /* what the cartridge window starts with:
                                       0 nothing readable, 1 $AA55 (title screen
                                       first), 2 $55AA (straight to the game),
                                       3 anything else */
    int      cart_visible;          /* the cartridge window is mapped at $8000 */
    uint8_t  mem_ctrl, net_ctrl;
    char     config[96];            /* "lower: OS7 ROM + RAM, upper: cartridge, EOS ROM off" */
    int      fujinet_runtime;       /* FujiNet (in-process or external) is on */
    int      fujinet_link;          /* its BoIP client is connected */
    char     boip[80];              /* "127.0.0.1:65216" */
} adamdebug_cart;
void adamdebug_cart_info(adamdebug *dbg, adamdebug_cart *out);
/* One line: "ADAM, memory 0F (lower OS7 ROM + RAM, upper cartridge), cartridge
 * loop.rom (128 bytes), FujiNet connected". */
int  adamdebug_cart_summary(adamdebug *dbg, char *dst, int dstsz);

/* ---- Sound: the SN76489, and the AY-3-8910 of a Super Game Module ---------------------
 * From adamcore's emulator-thread mirrors (the live chips are ~50 ms behind on
 * the audio thread). Valid while stopped. */
typedef struct {
    int      present;               /* always 1 with a machine */
    uint8_t  latched_reg;           /* 0..7: tone0 period, tone0 atten, ... noise atten */
    int      period[3];             /* 10 bits; 0 counts as 1024 */
    double   freq[3];               /* 3579545 / (32 * period) */
    int      atten[4];              /* tones 0-2 then noise: 0 loudest, 15 muted, 2 dB a step */
    int      level[4];              /* 15 - atten: 0 silent .. 15 loudest */
    uint8_t  noise_ctrl;
    int      noise_white;           /* bit 2: white noise, else periodic */
    int      noise_rate;            /* bits 0-1: 0..2 fixed rates, 3 follows tone 2 */
    double   noise_hz;
} adamdebug_sn;

typedef struct {
    int      present;               /* the SGM is fitted */
    uint8_t  addr;                  /* the latched register */
    uint8_t  reg[16];               /* R7 mixer is active-low */
    int      period[3];             /* 12 bits */
    double   freq[3];               /* 111860.8 / period */
    int      tone_on[3], noise_on[3];
    int      level[3];              /* 0-15 */
    int      envelope[3];           /* the voice follows the envelope */
    int      noise_period;          /* 5 bits */
    int      env_period;            /* 16 bits */
    int      env_shape;             /* R13, low 4 bits */
} adamdebug_ay;

typedef struct {
    adamdebug_sn sn;
    adamdebug_ay ay;
} adamdebug_sound;
/* Returns 1 with a machine, 0 without. */
int  adamdebug_sound_get(adamdebug *dbg, adamdebug_sound *out);

/* ---- the Video tab's text for the console `video` command ------------------------------
 * func gets a section name (or "") and returns text that stays valid until its
 * next call, NULL when there is no frame yet. Set by whoever owns the Video
 * controller; until then `video` says there is no video state. */
void adamdebug_set_video_text(const char *(*func)(const char *section));

#ifdef __cplusplus
}
#endif

#endif /* ADAMDEBUG_H */
