/*
 * debugger.c -- the adamdebug engine (core/include/adamdebug.h).
 *
 * Built on adamcore's debug interface (adamcore_debug.h). Attached, the
 * emulation thread leaves the fast frame path for adamdebug_session_frame(),
 * which runs adamcore_debug_run() in slices of SLICE instructions, looking at
 * the stop request between them; stopping PARKS the thread on a condition
 * variable until a window resumes it, so "stopped" means parked. What stops a
 * slice:
 *   - an enabled execute breakpoint: adamcore's PC bitmap is the pre-filter;
 *     on ADAMCORE_RUN_BREAKPOINT the conditions are evaluated and a false one
 *     is stepped past (one instruction with the check off) and the run goes on;
 *   - the exec hook, installed only while something needs it (a transient
 *     step-over/out/run-to/scanline target, the trace ring, or a watchpoint):
 *     it returns nonzero to stop BEFORE the instruction at the PC;
 *   - a watchpoint: the bus hook (installed only with an enabled READ, WRITE,
 *     IN or OUT breakpoint) records the hit in `pend`, and the exec hook
 *     stops the machine at the next instruction boundary, so it stops AFTER
 *     the accessing instruction. READ ignores opcode/operand fetches.
 * Detached, every breakpoint and both hooks are taken out of the core and the
 * session goes back to adamcore_run_frame(); nothing is paid.
 *
 * Threading. UI threads call the public API; the emulation thread runs the
 * loop and the hooks. State is under d->mtx (the hooks take it only when they
 * must evaluate a breakpoint); the three lookup maps the hooks read without
 * it are rebuilt in place, which at worst misses a breakpoint for a moment
 * while one is being edited. Everything that touches the adamcore hooks and
 * breakpoint bitmap runs on the emulation thread (sync_core_locked), and
 * only while the machine is not inside adamcore_debug_run. Calls that change
 * the machine need it parked; the stop callback fires with no lock held.
 *
 * The FujiNet trace stands apart: adamcore's BoIP hook feeds it from session
 * start, attached or not, under its own lock.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "adamcore_debug.h"
#include "../src/compat.h"
#include "../src/session_internal.h"
#include "debugger_internal.h"
#include "adamvidtab.h"
#include "expr.h"
#include "fujitrace.h"
#include "z80dasm.h"

#define MAX_BP  ADAMDEBUG_MAX_BREAKPOINTS
#define TRACE_RING_LEN ADAMDEBUG_TRACE_SIZE /* power of two */
#define SLICE   4096 /* instructions between looks at the stop request */

/* The raw ring record: registers as they were BEFORE the instruction, the
 * four bytes at pc. The instruction is decoded when a record is read. */
typedef struct {
    uint16_t pc, sp, af, bc, de, hl, ix, iy, af2, bc2, de2, hl2;
    uint8_t reg_i, reg_r, im, iff, halted;
    uint8_t bytes[4];
    uint64_t cycles;
    uint32_t repeat;
    int16_t line;
} ring_rec;

/* A watch that fired during an instruction, waiting for the boundary. */
typedef struct {
    int active;
    adamdebug_stop_info info;
    char why[64]; /* the terse wording of adamdebug_stop_reason */
} pending_hit;

enum { ACT_RUN = 0, ACT_STEP, ACT_OVER, ACT_OUT, ACT_RUNTO, ACT_SCANLINE,
       ACT_FRAME };
/* What the first call after a release does: run the instruction at the PC
 * with the stop checks off (NEXT_FIRST) or step past a false-condition
 * breakpoint (NEXT_PASS). */
enum { NEXT_NONE = 0, NEXT_PASS, NEXT_FIRST };

struct adamdebug {
    adamsession *session;
    pthread_mutex_t mtx;
    pthread_cond_t cv;   /* parks the emulation thread */
    pthread_cond_t evcv; /* the machine parked, or the core was cleaned up */

    int attached;
    int stopped;   /* the emulation thread is parked */
    int stop_req;  /* park at the next slice boundary */
    int act;       /* what the machine does once released */
    long count;    /* instructions / lines / frames still to go */
    uint16_t target, sp_guard;
    int last_line;
    int next1;

    char reason[128];
    int reason_addr;
    unsigned generation;
    /* The current stop, in the same critical section as `stopped`. */
    adamdebug_stop_info stop;
    uint64_t serial;

    void (*stop_cb)(void *ud, int kind, uint16_t pc);
    void *stop_ud;

    /* The breakpoint table, and what the hooks look up. */
    adamdebug_breakpoint bp[MAX_BP];
    int nbp, next_id, nwatch;
    uint8_t map_exec[65536]; /* nonzero: an enabled execute bp covers it */
    uint8_t map_mem[65536];  /* ADAMDEBUG_BP_READ / _WRITE bits */
    uint8_t map_io[256];     /* ADAMDEBUG_BP_IN / _OUT bits */
    int epoch;               /* bumped when the maps change */

    /* What is installed in the core (emulation thread, under mtx). */
    adamcore *armed_core;
    int armed_epoch;
    int hook_exec, hook_bus;
    int core_dirty;          /* something may be installed: clean up on detach */

    /* Hook-private: emulation thread only. */
    int skip;                /* the exec hook's first call is the released instruction */
    pending_hit pend;        /* a watchpoint fired during the instruction */
    int hook_kind;           /* what the exec hook stopped for ... */
    adamdebug_stop_info hook_info;
    char hook_why[64];       /* ... and the terse wording */
    uint16_t cur_pc;         /* the instruction being executed (exec hook) */

    /* The expression context (mtx held while evaluating). */
    adamcore_z80_regs rs;
    const adamcore *ec;
    long acc_value, acc_addr, acc_old;
    int in_access;

    /* The instruction history: trace and history share one ring, allocated on
     * the first enable. */
    pthread_mutex_t trace_mtx;
    int trace_on, history_want;
    ring_rec *ring;
    uint64_t ring_total;

    sym_tables syms;
    unsigned sym_gen;

    /* FujiNet packet trace, fed on the emulator thread by adamcore's BoIP
     * hook. Its own lock, so the wire never waits on a UI holding mtx. */
    pthread_mutex_t fuji_mtx;
    adamfuji *fuji;
};

/* The live core, or NULL. Read it with mtx held (or on the emulation
 * thread): the emulation thread clears it under mtx (adamdebug_clear_core)
 * before destroying the core, so a UI-thread call in flight never touches a
 * core that is going away. */
static adamcore *core_of(adamdebug *d)
{
    return d->session->core;
}

static uint8_t peek(adamdebug *d, uint16_t addr)
{
    adamcore *c = core_of(d);
    return c ? adamcore_peek(c, addr) : 0xFF;
}

static void bump_locked(adamdebug *d)
{
    d->generation++;
}

static void touch(adamdebug *d)
{
    pthread_mutex_lock(&d->mtx);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_touch(adamdebug *d)
{
    if (d) touch(d);
}

/* ---- expressions ------------------------------------------------------------ */

const char *const adamdebug_reg_names[] = {
    "a", "f", "b", "c", "d", "e", "h", "l", "af", "bc", "de", "hl", "ix", "iy",
    "sp", "pc", "i", "r", "im", "iff1", "iff2", "af'", "bc'", "de'", "hl'",
    "ixh", "ixl", "iyh", "iyl",
    "cf", "nf", "pf", "hf", "zf", "sf", "yf", "xf", "halted", "line", "vline",
    "frame", "cycles", "value", "addr", "old", "vdpaddr", "vdpsecond", NULL,
};

static const char *(*g_video_text)(const char *section);

/* ---- registers by name (set / register_set) ---------------------------------- */

enum {
    R_A, R_F, R_B, R_C, R_D, R_E, R_H, R_L,
    R_AF, R_BC, R_DE, R_HL, R_IX, R_IY, R_SP, R_PC,
    R_AF2, R_BC2, R_DE2, R_HL2,
    R_IXH, R_IXL, R_IYH, R_IYL,
    R_I, R_R, R_IM, R_IFF1, R_IFF2,
    R_FLAG_S, R_FLAG_Z, R_FLAG_Y, R_FLAG_H, R_FLAG_X, R_FLAG_P, R_FLAG_N,
    R_FLAG_C
};

static const struct {
    const char *name;
    int code;
} k_regs[] = {
    { "a", R_A }, { "f", R_F }, { "b", R_B }, { "c", R_C }, { "d", R_D },
    { "e", R_E }, { "h", R_H }, { "l", R_L },
    { "af", R_AF }, { "bc", R_BC }, { "de", R_DE }, { "hl", R_HL },
    { "ix", R_IX }, { "iy", R_IY }, { "sp", R_SP }, { "pc", R_PC },
    { "af'", R_AF2 }, { "bc'", R_BC2 }, { "de'", R_DE2 }, { "hl'", R_HL2 },
    { "ixh", R_IXH }, { "ixl", R_IXL }, { "iyh", R_IYH }, { "iyl", R_IYL },
    { "i", R_I }, { "r", R_R }, { "im", R_IM }, { "iff1", R_IFF1 },
    { "iff2", R_IFF2 },
    { "sf", R_FLAG_S }, { "zf", R_FLAG_Z }, { "yf", R_FLAG_Y },
    { "hf", R_FLAG_H }, { "xf", R_FLAG_X }, { "pf", R_FLAG_P },
    { "nf", R_FLAG_N }, { "cf", R_FLAG_C },
};
#define NUM_REGS ((int)(sizeof(k_regs) / sizeof(k_regs[0])))

static void lower_copy(char *dst, int dstsz, const char *src)
{
    int idx = 0;
    while (src[idx] && idx + 1 < dstsz) {
        dst[idx] = (char)tolower((unsigned char)src[idx]);
        idx++;
    }
    dst[idx] = '\0';
}

static int reg_code(const char *name)
{
    char low[16];
    int idx;
    lower_copy(low, (int)sizeof low, name);
    for (idx = 0; idx < NUM_REGS; idx++)
        if (!strcmp(low, k_regs[idx].name))
            return k_regs[idx].code;
    return -1;
}

static unsigned flag_mask(int code)
{
    switch (code) {
    case R_FLAG_S: return 0x80;
    case R_FLAG_Z: return 0x40;
    case R_FLAG_Y: return 0x20;
    case R_FLAG_H: return 0x10;
    case R_FLAG_X: return 0x08;
    case R_FLAG_P: return 0x04;
    case R_FLAG_N: return 0x02;
    default:       return 0x01;
    }
}

static long reg_value(const adamcore_z80_regs *regs, int code)
{
    switch (code) {
    case R_A: return regs->a;
    case R_F: return regs->f;
    case R_B: return regs->b;
    case R_C: return regs->c;
    case R_D: return regs->d;
    case R_E: return regs->e;
    case R_H: return regs->h;
    case R_L: return regs->l;
    case R_AF: return (regs->a << 8) | regs->f;
    case R_BC: return (regs->b << 8) | regs->c;
    case R_DE: return (regs->d << 8) | regs->e;
    case R_HL: return (regs->h << 8) | regs->l;
    case R_IX: return regs->ix;
    case R_IY: return regs->iy;
    case R_SP: return regs->sp;
    case R_PC: return regs->pc;
    case R_AF2: return (regs->a2 << 8) | regs->f2;
    case R_BC2: return (regs->b2 << 8) | regs->c2;
    case R_DE2: return (regs->d2 << 8) | regs->e2;
    case R_HL2: return (regs->h2 << 8) | regs->l2;
    case R_IXH: return regs->ix >> 8;
    case R_IXL: return regs->ix & 0xFF;
    case R_IYH: return regs->iy >> 8;
    case R_IYL: return regs->iy & 0xFF;
    case R_I: return regs->i;
    case R_R: return regs->r;
    case R_IM: return regs->im;
    case R_IFF1: return regs->iff1;
    case R_IFF2: return regs->iff2;
    default: return (regs->f & flag_mask(code)) != 0;
    }
}

static void reg_store(adamcore_z80_regs *regs, int code, long value)
{
    const uint8_t byte = (uint8_t)value;
    const uint16_t word = (uint16_t)value;
    switch (code) {
    case R_A: regs->a = byte; break;
    case R_F: regs->f = byte; break;
    case R_B: regs->b = byte; break;
    case R_C: regs->c = byte; break;
    case R_D: regs->d = byte; break;
    case R_E: regs->e = byte; break;
    case R_H: regs->h = byte; break;
    case R_L: regs->l = byte; break;
    case R_AF: regs->a = (uint8_t)(word >> 8); regs->f = (uint8_t)word; break;
    case R_BC: regs->b = (uint8_t)(word >> 8); regs->c = (uint8_t)word; break;
    case R_DE: regs->d = (uint8_t)(word >> 8); regs->e = (uint8_t)word; break;
    case R_HL: regs->h = (uint8_t)(word >> 8); regs->l = (uint8_t)word; break;
    case R_IX: regs->ix = word; break;
    case R_IY: regs->iy = word; break;
    case R_SP: regs->sp = word; break;
    case R_PC: regs->pc = word; break;
    case R_AF2: regs->a2 = (uint8_t)(word >> 8); regs->f2 = (uint8_t)word; break;
    case R_BC2: regs->b2 = (uint8_t)(word >> 8); regs->c2 = (uint8_t)word; break;
    case R_DE2: regs->d2 = (uint8_t)(word >> 8); regs->e2 = (uint8_t)word; break;
    case R_HL2: regs->h2 = (uint8_t)(word >> 8); regs->l2 = (uint8_t)word; break;
    case R_IXH: regs->ix = (uint16_t)((regs->ix & 0x00FF) | (byte << 8)); break;
    case R_IXL: regs->ix = (uint16_t)((regs->ix & 0xFF00) | byte); break;
    case R_IYH: regs->iy = (uint16_t)((regs->iy & 0x00FF) | (byte << 8)); break;
    case R_IYL: regs->iy = (uint16_t)((regs->iy & 0xFF00) | byte); break;
    case R_I: regs->i = byte; break;
    case R_R: regs->r = byte; break;
    case R_IM: regs->im = (uint8_t)value; break;
    case R_IFF1: regs->iff1 = (uint8_t)value; break;
    case R_IFF2: regs->iff2 = (uint8_t)value; break;
    default:
        if (value) regs->f |= (uint8_t)flag_mask(code);
        else       regs->f &= (uint8_t)~flag_mask(code);
        break;
    }
}

static uint8_t expr_read(void *user, uint16_t addr)
{
    adamdebug *d = user;
    return d->ec ? adamcore_peek(d->ec, addr) : 0xFF;
}

static int expr_name(void *user, const char *name, long *out)
{
    adamdebug *d = user;
    const adamcore_z80_regs *r = &d->rs;
    char n[16];
    size_t i;
    uint16_t addr;
    int code;

    for (i = 0; name[i] && i + 1 < sizeof n; i++)
        n[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    n[i] = '\0';
    if (name[i]) n[0] = '\0'; /* too long to be a register */

    if (!strcmp(n, "value") || !strcmp(n, "addr") || !strcmp(n, "old")) {
        if (!d->in_access)
            return 0;
        *out = n[0] == 'v' ? d->acc_value : n[0] == 'a' ? d->acc_addr : d->acc_old;
        return 1;
    }
    code = n[0] ? reg_code(n) : -1;
    if (code >= 0) {
        *out = reg_value(r, code);
        return 1;
    }
    if (!strcmp(n, "halted")) {
        *out = r->halted;
        return 1;
    }
    if (!strcmp(n, "cycles")) {
        *out = (long)r->cycles;
        return 1;
    }
    if (!strcmp(n, "line") || !strcmp(n, "vline") || !strcmp(n, "frame")) {
        int line = 0;
        uint64_t frame = 0;
        if (d->ec)
            adamcore_debug_beam(d->ec, &line, &frame);
        *out = n[0] == 'f' ? (long)frame : line;
        return 1;
    }
    if (!strcmp(n, "vdpaddr") || !strcmp(n, "vdpsecond")) {
        /* the VDP's host side as it is right now: inside the bus hook that is
         * before an OUT lands, which is what a Break-on-writes condition wants */
        uint8_t vregs[8], vstatus;
        uint16_t vaddr = 0;
        adamcore_vdp_latch latch;
        memset(&latch, 0, sizeof latch);
        if (d->ec) {
            adamcore_vdp_state(d->ec, vregs, &vstatus, &vaddr);
            adamcore_vdp_latch_state(d->ec, &latch);
        }
        *out = n[3] == 'a' ? vaddr : latch.latched;
        return 1;
    }
    if (symtabs_find(&d->syms, name, &addr)) {
        *out = addr;
        return 1;
    }
    return 0;
}

/* Evaluate with mtx held. */
static int eval_locked(adamdebug *d, const char *text, long *out, char *err,
                       int errsz)
{
    adamcore *c = core_of(d);
    expr_ctx ctx;

    ctx.name = expr_name;
    ctx.read8 = expr_read;
    ctx.user = d;
    if (c) {
        adamcore_get_regs(c, &d->rs);
        d->ec = c;
    } else {
        memset(&d->rs, 0, sizeof d->rs);
        d->ec = NULL;
    }
    return expr_eval(text, &ctx, out, err, errsz);
}

int adamdebug_eval(adamdebug *d, const char *text, long *value, char *err,
                   int errsz)
{
    int rc;
    long scratch;
    if (err && errsz > 0) err[0] = '\0';
    if (!d) return -1;
    pthread_mutex_lock(&d->mtx);
    d->in_access = 0;
    if (d->stopped && d->stop.reason >= ADAMDEBUG_STOP_WATCH_READ &&
        d->stop.reason <= ADAMDEBUG_STOP_WATCH_OUT) {
        d->in_access = 1;
        d->acc_addr = d->stop.addr;
        d->acc_value = d->stop.value < 0 ? 0 : d->stop.value;
        d->acc_old = d->stop.old_value < 0 ? d->acc_value : d->stop.old_value;
    }
    rc = eval_locked(d, text, value ? value : &scratch, err, errsz);
    d->in_access = 0;
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

static int cond_true_locked(adamdebug *d, const adamdebug_breakpoint *b)
{
    long v = 0;
    char err[64];
    if (!b->condition[0])
        return 1;
    if (eval_locked(d, b->condition, &v, err, sizeof err) != 0)
        return 1; /* a condition that no longer evaluates stops, not hides */
    return v != 0;
}

/* ---- breakpoint maps -------------------------------------------------------- */

static void rebuild_locked(adamdebug *d)
{
    int i;
    unsigned a;

    memset(d->map_exec, 0, sizeof d->map_exec);
    memset(d->map_mem, 0, sizeof d->map_mem);
    memset(d->map_io, 0, sizeof d->map_io);
    d->nwatch = 0;
    for (i = 0; i < d->nbp; i++) {
        const adamdebug_breakpoint *b = &d->bp[i];
        if (!b->enabled)
            continue;
        for (a = b->start; a <= b->end; a++) {
            if (b->type & ADAMDEBUG_BP_EXEC)
                d->map_exec[a] = 1;
            if (b->type & (ADAMDEBUG_BP_READ | ADAMDEBUG_BP_WRITE))
                d->map_mem[a] |= (uint8_t)(b->type & (ADAMDEBUG_BP_READ |
                                                      ADAMDEBUG_BP_WRITE));
            if ((b->type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) && a < 256)
                d->map_io[a] |= (uint8_t)(b->type & (ADAMDEBUG_BP_IN |
                                                     ADAMDEBUG_BP_OUT));
        }
        if (b->type & (ADAMDEBUG_BP_READ | ADAMDEBUG_BP_WRITE |
                       ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT))
            d->nwatch++;
    }
    d->epoch++;
}

/* ---- the trace ring --------------------------------------------------------- */

static int want_ring(const adamdebug *d)
{
    return d->trace_on || d->history_want;
}

static void trace_record(adamdebug *d, adamcore *c, uint16_t pc)
{
    adamcore_z80_regs r;
    ring_rec rec;
    int line = 0;

    adamcore_get_regs(c, &r);
    memset(&rec, 0, sizeof rec);
    rec.pc = pc;
    rec.sp = r.sp;
    rec.af = (uint16_t)((r.a << 8) | r.f);
    rec.bc = (uint16_t)((r.b << 8) | r.c);
    rec.de = (uint16_t)((r.d << 8) | r.e);
    rec.hl = (uint16_t)((r.h << 8) | r.l);
    rec.ix = r.ix;
    rec.iy = r.iy;
    rec.af2 = (uint16_t)((r.a2 << 8) | r.f2);
    rec.bc2 = (uint16_t)((r.b2 << 8) | r.c2);
    rec.de2 = (uint16_t)((r.d2 << 8) | r.e2);
    rec.hl2 = (uint16_t)((r.h2 << 8) | r.l2);
    rec.reg_i = r.i;
    rec.reg_r = r.r;
    rec.im = r.im;
    rec.iff = (uint8_t)((r.iff1 ? 1 : 0) | (r.iff2 ? 2 : 0));
    rec.halted = r.halted;
    rec.cycles = r.cycles;
    adamcore_peek_block(c, pc, rec.bytes, 4);
    adamcore_debug_beam(c, &line, NULL);
    rec.line = (int16_t)line;

    pthread_mutex_lock(&d->trace_mtx);
    if (d->ring) {
        /* A CPU already in HALT spins on the same pc every four cycles: one
         * record with a count, not a flood that scrolls everything away. */
        if (rec.halted && d->ring_total > 0) {
            ring_rec *last =
                &d->ring[(d->ring_total - 1) & (TRACE_RING_LEN - 1)];
            if (last->halted && last->pc == pc) {
                last->repeat++;
                pthread_mutex_unlock(&d->trace_mtx);
                return;
            }
        }
        d->ring[d->ring_total & (TRACE_RING_LEN - 1)] = rec;
        d->ring_total++;
    }
    pthread_mutex_unlock(&d->trace_mtx);
}

/* ---- the hooks (emulation thread) ------------------------------------------- */

static void info_clear(adamdebug_stop_info *info, int reason)
{
    memset(info, 0, sizeof *info);
    info->reason = reason;
    info->value = info->old_value = info->from_pc = -1;
}

/* A stop returns BEFORE the instruction runs: it is not recorded as if it
 * had; the instruction is recorded when it runs after the release. */
static int exec_hook(void *ud, adamcore *c, uint16_t pc)
{
    adamdebug *d = ud;
    const int first = d->skip;

    d->skip = 0;
    d->cur_pc = pc;
    if (!first) {
        if (d->pend.active) {
            d->hook_info = d->pend.info;
            snprintf(d->hook_why, sizeof d->hook_why, "%s", d->pend.why);
            d->hook_kind = ADAMDBG_STOP_WATCH;
            d->pend.active = 0;
            return 1;
        }
        switch (d->act) {
        case ACT_OVER:
        case ACT_OUT:
        case ACT_RUNTO:
            if (pc == d->target) {
                adamcore_z80_regs r;
                if (d->act == ACT_RUNTO) {
                    info_clear(&d->hook_info, ADAMDEBUG_STOP_ONESHOT);
                    snprintf(d->hook_why, sizeof d->hook_why, "run to");
                    d->hook_kind = ADAMDBG_STOP_RUNTO;
                    return 1;
                }
                adamcore_get_regs(c, &r);
                if (d->act == ACT_OVER ? (r.sp >= d->sp_guard && !r.halted)
                                       : r.sp > d->sp_guard) {
                    info_clear(&d->hook_info, ADAMDEBUG_STOP_ONESHOT);
                    snprintf(d->hook_why, sizeof d->hook_why,
                             d->act == ACT_OVER ? "step over" : "step out");
                    d->hook_kind = ADAMDBG_STOP_STEP;
                    return 1;
                }
            }
            break;
        case ACT_SCANLINE: {
            int line = 0;
            adamcore_debug_beam(c, &line, NULL);
            if (line != d->last_line) {
                d->last_line = line;
                if (--d->count <= 0) {
                    info_clear(&d->hook_info, ADAMDEBUG_STOP_SCANLINE);
                    snprintf(d->hook_why, sizeof d->hook_why, "scanline");
                    d->hook_kind = ADAMDBG_STOP_STEP;
                    return 1;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    if (want_ring(d))
        trace_record(d, c, pc);
    return 0;
}

static void bus_hook(void *ud, int kind, uint16_t addr, uint8_t val)
{
    adamdebug *d = ud;
    int type, reason, i, io = 0;

    switch (kind) {
    case ADAMCORE_BUS_MEM_R:
        type = ADAMDEBUG_BP_READ;
        reason = ADAMDEBUG_STOP_WATCH_READ;
        if (!(d->map_mem[addr] & type)) return;
        break;
    case ADAMCORE_BUS_MEM_W:
        type = ADAMDEBUG_BP_WRITE;
        reason = ADAMDEBUG_STOP_WATCH_WRITE;
        if (!(d->map_mem[addr] & type)) return;
        break;
    case ADAMCORE_BUS_IO_R:
        type = ADAMDEBUG_BP_IN;
        reason = ADAMDEBUG_STOP_WATCH_IN;
        io = 1;
        if (!(d->map_io[addr & 0xFF] & type)) return;
        break;
    case ADAMCORE_BUS_IO_W:
        type = ADAMDEBUG_BP_OUT;
        reason = ADAMDEBUG_STOP_WATCH_OUT;
        io = 1;
        if (!(d->map_io[addr & 0xFF] & type)) return;
        break;
    default:
        return;
    }
    if (type == ADAMDEBUG_BP_READ) {
        /* the fetches of the instruction itself go through the same bus: the
         * byte just fetched is at PC-1, which is not a data read */
        adamcore_z80_regs r;
        adamcore_get_regs(core_of(d), &r);
        if ((uint16_t)(r.pc - 1) == addr)
            return;
    }
    if (io)
        addr &= 0xFF;

    pthread_mutex_lock(&d->mtx);
    {
        adamdebug_stop_info access;
        adamcore *c = core_of(d);

        info_clear(&access, reason);
        access.addr = addr;
        access.value = val;
        /* a write hook fires BEFORE the store: the old byte is still there */
        access.old_value = kind == ADAMCORE_BUS_MEM_W && c
                               ? adamcore_peek(c, addr)
                               : (kind == ADAMCORE_BUS_IO_W ? -1 : val);
        access.from_pc = d->cur_pc;
        d->acc_value = val;
        d->acc_addr = addr;
        d->acc_old = access.old_value < 0 ? val : access.old_value;
        d->in_access = 1;
        for (i = 0; i < d->nbp; i++) {
            adamdebug_breakpoint *b = &d->bp[i];
            if (!b->enabled || !(b->type & type) || addr < b->start ||
                addr > b->end || !cond_true_locked(d, b))
                continue;
            b->hits++;
            if (!d->pend.active) {
                d->pend.active = 1;
                d->pend.info = access;
                d->pend.info.bp_id = b->id;
                if (io)
                    snprintf(d->pend.why, sizeof d->pend.why,
                             "%s $%02X %s $%02X",
                             type == ADAMDEBUG_BP_IN ? "in" : "out", addr,
                             type == ADAMDEBUG_BP_IN ? "->" : "<-", val);
                else
                    snprintf(d->pend.why, sizeof d->pend.why,
                             "%s $%04X = $%02X",
                             type == ADAMDEBUG_BP_READ ? "read" : "write",
                             addr, val);
            }
        }
        d->in_access = 0;
    }
    pthread_mutex_unlock(&d->mtx);
}

/* The enabled execute breakpoints at pc whose conditions hold; the first
 * one's id, or 0. Counts a hit on each. mtx held. */
static int exec_hit_locked(adamdebug *d, uint16_t pc)
{
    int i, first = 0;

    d->acc_addr = pc;
    d->acc_value = peek(d, pc);
    d->acc_old = d->acc_value;
    d->in_access = 1;
    for (i = 0; i < d->nbp; i++) {
        adamdebug_breakpoint *b = &d->bp[i];
        if (b->enabled && (b->type & ADAMDEBUG_BP_EXEC) && pc >= b->start &&
            pc <= b->end && cond_true_locked(d, b)) {
            b->hits++;
            if (!first)
                first = b->id;
        }
    }
    d->in_access = 0;
    return first;
}

/* ---- keeping the core in step with the engine ------------------------------- */

static void update_engagement_locked(adamdebug *d)
{
    d->session->dbg_engaged = d->attached || d->core_dirty;
}

/* Emulation thread, mtx held, not inside adamcore_debug_run: make what is
 * installed in the core match what the engine wants. */
static void sync_core_locked(adamdebug *d, adamcore *c)
{
    int want_exec, want_bus;

    if (d->armed_core != c) { /* a new machine: nothing is installed in it */
        d->armed_core = c;
        d->hook_exec = d->hook_bus = 0;
        d->armed_epoch = -1;
        d->core_dirty = 0;
    }
    if (!d->attached) {
        if (d->core_dirty) {
            adamcore_bp_clear_all(c);
            adamcore_set_exec_hook(c, NULL, NULL);
            adamcore_set_bus_hook(c, NULL, NULL);
            d->hook_exec = d->hook_bus = 0;
            d->core_dirty = 0;
            d->pend.active = 0;
            pthread_cond_broadcast(&d->evcv);
        }
        update_engagement_locked(d);
        return;
    }
    if (d->armed_epoch != d->epoch) {
        unsigned a;
        adamcore_bp_clear_all(c);
        for (a = 0; a < 0x10000; a++)
            if (d->map_exec[a])
                adamcore_bp_set(c, (uint16_t)a);
        d->armed_epoch = d->epoch;
    }
    want_exec = want_ring(d) || d->nwatch > 0 ||
                (!d->stopped && (d->act == ACT_OVER || d->act == ACT_OUT ||
                                 d->act == ACT_RUNTO ||
                                 d->act == ACT_SCANLINE));
    want_bus = d->nwatch > 0;
    if (want_exec != d->hook_exec) {
        adamcore_set_exec_hook(c, want_exec ? exec_hook : NULL,
                               want_exec ? d : NULL);
        d->hook_exec = want_exec;
    }
    if (want_bus != d->hook_bus) {
        adamcore_set_bus_hook(c, want_bus ? bus_hook : NULL,
                              want_bus ? d : NULL);
        d->hook_bus = want_bus;
    }
    d->core_dirty = 1;
    update_engagement_locked(d);
}

/* Applies the engine to a freshly created core (emulator thread, from
 * session.c right after adamcore_create). An attached engine stops the new
 * machine at its first instruction. */
void adamdebug_on_core_created(adamsession *s)
{
    adamdebug *d = s->debugger;
    adamcore *c = s->core;

    if (!d || !c) return;
    pthread_mutex_lock(&d->mtx);
    d->armed_core = NULL;
    d->stopped = 0;
    d->pend.active = 0;
    d->next1 = NEXT_FIRST;
    if (d->attached)
        d->stop_req = 1;
    bump_locked(d);
    sync_core_locked(d, c);
    pthread_mutex_unlock(&d->mtx);
}

/* The emulation thread, just before it destroys its core: clear the pointer
 * under mtx so no UI-thread call is mid-flight on it. */
void adamdebug_clear_core(adamsession *s)
{
    adamdebug *d = s->debugger;

    if (!d) {
        s->core = NULL;
        return;
    }
    pthread_mutex_lock(&d->mtx);
    s->core = NULL;
    d->armed_core = NULL;
    d->core_dirty = 0;
    d->stopped = 0;
    d->pend.active = 0;
    update_engagement_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- the emulation thread's frame driver ------------------------------------ */

/* Park (mtx held; drops it around the callback). `info` is the stop in
 * words, `why` the terse wording of adamdebug_stop_reason. One critical
 * section makes stopped, the stop, its serial and the generation agree. */
static void park_locked(adamdebug *d, adamcore *c,
                        const adamdebug_stop_info *info, const char *why,
                        int kind)
{
    adamcore_z80_regs r;
    void (*cb)(void *, int, uint16_t);
    void *ud;

    adamcore_get_regs(c, &r);
    snprintf(d->reason, sizeof d->reason, "%s", why);
    d->reason_addr = r.pc;
    d->stop = *info;
    if (d->stop.reason < ADAMDEBUG_STOP_WATCH_READ ||
        d->stop.reason > ADAMDEBUG_STOP_WATCH_OUT)
        d->stop.addr = r.pc;
    d->stop.pc = r.pc;
    d->stop.serial = ++d->serial;
    d->stopped = 1;
    d->stop_req = 0;
    d->pend.active = 0;
    d->next1 = NEXT_FIRST;
    bump_locked(d);
    /* the picture as far as the beam has drawn it */
    session_publish_frame(d->session, adamcore_framebuffer(c, NULL, NULL));
    pthread_cond_broadcast(&d->evcv);
    cb = d->stop_cb;
    ud = d->stop_ud;
    if (cb) {
        pthread_mutex_unlock(&d->mtx);
        cb(ud, kind, r.pc);
        pthread_mutex_lock(&d->mtx);
    }
}

/* A stop that carries no access: pause, step, frame ... */
static void park_plain_locked(adamdebug *d, adamcore *c, int reason,
                              const char *why, int kind)
{
    adamdebug_stop_info info;

    info_clear(&info, reason);
    park_locked(d, c, &info, why, kind);
}

int adamdebug_session_frame(adamsession *s)
{
    adamdebug *d = s->debugger;
    adamcore *c = s->core;
    int changed = 0;

    if (!c)
        return 0;
    if (!d)
        return adamcore_run_frame(c);

    pthread_mutex_lock(&d->mtx);
    for (;;) {
        adamcore_run_status st;
        uint32_t n = SLICE;
        int check = 1, chg = 0;

        sync_core_locked(d, c);
        if (s->stop_flag) {
            d->stopped = 0;
            break;
        }
        if (!d->attached) { /* detached: the core is clean; the fast path */
            s->audio_mute = 0;
            pthread_mutex_unlock(&d->mtx);
            return changed | adamcore_run_frame(c);
        }
        if (d->stopped) {
            s->audio_mute = 1;
            pthread_cond_wait(&d->cv, &d->mtx);
            continue;
        }
        if (d->stop_req) {
            park_plain_locked(d, c, ADAMDEBUG_STOP_PAUSE, "stopped",
                              ADAMDBG_STOP_PAUSE);
            continue;
        }
        s->audio_mute = d->act == ACT_STEP;

        if (d->next1 == NEXT_FIRST) {
            n = 1;
            check = 0;
            d->skip = 1;
        } else if (d->next1 == NEXT_PASS) {
            n = 1;
            check = 0;
        } else if (d->act == ACT_STEP) {
            n = 1;
        }
        pthread_mutex_unlock(&d->mtx);
        st = adamcore_debug_run(c, n, check, &chg);
        d->skip = 0;
        pthread_mutex_lock(&d->mtx);
        changed |= chg;

        switch (st) {
        case ADAMCORE_RUN_STEPPED:
            d->next1 = NEXT_NONE;
            if (d->pend.active) {
                adamdebug_stop_info info = d->pend.info;
                char why[64];
                snprintf(why, sizeof why, "%s", d->pend.why);
                park_locked(d, c, &info, why, ADAMDBG_STOP_WATCH);
            } else if (d->act == ACT_STEP && --d->count <= 0) {
                park_plain_locked(d, c, ADAMDEBUG_STOP_STEP, "step",
                                  ADAMDBG_STOP_STEP);
            }
            break;
        case ADAMCORE_RUN_BREAKPOINT: {
            adamcore_z80_regs r;
            int hit;
            adamcore_get_regs(c, &r);
            hit = exec_hit_locked(d, r.pc);
            if (d->pend.active) {
                /* a watch fired in the instruction just before this boundary
                 * (the bp check comes before the exec hook): that stop first */
                adamdebug_stop_info info = d->pend.info;
                char why[64];
                snprintf(why, sizeof why, "%s", d->pend.why);
                park_locked(d, c, &info, why, ADAMDBG_STOP_WATCH);
            } else if (hit) {
                /* stopped BEFORE the instruction: it is recorded in the trace
                 * only when it runs after the release */
                adamdebug_stop_info info;
                char why[48];
                info_clear(&info, ADAMDEBUG_STOP_BREAKPOINT);
                info.bp_id = hit;
                snprintf(why, sizeof why, "breakpoint: exec $%04X", r.pc);
                park_locked(d, c, &info, why, ADAMDBG_STOP_BREAKPOINT);
            } else {
                d->next1 = NEXT_PASS;
            }
            break;
        }
        case ADAMCORE_RUN_HOOK_STOP: {
            adamdebug_stop_info info = d->hook_info;
            char why[64];
            snprintf(why, sizeof why, "%s", d->hook_why);
            park_locked(d, c, &info, why, d->hook_kind);
            break;
        }
        case ADAMCORE_RUN_FRAME_DONE:
            if (d->act == ACT_FRAME && --d->count <= 0) {
                park_plain_locked(d, c, ADAMDEBUG_STOP_FRAME, "frame",
                                  ADAMDBG_STOP_STEP);
            } else if (d->act == ACT_SCANLINE) {
                d->last_line = 0; /* the beam wrapped: that was a line */
                if (--d->count <= 0) {
                    park_plain_locked(d, c, ADAMDEBUG_STOP_SCANLINE, "scanline",
                                      ADAMDBG_STOP_STEP);
                    break;
                }
                goto yield;
            } else if (d->act != ACT_STEP) {
                goto yield;
            }
            break;
        }
        continue;

    yield: /* a frame boundary: back to the paced loop */
        s->audio_mute = 0;
        pthread_mutex_unlock(&d->mtx);
        return changed;
    }
    s->audio_mute = 0;
    pthread_mutex_unlock(&d->mtx);
    return changed;
}

void adamdebug_resume_for_stop(adamdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    pthread_cond_broadcast(&d->cv);
    pthread_cond_broadcast(&d->evcv);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- lifecycle -------------------------------------------------------------- */

adamdebug *adamsession_debugger(adamsession *s)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->debugger) {
        adamdebug *d = calloc(1, sizeof(*d));
        if (d) {
            d->session = s;
            pthread_mutex_init(&d->mtx, NULL);
            pthread_cond_init(&d->cv, NULL);
            adam_cond_init_monotonic(&d->evcv);
            pthread_mutex_init(&d->trace_mtx, NULL);
            pthread_mutex_init(&d->fuji_mtx, NULL);
            d->next_id = 1;
            d->reason_addr = -1;
            d->fuji = calloc(1, sizeof(*d->fuji));
            if (d->fuji)
                adamfuji_init(d->fuji);
            symtabs_load_text(&d->syms, adamdebug_builtin_os7_sym, "os7");
            symtabs_load_text(&d->syms, adamdebug_builtin_eos_sym, "eos");
            s->debugger = d;
            adamvid_tab_register_console(d); /* the console `video` command */
        }
    }
    pthread_mutex_unlock(&s->lifecycle_mtx);
    return s->debugger;
}

void adamdebug_destroy(adamdebug *d)
{
    if (!d) return;
    symtabs_free(&d->syms);
    free(d->ring);
    pthread_mutex_destroy(&d->mtx);
    pthread_cond_destroy(&d->cv);
    pthread_cond_destroy(&d->evcv);
    pthread_mutex_destroy(&d->trace_mtx);
    pthread_mutex_destroy(&d->fuji_mtx);
    free(d->fuji);
    free(d);
}

void adamdebug_attach(adamdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->attached) {
        d->attached = 1;
        d->stop_req = 1;
        d->next1 = NEXT_FIRST;
        d->session->dbg_engaged = 1;
        pthread_cond_broadcast(&d->cv);
    }
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_detach(adamdebug *d)
{
    int waited = 0;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->attached = 0;
    d->stop_req = 0;
    d->stopped = 0;
    d->pend.active = 0;
    update_engagement_locked(d);
    pthread_cond_broadcast(&d->cv);
    bump_locked(d);
    /* the emulation thread takes everything out of the core: wait for it */
    while (d->core_dirty && d->session->running && waited < 400) {
        adam_cond_timedwait_ms(&d->evcv, &d->mtx, 5);
        waited += 5;
    }
    pthread_mutex_unlock(&d->mtx);
}

int adamdebug_is_attached(adamdebug *d)
{
    int a;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    a = d->attached;
    pthread_mutex_unlock(&d->mtx);
    return a;
}

int adamdebug_is_stopped(adamdebug *d)
{
    int s;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    s = d->stopped;
    pthread_mutex_unlock(&d->mtx);
    return s;
}

void adamdebug_stop(adamdebug *d)
{
    if (!d) return;
    if (!adamdebug_is_attached(d)) {
        adamdebug_attach(d);
        return;
    }
    pthread_mutex_lock(&d->mtx);
    if (!d->stopped)
        d->stop_req = 1;
    pthread_mutex_unlock(&d->mtx);
}

/* Leave the parked state with an action (mtx held, stopped). */
static void release_locked(adamdebug *d, int act)
{
    d->act = act;
    d->stopped = 0;
    d->stop_req = 0;
    bump_locked(d);
    pthread_cond_broadcast(&d->cv);
}

void adamdebug_resume(adamdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped)
        release_locked(d, ACT_RUN);
    else if (d->stop_req) /* asked to stop, not parked yet: never mind */
        d->stop_req = 0;
    pthread_mutex_unlock(&d->mtx);
}

int adamdebug_stop_reason(adamdebug *d, char *dst, int dstsz, int *address)
{
    int n;
    if (!d || !dst || dstsz <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    n = snprintf(dst, (size_t)dstsz, "%s", d->stopped ? d->reason : "running");
    if (address)
        *address = d->stopped ? d->reason_addr : -1;
    pthread_mutex_unlock(&d->mtx);
    return n < dstsz ? n : dstsz - 1;
}

unsigned adamdebug_generation(adamdebug *d)
{
    unsigned g;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    g = d->generation;
    pthread_mutex_unlock(&d->mtx);
    return g;
}

int adamdebug_wait_stop(adamdebug *d, unsigned gen, int ms)
{
    int waited = 0, ok = 0;
    pthread_mutex_lock(&d->mtx);
    while (waited < ms) {
        if (d->stopped && d->generation != gen) {
            ok = 1;
            break;
        }
        adam_cond_timedwait_ms(&d->evcv, &d->mtx, 5);
        waited += 5;
    }
    pthread_mutex_unlock(&d->mtx);
    return ok;
}

void adamdebug_set_stop_callback(adamdebug *d,
                                 void (*cb)(void *, int, uint16_t), void *ud)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->stop_cb = cb;
    d->stop_ud = ud;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- stepping ---------------------------------------------------------------- */

static int decode_at(adamdebug *d, uint16_t pc, z80d_insn *ins)
{
    uint8_t code[4];
    adamcore *c = core_of(d);
    if (c)
        adamcore_peek_block(c, pc, code, 4);
    else
        memset(code, 0, sizeof code);
    return z80_disassemble(ins, pc, code);
}

void adamdebug_step_count(adamdebug *d, long n)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped && d->attached) {
        d->count = n > 0 ? n : 1;
        release_locked(d, ACT_STEP);
    }
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_step(adamdebug *d)
{
    adamdebug_step_count(d, 1);
}

void adamdebug_step_over(adamdebug *d)
{
    adamcore *c;
    adamcore_z80_regs r;
    z80d_insn ins;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (!c || !d->stopped || !d->attached) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    adamcore_get_regs(c, &r);
    decode_at(d, r.pc, &ins);
    if ((ins.flags & (ADAMDASM_CALL | ADAMDASM_BLOCK | ADAMDASM_HALT)) ||
        ((ins.flags & ADAMDASM_RELATIVE) && (ins.flags & ADAMDASM_COND) &&
         ins.bytes[0] == 0x10)) {
        /* CALL/RST, the block repeats, HALT and DJNZ: run to the next
         * instruction with the stack back at (or above) this level, which
         * survives recursion and an NMI that fires on the way */
        d->target = (uint16_t)(r.pc + ins.len);
        d->sp_guard = r.sp;
        release_locked(d, ACT_OVER);
    } else {
        d->count = 1;
        release_locked(d, ACT_STEP);
    }
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_step_out(adamdebug *d)
{
    adamcore *c;
    adamcore_z80_regs r;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (c && d->stopped && d->attached) {
        /* to the return address the CALL pushed, with an SP guard;
         * approximate: a routine that juggles its own return address
         * defeats it, and then the machine runs on rather than stopping
         * wrong */
        adamcore_get_regs(c, &r);
        d->target = (uint16_t)(adamcore_peek(c, r.sp) |
                               (adamcore_peek(c, (uint16_t)(r.sp + 1)) << 8));
        d->sp_guard = r.sp;
        release_locked(d, ACT_OUT);
    }
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_scanline(adamdebug *d, int n)
{
    adamcore *c;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (c && d->stopped && d->attached) {
        adamcore_debug_beam(c, &d->last_line, NULL);
        d->count = n > 0 ? n : 1;
        release_locked(d, ACT_SCANLINE);
    }
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_frame(adamdebug *d, int n)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped && d->attached) {
        d->count = n > 0 ? n : 1;
        release_locked(d, ACT_FRAME);
    }
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_run_to(adamdebug *d, uint16_t addr)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    if (d->stopped && d->attached) {
        d->target = addr;
        release_locked(d, ACT_RUNTO);
    }
    pthread_mutex_unlock(&d->mtx);
}

/* ---- CPU ---------------------------------------------------------------------- */

void adamdebug_cpu_get(adamdebug *d, adamdebug_cpu *o)
{
    adamcore *c;
    adamcore_z80_regs r;
    uint64_t frame = 0;
    int line = 0;

    memset(o, 0, sizeof *o);
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (c) {
        adamcore_get_regs(c, &r);
        adamcore_debug_beam(c, &line, &frame);
    }
    pthread_mutex_unlock(&d->mtx);
    if (!c)
        return;
    o->pc = r.pc;
    o->sp = r.sp;
    o->af = (r.a << 8) | r.f;
    o->bc = (r.b << 8) | r.c;
    o->de = (r.d << 8) | r.e;
    o->hl = (r.h << 8) | r.l;
    o->ix = r.ix;
    o->iy = r.iy;
    o->wz = r.wz;
    o->af2 = (r.a2 << 8) | r.f2;
    o->bc2 = (r.b2 << 8) | r.c2;
    o->de2 = (r.d2 << 8) | r.e2;
    o->hl2 = (r.h2 << 8) | r.l2;
    o->i = r.i;
    o->r = r.r;
    o->im = r.im;
    o->iff1 = r.iff1;
    o->iff2 = r.iff2;
    o->halted = r.halted;
    o->sf = (r.f >> 7) & 1;
    o->zf = (r.f >> 6) & 1;
    o->yf = (r.f >> 5) & 1;
    o->hf = (r.f >> 4) & 1;
    o->xf = (r.f >> 3) & 1;
    o->pf = (r.f >> 2) & 1;
    o->nf = (r.f >> 1) & 1;
    o->cf = r.f & 1;
    o->cycles = r.cycles;
    o->line = line;
    o->frame = (uint32_t)frame;
}

static void set_flag(uint8_t *f, int bit, int on)
{
    if (on)
        *f = (uint8_t)(*f | (1u << bit));
    else
        *f = (uint8_t)(*f & ~(1u << bit));
}

void adamdebug_cpu_set(adamdebug *d, int reg, int v)
{
    adamcore *c;
    adamcore_z80_regs r;

    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (!c || !d->stopped) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    adamcore_get_regs(c, &r);
    switch (reg) {
    case ADAM_REG_PC:
        r.pc = (uint16_t)v;
        d->reason_addr = v & 0xFFFF;
        d->stop.pc = (uint16_t)v;
        break;
    case ADAM_REG_SP:  r.sp = (uint16_t)v; break;
    case ADAM_REG_AF:  r.a = (uint8_t)(v >> 8); r.f = (uint8_t)v; break;
    case ADAM_REG_BC:  r.b = (uint8_t)(v >> 8); r.c = (uint8_t)v; break;
    case ADAM_REG_DE:  r.d = (uint8_t)(v >> 8); r.e = (uint8_t)v; break;
    case ADAM_REG_HL:  r.h = (uint8_t)(v >> 8); r.l = (uint8_t)v; break;
    case ADAM_REG_IX:  r.ix = (uint16_t)v; break;
    case ADAM_REG_IY:  r.iy = (uint16_t)v; break;
    case ADAM_REG_AF2: r.a2 = (uint8_t)(v >> 8); r.f2 = (uint8_t)v; break;
    case ADAM_REG_BC2: r.b2 = (uint8_t)(v >> 8); r.c2 = (uint8_t)v; break;
    case ADAM_REG_DE2: r.d2 = (uint8_t)(v >> 8); r.e2 = (uint8_t)v; break;
    case ADAM_REG_HL2: r.h2 = (uint8_t)(v >> 8); r.l2 = (uint8_t)v; break;
    case ADAM_REG_I:   r.i = (uint8_t)v; break;
    case ADAM_REG_R:   r.r = (uint8_t)v; break;
    case ADAM_REG_IM:  r.im = (uint8_t)((v & 3) == 3 ? 2 : (v & 3)); break;
    case ADAM_REG_IFF1: r.iff1 = v != 0; break;
    case ADAM_REG_IFF2: r.iff2 = v != 0; break;
    case ADAM_REG_A:   r.a = (uint8_t)v; break;
    case ADAM_REG_F:   r.f = (uint8_t)v; break;
    case ADAM_REG_B:   r.b = (uint8_t)v; break;
    case ADAM_REG_C:   r.c = (uint8_t)v; break;
    case ADAM_REG_D:   r.d = (uint8_t)v; break;
    case ADAM_REG_E:   r.e = (uint8_t)v; break;
    case ADAM_REG_H:   r.h = (uint8_t)v; break;
    case ADAM_REG_L:   r.l = (uint8_t)v; break;
    case ADAM_FLAG_S:  set_flag(&r.f, 7, v); break;
    case ADAM_FLAG_Z:  set_flag(&r.f, 6, v); break;
    case ADAM_FLAG_H:  set_flag(&r.f, 4, v); break;
    case ADAM_FLAG_PV: set_flag(&r.f, 2, v); break;
    case ADAM_FLAG_N:  set_flag(&r.f, 1, v); break;
    case ADAM_FLAG_C:  set_flag(&r.f, 0, v); break;
    default:
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    adamcore_set_regs(c, &r);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- memory ------------------------------------------------------------------- */

int adamdebug_read(adamdebug *d, uint16_t addr, uint8_t *dst, int n)
{
    adamcore *c;
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (c)
        adamcore_peek_block(c, addr, dst, (uint32_t)n);
    pthread_mutex_unlock(&d->mtx);
    if (!c) {
        memset(dst, 0, (size_t)n);
        return 0;
    }
    return n;
}

void adamdebug_write(adamdebug *d, uint16_t addr, uint8_t value)
{
    adamcore *c;
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (c) {
        adamcore_poke(c, addr, value);
        bump_locked(d);
    }
    pthread_mutex_unlock(&d->mtx);
}

static void memmap_locked(adamdebug *d, adamdebug_memmap *o)
{
    static const char *const lower[4] = { "SmartWriter ROM", "RAM",
                                          "expansion RAM", "OS7 ROM + RAM" };
    static const char *const upper[4] = { "RAM", "expansion ROM (empty)",
                                          "expansion RAM", "cartridge" };
    adamcore *c = core_of(d);
    uint8_t mem = 0, net = 0;
    int game = 0;

    memset(o, 0, sizeof *o);
    if (c)
        adamcore_get_machine_state(c, &mem, &net, &game);
    o->mem_ctrl = mem;
    o->net_ctrl = net;
    o->lower = mem & 3;
    o->upper = (mem >> 2) & 3;
    o->lower_name = lower[o->lower];
    o->upper_name = upper[o->upper];
    o->eos_rom = (net & 2) != 0;
    o->net_reset = net & 1;
    o->game_mode = game;
    o->cv_console = d->session->opts.machine != 0;
}

void adamdebug_memmap_get(adamdebug *d, adamdebug_memmap *o)
{
    pthread_mutex_lock(&d->mtx);
    memmap_locked(d, o);
    pthread_mutex_unlock(&d->mtx);
}

/* The Z80's I/O decode (adamcore machine.c io_read/io_write): the top three
 * address bits pick the device. */
static const char *port_name(uint8_t port, int is_in)
{
    switch (port & 0xE0) {
    case 0x20: return "NET_CTRL";
    case 0x40:
        switch (port) {
        case 0x50: return "SGM AY address";
        case 0x51: return "SGM AY data";
        case 0x52: return "SGM AY read";
        case 0x53: return "SGM RAM enable";
        default: return "SGM area";
        }
    case 0x60: return "MEM_CTRL";
    case 0x80: return "strobe: keypad";
    case 0xA0: return (port & 1) ? "VDP control/status" : "VDP data";
    case 0xC0: return "strobe: joystick";
    default:   return is_in ? ((port & 2) ? "controller 2" : "controller 1")
                            : "PSG (SN76489)";
    }
}

/* ---- disassembly ------------------------------------------------------------- */

static void label_for_locked(adamdebug *d, uint16_t addr, char *dst, int dstsz,
                             int with_offset)
{
    const char *l = symtabs_exact(&d->syms, addr);
    dst[0] = '\0';
    if (l) {
        snprintf(dst, (size_t)dstsz, "%s", l);
    } else if (with_offset) {
        uint16_t off = 0;
        l = symtabs_at(&d->syms, addr, &off);
        if (l && off > 0 && off <= 16)
            snprintf(dst, (size_t)dstsz, "%s+$%X", l, off);
    }
}

static void fill_line_locked(adamdebug *d, uint16_t addr, adamdebug_line *l,
                             uint16_t pc)
{
    z80d_insn ins;
    int i, o = 0;

    memset(l, 0, sizeof *l);
    decode_at(d, addr, &ins);
    l->address = addr;
    l->length = ins.len;
    l->is_pc = addr == pc;
    l->target = ins.target;
    l->flags = ins.flags;
    for (i = 0; i < d->nbp; i++) {
        const adamdebug_breakpoint *b = &d->bp[i];
        if ((b->type & ADAMDEBUG_BP_EXEC) && addr >= b->start &&
            addr <= b->end) {
            l->has_breakpoint = 1;
            if (b->enabled)
                l->breakpoint_enabled = 1;
        }
    }
    for (i = 0; i < ins.len && o < (int)sizeof l->bytes - 3; i++)
        o += snprintf(l->bytes + o, sizeof l->bytes - (size_t)o, "%s%02X",
                      i ? " " : "", ins.bytes[i]);
    label_for_locked(d, addr, l->label, sizeof l->label, 0);
    snprintf(l->disasm, sizeof l->disasm, "%s", ins.text);

    if (ins.flags & (ADAMDASM_JUMP | ADAMDASM_CALL)) {
        label_for_locked(d, ins.target, l->comment, sizeof l->comment, 1);
    } else if ((ins.bytes[0] == 0xDB || ins.bytes[0] == 0xD3) && ins.len == 2) {
        /* IN A,(n) / OUT (n),A: the port's name */
        snprintf(l->comment, sizeof l->comment, "%s",
                 port_name(ins.bytes[1], ins.bytes[0] == 0xDB));
    } else {
        /* an absolute operand that names a place: LD (nn),A, LD HL,nn ... */
        const char *p = strchr(ins.text, '$');
        if (p && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) &&
            isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
            unsigned v;
            if (sscanf(p + 1, "%4x", &v) == 1)
                label_for_locked(d, (uint16_t)v, l->comment,
                                 sizeof l->comment, 0);
        }
    }
}

int adamdebug_disassemble(adamdebug *d, uint16_t addr, adamdebug_line *out,
                          int max, int *pc_line)
{
    adamcore *c;
    adamcore_z80_regs r;
    int n = 0;

    if (pc_line) *pc_line = -1;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (!c) {
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    adamcore_get_regs(c, &r);
    while (n < max) {
        fill_line_locked(d, addr, &out[n], r.pc);
        if (out[n].is_pc && pc_line && *pc_line < 0)
            *pc_line = n;
        addr = (uint16_t)(addr + out[n].length);
        n++;
    }
    pthread_mutex_unlock(&d->mtx);
    return n;
}

static int row_address_locked(adamdebug *d, uint16_t addr, int rows)
{
    z80d_insn ins;
    int back;

    if (!core_of(d) || rows == 0)
        return addr;
    if (rows > 0) {
        int i;
        for (i = 0; i < rows; i++)
            addr = (uint16_t)(addr + decode_at(d, addr, &ins));
        return addr;
    }
    /* Backwards: the longest run of instructions that lands exactly on addr,
     * from as far back as the rows could take. */
    rows = -rows;
    for (back = rows * 4; back >= 1; back--) {
        uint16_t starts[256];
        int count = 0;
        uint16_t a = (uint16_t)(addr - back);
        while (count < 256) {
            int dist = (uint16_t)(addr - a);
            if (dist == 0 || dist > back)
                break;
            starts[count++] = a;
            a = (uint16_t)(a + decode_at(d, a, &ins));
        }
        if (a == addr && count >= rows)
            return starts[count - rows];
    }
    return (uint16_t)(addr - rows);
}

int adamdebug_row_address(adamdebug *d, uint16_t addr, int rows)
{
    int a;
    if (!d) return addr;
    pthread_mutex_lock(&d->mtx);
    a = row_address_locked(d, addr, rows);
    pthread_mutex_unlock(&d->mtx);
    return a;
}

/* ---- symbols and labels ------------------------------------------------------ */

int adamdebug_symbols_load(adamdebug *d, const char *path, const char *table)
{
    int rc;
    pthread_mutex_lock(&d->mtx);
    rc = symtabs_load_file(&d->syms, path, table);
    d->sym_gen++;
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

const char *adamdebug_symbol_at(adamdebug *d, uint16_t addr, uint16_t *offset)
{
    return symtabs_at(&d->syms, addr, offset);
}

int adamdebug_symbol_find(adamdebug *d, const char *name, uint16_t *addr)
{
    return symtabs_find(&d->syms, name, addr);
}

int adamdebug_load_symbols(adamdebug *d, const char *path, char *msg,
                           int msgsz)
{
    char cand[ADAM_PATH_MAX + 8];
    int n;

    if (!d) return -1;
    if (msg && msgsz > 0) msg[0] = '\0';
    if (!path || !*path) {
        static const char *const exts[] = { ".sym", ".map", ".noi", NULL };
        const char *cart = d->session->cart_path;
        const char *dot, *slash;
        size_t stem;
        int i;

        if (!cart[0]) {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz,
                         "No cartridge file is open; choose a symbol file.");
            return -1;
        }
        dot = strrchr(cart, '.');
        slash = strrchr(cart, '/');
#ifdef _WIN32
        {
            const char *bs = strrchr(cart, '\\');
            if (bs && (!slash || bs > slash)) slash = bs;
        }
#endif
        stem = (dot && (!slash || dot > slash)) ? (size_t)(dot - cart)
                                                : strlen(cart);
        path = NULL;
        for (i = 0; exts[i]; i++) {
            FILE *f;
            snprintf(cand, sizeof cand, "%.*s%s", (int)stem, cart, exts[i]);
            f = fopen(cand, "r");
            if (f) {
                fclose(f);
                path = cand;
                break;
            }
        }
        if (!path) {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz,
                         "No .sym, .map or .noi next to %s", cart);
            return -1;
        }
    }
    pthread_mutex_lock(&d->mtx);
    n = symtabs_load_file(&d->syms, path, "file");
    d->sym_gen++;
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    if (msg && msgsz > 0) {
        if (n < 0)
            snprintf(msg, (size_t)msgsz, "Cannot read %s", path);
        else
            snprintf(msg, (size_t)msgsz, "%d label%s from %s", n,
                     n == 1 ? "" : "s", path);
    }
    return n;
}

int adamdebug_label_address(adamdebug *d, const char *label)
{
    uint16_t a;
    int rc;
    if (!d || !label) return -1;
    pthread_mutex_lock(&d->mtx);
    rc = symtabs_find(&d->syms, label, &a) ? (int)a : -1;
    pthread_mutex_unlock(&d->mtx);
    return rc;
}

int adamdebug_address_label(adamdebug *d, uint16_t addr, char *dst, int dstsz)
{
    int n;
    if (!d || !dst || dstsz <= 0) return 0;
    pthread_mutex_lock(&d->mtx);
    label_for_locked(d, addr, dst, dstsz, 0);
    pthread_mutex_unlock(&d->mtx);
    n = (int)strlen(dst);
    return n;
}

int adamdebug_set_label(adamdebug *d, uint16_t addr, const char *label)
{
    if (!d) return -1;
    pthread_mutex_lock(&d->mtx);
    symtabs_set_user(&d->syms, addr, label);
    d->sym_gen++;
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return 0;
}

int adamdebug_label_completions(adamdebug *d, const char *word, char *dst,
                                int dstsz, int maxn)
{
    size_t wl = strlen(word);
    const char *name;
    int i, count = 0, n = 0;

    if (!wl || dstsz <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&d->mtx);
    for (i = 0; (name = symtabs_name_at(&d->syms, i)) != NULL && count < maxn;
         i++) {
        size_t k;
        for (k = 0; k < wl && name[k]; k++) {
            char a = name[k], b = word[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (k == wl) {
            int w = snprintf(dst + n, (size_t)(dstsz - n), "%s\n", name);
            if (w < 0 || w >= dstsz - n) break;
            n += w;
            count++;
        }
    }
    pthread_mutex_unlock(&d->mtx);
    return count;
}

/* ---- breakpoints ------------------------------------------------------------- */

static void set_err(char *err, int errsz, const char *msg)
{
    if (err && errsz > 0)
        snprintf(err, (size_t)errsz, "%s", msg);
}

int adamdebug_breakpoint_add(adamdebug *d, int type, uint16_t start,
                             uint16_t end, const char *cond, char *err,
                             int errsz)
{
    adamdebug_breakpoint *b;
    int id;

    if (err && errsz > 0) err[0] = '\0';
    if (!d) return -1;
    if (!(type & 31) || (type & ~31)) {
        set_err(err, errsz, "no such breakpoint type");
        return -1;
    }
    if (end < start) {
        uint16_t t = start;
        start = end;
        end = t;
    }
    if ((type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) &&
        (type & ~(ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) == 0 && end > 255) {
        set_err(err, errsz, "ports are 0-255");
        return -1;
    }
    pthread_mutex_lock(&d->mtx);
    if (cond && *cond) {
        long v;
        char why[96];
        d->in_access = 1;
        d->acc_value = d->acc_addr = d->acc_old = 0;
        if (eval_locked(d, cond, &v, why, sizeof why) != 0 &&
            strcmp(why, "division by zero") != 0) {
            d->in_access = 0;
            pthread_mutex_unlock(&d->mtx);
            set_err(err, errsz, why);
            return -1;
        }
        d->in_access = 0;
    }
    if (d->nbp >= MAX_BP) {
        pthread_mutex_unlock(&d->mtx);
        set_err(err, errsz, "too many breakpoints");
        return -1;
    }
    b = &d->bp[d->nbp++];
    memset(b, 0, sizeof *b);
    b->id = id = d->next_id++;
    b->type = type;
    b->start = start;
    b->end = end;
    b->enabled = 1;
    snprintf(b->condition, sizeof b->condition, "%s", cond ? cond : "");
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
    return id;
}

void adamdebug_breakpoint_remove(adamdebug *d, int id)
{
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++) {
        if (d->bp[i].id == id) {
            memmove(&d->bp[i], &d->bp[i + 1],
                    (size_t)(d->nbp - i - 1) * sizeof d->bp[0]);
            d->nbp--;
            break;
        }
    }
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_breakpoint_enable(adamdebug *d, int id, int enabled)
{
    int i;
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if (d->bp[i].id == id)
            d->bp[i].enabled = enabled != 0;
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int adamdebug_breakpoint_list(adamdebug *d, adamdebug_breakpoint *out, int max)
{
    int n;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    n = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bp, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&d->mtx);
    return n;
}

void adamdebug_breakpoint_clear(adamdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->nbp = 0;
    rebuild_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int adamdebug_breakpoint_set_condition(adamdebug *dbg, int bp_id,
                                       const char *condition, char *err,
                                       int errsz)
{
    int idx, applied = 0;

    if (err && errsz > 0) err[0] = '\0';
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = 0; idx < dbg->nbp && dbg->bp[idx].id != bp_id; idx++) {}
    if (idx == dbg->nbp) {
        set_err(err, errsz, "no such breakpoint");
    } else {
        long val;
        char why[96] = "";
        if (condition && *condition) {
            /* tried once now, so a typo is refused instead of sitting silent */
            dbg->in_access = 1;
            dbg->acc_value = dbg->acc_addr = dbg->acc_old = 0;
            if (eval_locked(dbg, condition, &val, why, sizeof why) != 0 &&
                strcmp(why, "division by zero") != 0) {
                set_err(err, errsz, why);
                dbg->in_access = 0;
                pthread_mutex_unlock(&dbg->mtx);
                return 0;
            }
            dbg->in_access = 0;
        }
        snprintf(dbg->bp[idx].condition, sizeof dbg->bp[idx].condition, "%s",
                 condition ? condition : "");
        bump_locked(dbg);
        applied = 1;
    }
    pthread_mutex_unlock(&dbg->mtx);
    return applied;
}

void adamdebug_breakpoint_clear_hits(adamdebug *dbg)
{
    int idx;
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = 0; idx < dbg->nbp; idx++)
        dbg->bp[idx].hits = 0;
    bump_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

int adamdebug_breakpoint_check(adamdebug *d, uint16_t addr)
{
    int i, found = 0;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if ((d->bp[i].type & ADAMDEBUG_BP_EXEC) && addr >= d->bp[i].start &&
            addr <= d->bp[i].end)
            found = 1;
    pthread_mutex_unlock(&d->mtx);
    return found;
}

int adamdebug_breakpoint_toggle(adamdebug *d, uint16_t addr)
{
    int i, found = -1;
    if (!d) return 0;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->nbp; i++)
        if (d->bp[i].type == ADAMDEBUG_BP_EXEC && d->bp[i].start == addr &&
            d->bp[i].end == addr) {
            found = d->bp[i].id;
            break;
        }
    pthread_mutex_unlock(&d->mtx);
    if (found >= 0) {
        adamdebug_breakpoint_remove(d, found);
        return 0;
    }
    return adamdebug_breakpoint_add(d, ADAMDEBUG_BP_EXEC, addr, addr, NULL,
                                    NULL, 0) >= 0;
}

/* ---- trace and history -------------------------------------------------------- */

/* Trace and History share one ring: it records while either wants it. Mtx
 * held. */
static void apply_ring_locked(adamdebug *dbg)
{
    if (want_ring(dbg)) {
        pthread_mutex_lock(&dbg->trace_mtx);
        if (!dbg->ring)
            dbg->ring = calloc(TRACE_RING_LEN, sizeof(ring_rec));
        pthread_mutex_unlock(&dbg->trace_mtx);
    }
    bump_locked(dbg);
}

void adamdebug_trace_enable(adamdebug *dbg, int enable)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    dbg->trace_on = enable != 0;
    apply_ring_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

int adamdebug_trace_enabled(adamdebug *dbg)
{
    return dbg && dbg->trace_on;
}

void adamdebug_history_enable(adamdebug *dbg, int enable)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    dbg->history_want = enable != 0;
    apply_ring_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

int adamdebug_history_enabled(adamdebug *dbg)
{
    return dbg && dbg->history_want;
}

static void ring_entry(const ring_rec *rec, adamdebug_trace *out)
{
    z80d_insn insn;

    memset(out, 0, sizeof *out);
    out->pc = rec->pc;
    out->sp = rec->sp;
    out->af = rec->af;
    out->bc = rec->bc;
    out->de = rec->de;
    out->hl = rec->hl;
    out->ix = rec->ix;
    out->iy = rec->iy;
    out->af2 = rec->af2;
    out->bc2 = rec->bc2;
    out->de2 = rec->de2;
    out->hl2 = rec->hl2;
    out->reg_i = rec->reg_i;
    out->reg_r = rec->reg_r;
    out->im = rec->im;
    out->iff = rec->iff;
    out->halted = rec->halted;
    out->repeat = rec->repeat;
    out->cycles = rec->cycles;
    out->line = rec->line;
    memcpy(out->bytes, rec->bytes, 4);
    out->len = (uint8_t)z80_disassemble(&insn, rec->pc, rec->bytes);
}

/* The newest `max` records, oldest first, into out. Returns how many. */
static int ring_newest(adamdebug *dbg, adamdebug_trace *out, int max)
{
    uint64_t held, take, idx;

    if (!dbg || max <= 0) return 0;
    pthread_mutex_lock(&dbg->trace_mtx);
    if (!dbg->ring) {
        pthread_mutex_unlock(&dbg->trace_mtx);
        return 0;
    }
    held = dbg->ring_total < TRACE_RING_LEN ? dbg->ring_total : TRACE_RING_LEN;
    take = held < (uint64_t)max ? held : (uint64_t)max;
    for (idx = 0; idx < take; idx++)
        ring_entry(&dbg->ring[(dbg->ring_total - take + idx) & (TRACE_RING_LEN - 1)],
                   &out[idx]);
    pthread_mutex_unlock(&dbg->trace_mtx);
    return (int)take;
}

int adamdebug_trace_read(adamdebug *dbg, adamdebug_trace *out, int max)
{
    int count = ring_newest(dbg, out, max), idx;

    for (idx = 0; idx < count / 2; idx++) { /* newest first */
        adamdebug_trace swap = out[idx];
        out[idx] = out[count - 1 - idx];
        out[count - 1 - idx] = swap;
    }
    return count;
}

void adamdebug_trace_clear(adamdebug *dbg)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->trace_mtx);
    dbg->ring_total = 0;
    pthread_mutex_unlock(&dbg->trace_mtx);
    touch(dbg);
}

int adamdebug_trace_count(adamdebug *dbg)
{
    uint64_t held;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->trace_mtx);
    held = dbg->ring_total < TRACE_RING_LEN ? dbg->ring_total : TRACE_RING_LEN;
    pthread_mutex_unlock(&dbg->trace_mtx);
    return (int)held;
}

uint64_t adamdebug_trace_total(adamdebug *dbg)
{
    uint64_t total;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->trace_mtx);
    total = dbg->ring_total;
    pthread_mutex_unlock(&dbg->trace_mtx);
    return total;
}

int adamdebug_trace_format(const adamdebug_trace *rec, char *dst, int dstsz)
{
    z80d_insn insn;

    if (!rec || !dst || dstsz <= 0) return 0;
    z80_disassemble(&insn, rec->pc, rec->bytes);
    return snprintf(dst, (size_t)dstsz,
                    "%04X  %-18s AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X",
                    rec->pc, insn.text, rec->af, rec->bc, rec->de, rec->hl,
                    rec->sp);
}

int adamdebug_trace_match(const adamdebug_trace *rec, int filter, uint16_t low,
                          uint16_t high)
{
    z80d_insn insn;

    if (!rec) return 0;
    if (filter == ADAMDEBUG_TRACE_RANGE) {
        if (low > high) { uint16_t swap = low; low = high; high = swap; }
        return rec->pc >= low && rec->pc <= high;
    }
    if (filter == ADAMDEBUG_TRACE_BRANCHES) {
        z80_disassemble(&insn, rec->pc, rec->bytes);
        return (insn.flags & (ADAMDASM_JUMP | ADAMDASM_CALL | ADAMDASM_RET)) != 0;
    }
    return 1;
}

void adamdebug_history_clear(adamdebug *dbg)
{
    adamdebug_trace_clear(dbg);
}

uint64_t adamdebug_history_count(adamdebug *dbg)
{
    return adamdebug_trace_total(dbg);
}

int adamdebug_history_copy(adamdebug *dbg, adamhist_entry *out, int max)
{
    return ring_newest(dbg, out, max);
}

int adamdebug_history_format(const adamhist_entry *rec, char *dst, int dstsz)
{
    static const char names[] = "SZYHXPNC";
    z80d_insn insn;
    char flags[9];
    int idx, len;

    if (!rec || !dst || dstsz <= 0) return 0;
    z80_disassemble(&insn, rec->pc, rec->bytes);
    for (idx = 0; idx < 8; idx++)
        flags[idx] = ((rec->af >> (7 - idx)) & 1) ? names[idx] : '.';
    flags[8] = '\0';
    len = snprintf(dst, (size_t)dstsz,
                   "%04X  %-18s AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X "
                   "IY=%04X SP=%04X  %s%s",
                   rec->pc, insn.text, rec->af, rec->bc, rec->de, rec->hl,
                   rec->ix, rec->iy, rec->sp, flags,
                   rec->halted ? " HALT" : "");
    if (len > 0 && len < dstsz && rec->repeat)
        len += snprintf(dst + len, (size_t)(dstsz - len), " x%u",
                        (unsigned)rec->repeat + 1);
    return len < dstsz ? len : dstsz - 1;
}

int adamdebug_history_match(const adamhist_entry *rec, int filter,
                            uint16_t low, uint16_t high)
{
    return adamdebug_trace_match(rec, filter, low, high);
}

/* ---- files -------------------------------------------------------------------- */

static int write_bytes(const char *path, const uint8_t *data, size_t n,
                       char *msg, int msgsz)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n) {
        if (f) fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
        return -1;
    }
    fclose(f);
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "Saved %zu bytes to %s", n, path);
    return 0;
}

static void not_running(char *msg, int msgsz)
{
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz, "The machine is not running.");
}

int adamdebug_save(adamdebug *d, const char *kind, const char *path, char *msg,
                   int msgsz)
{
    adamcore *c;
    FILE *f;

    if (!d || !kind || !path) {
        not_running(msg, msgsz);
        return -1;
    }
    if (!strcmp(kind, "mem") || !strcmp(kind, "vram")) {
        const int mem = kind[0] == 'm';
        size_t n = mem ? 0x10000 : 0x4000;
        uint8_t *buf = malloc(0x10000);
        int rc;

        if (!buf) return -1;
        pthread_mutex_lock(&d->mtx);
        c = core_of(d);
        if (c) {
            if (mem)
                adamcore_peek_block(c, 0, buf, 0x10000);
            else
                memcpy(buf, adamcore_vdp_vram(c), 0x4000);
        }
        pthread_mutex_unlock(&d->mtx);
        if (!c) {
            free(buf);
            not_running(msg, msgsz);
            return -1;
        }
        rc = write_bytes(path, buf, n, msg, msgsz);
        free(buf);
        return rc;
    }
    if (!strcmp(kind, "dis")) {
        adamcore_z80_regs r;
        uint32_t a = 0;

        pthread_mutex_lock(&d->mtx);
        c = core_of(d);
        f = c ? fopen(path, "w") : NULL;
        if (f) {
            adamcore_get_regs(c, &r);
            while (a < 0x10000) {
                adamdebug_line l;
                fill_line_locked(d, (uint16_t)a, &l, r.pc);
                if (l.label[0])
                    fprintf(f, "%s:\n", l.label);
                fprintf(f, "  %04X  %-12s %-24s%s%s\n", l.address, l.bytes,
                        l.disasm, l.comment[0] ? "; " : "", l.comment);
                a += (uint32_t)l.length;
            }
            fclose(f);
        }
        pthread_mutex_unlock(&d->mtx);
        if (!c) {
            not_running(msg, msgsz);
            return -1;
        }
        if (!f) {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
            return -1;
        }
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (!strcmp(kind, "regs")) {
        adamdebug_cpu cp;
        uint8_t regs[8] = { 0 }, status = 0;
        uint16_t vaddr = 0;
        int i;

        adamdebug_cpu_get(d, &cp);
        pthread_mutex_lock(&d->mtx);
        c = core_of(d);
        if (c)
            adamcore_vdp_state(c, regs, &status, &vaddr);
        pthread_mutex_unlock(&d->mtx);
        if (!c) {
            not_running(msg, msgsz);
            return -1;
        }
        f = fopen(path, "w");
        if (!f) {
            if (msg && msgsz > 0)
                snprintf(msg, (size_t)msgsz, "Cannot write %s", path);
            return -1;
        }
        fprintf(f, "PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X "
                   "IX=%04X IY=%04X\n",
                cp.pc, cp.sp, cp.af, cp.bc, cp.de, cp.hl, cp.ix, cp.iy);
        fprintf(f, "AF'=%04X BC'=%04X DE'=%04X HL'=%04X I=%02X R=%02X "
                   "IM=%d IFF1=%d IFF2=%d\n",
                cp.af2, cp.bc2, cp.de2, cp.hl2, cp.i, cp.r, cp.im, cp.iff1,
                cp.iff2);
        fprintf(f, "cycles=%llu frame=%u line=%d\n",
                (unsigned long long)cp.cycles, cp.frame, cp.line);
        for (i = 0; i < 8; i++)
            fprintf(f, "VDP R%d = %02X\n", i, regs[i]);
        fprintf(f, "VDP status=%02X addr=%04X\n", status, vaddr);
        fclose(f);
        if (msg && msgsz > 0)
            snprintf(msg, (size_t)msgsz, "Saved %s", path);
        return 0;
    }
    if (msg && msgsz > 0)
        snprintf(msg, (size_t)msgsz,
                 "Unknown save kind %s (dis, mem, vram, regs)", kind);
    return -1;
}

/* ---- VDP -------------------------------------------------------------------- */

void adamdebug_vdp_snapshot(adamdebug *d, adamvdp_snapshot *out)
{
    adamcore *c;
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (c) {
        memcpy(out->vram, adamcore_vdp_vram(c), sizeof(out->vram));
        adamcore_vdp_state(c, out->regs, &out->status, &out->addr);
        memcpy(out->palette565, adamcore_palette565(c),
               sizeof(out->palette565));
    }
    pthread_mutex_unlock(&d->mtx);
    if (!c)
        memset(out, 0, sizeof(*out));
}

int adamdebug_vdp_read(adamdebug *d, uint16_t addr, uint8_t *dst, int n)
{
    adamcore *c;
    int i;

    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (c) {
        const uint8_t *vram = adamcore_vdp_vram(c);
        for (i = 0; i < n; i++)
            dst[i] = vram[(addr + i) & 0x3FFF];
    }
    pthread_mutex_unlock(&d->mtx);
    if (!c) {
        memset(dst, 0, (size_t)n);
        return 0;
    }
    return n;
}

int adamdebug_vdp_write(adamdebug *d, uint16_t addr, const uint8_t *src, int n)
{
    adamcore *c;
    int i;

    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    if (c) {
        /* adamcore hands out VRAM read-only and has no poke entry point; the
         * cast is deliberate rather than a patch to the staged tree (which
         * cmake/StageAdamcore.cmake regenerates from upstream). Storing into
         * the array is the whole of a VRAM write -- the address register and
         * read-ahead latch belong to the port protocol, not to the RAM, so
         * leaving them alone is what keeps a transfer in flight intact. */
        uint8_t *vram = (uint8_t *)adamcore_vdp_vram(c);
        for (i = 0; i < n; i++)
            vram[(addr + i) & 0x3FFF] = src[i];
    }
    pthread_mutex_unlock(&d->mtx);
    return c ? n : 0;
}

/* ---- FujiNet ----------------------------------------------------------------- */

/* adamcore's BoIP wire trace, on the emulator thread. adamcore's dir 0 is
 * FujiNet -> ADAM. */
void adamdebug_boip_trace(void *ud, int dir, const uint8_t *buf, int len)
{
    adamdebug *d = ud;
    if (!d || !d->fuji) return;
    pthread_mutex_lock(&d->fuji_mtx);
    adamfuji_feed(d->fuji, dir == 0, buf, len);
    pthread_mutex_unlock(&d->fuji_mtx);
}

int adamdebug_fujinet_trace(adamdebug *d, adam_fuji_packet *out, int max)
{
    int n = 0;
    if (!d->fuji) return 0;
    pthread_mutex_lock(&d->fuji_mtx);
    n = adamfuji_read(d->fuji, out, max);
    pthread_mutex_unlock(&d->fuji_mtx);
    return n;
}

void adamdebug_fujinet_trace_clear(adamdebug *d)
{
    if (!d->fuji) return;
    pthread_mutex_lock(&d->fuji_mtx);
    adamfuji_clear(d->fuji);
    pthread_mutex_unlock(&d->fuji_mtx);
}

int adamdebug_fujinet_state(adamdebug *d, char *dst, int dstsz)
{
    adamsession *s = d->session;
    const char *link;
    int n;

    if (dstsz <= 0) return 0;
    if (!adamsession_fujinet_running(s) && !adamsession_fujinet_external(s))
        link = "no FujiNet runtime";
    else if (!adamsession_is_running(s))
        link = "the ADAM is off";
    else if (adamsession_fujinet_link(s))
        link = "FujiNet connected";
    else
        link = "waiting for FujiNet";
    n = snprintf(dst, (size_t)dstsz, "BoIP     %s\nLink     %s\n\n",
                 adamsession_boip_endpoint(s), link);
    if (n < 0) n = 0;
    if (n >= dstsz) return dstsz - 1;
    if (d->fuji) {
        pthread_mutex_lock(&d->fuji_mtx);
        n += adamfuji_format_state(d->fuji, dst + n, dstsz - n);
        pthread_mutex_unlock(&d->fuji_mtx);
    }
    return n;
}

/* ---- the stop, in words ------------------------------------------------------- */

static int format_stop(const adamdebug_stop_info *info, char *dst, int dstsz)
{
    char value[24] = "", was[24] = "";
    const unsigned stop_pc = info->pc;

    if (info->value >= 0)
        snprintf(value, sizeof value, " = $%02X", info->value);
    if (info->old_value >= 0 && info->reason == ADAMDEBUG_STOP_WATCH_WRITE)
        snprintf(was, sizeof was, " (was $%02X)", info->old_value);
    switch (info->reason) {
    case ADAMDEBUG_STOP_PAUSE:
        return snprintf(dst, (size_t)dstsz, "paused at $%04X", stop_pc);
    case ADAMDEBUG_STOP_STEP:
        return snprintf(dst, (size_t)dstsz, "step at $%04X", stop_pc);
    case ADAMDEBUG_STOP_FRAME:
        return snprintf(dst, (size_t)dstsz, "frame at $%04X", stop_pc);
    case ADAMDEBUG_STOP_SCANLINE:
        return snprintf(dst, (size_t)dstsz, "scanline at $%04X", stop_pc);
    case ADAMDEBUG_STOP_BREAKPOINT:
        return snprintf(dst, (size_t)dstsz, "breakpoint: exec $%04X", stop_pc);
    case ADAMDEBUG_STOP_WATCH_WRITE:
        return snprintf(dst, (size_t)dstsz, "wrote $%04X%s%s at $%04X",
                        info->addr, value, was, stop_pc);
    case ADAMDEBUG_STOP_WATCH_READ:
        return snprintf(dst, (size_t)dstsz, "read $%04X%s at $%04X",
                        info->addr, value, stop_pc);
    case ADAMDEBUG_STOP_WATCH_IN:
        return snprintf(dst, (size_t)dstsz, "in $%02X%s at $%04X",
                        info->addr & 0xFF, value, stop_pc);
    case ADAMDEBUG_STOP_WATCH_OUT:
        return snprintf(dst, (size_t)dstsz, "out $%02X%s at $%04X",
                        info->addr & 0xFF, value, stop_pc);
    default:
        return snprintf(dst, (size_t)dstsz, "stopped at $%04X", stop_pc);
    }
}

int adamdebug_stop_text(adamdebug *dbg, char *dst, int dstsz, int *address)
{
    int len;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (address) *address = -1;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    if (dbg->stopped) {
        len = format_stop(&dbg->stop, dst, dstsz);
        if (address) *address = dbg->stop.pc;
    } else {
        len = snprintf(dst, (size_t)dstsz, "running");
    }
    pthread_mutex_unlock(&dbg->mtx);
    return len < dstsz ? len : dstsz - 1;
}

int adamdebug_last_stop(adamdebug *dbg, adamdebug_stop_info *out)
{
    int stopped;
    if (!dbg || !out) return 0;
    pthread_mutex_lock(&dbg->mtx);
    *out = dbg->stop;
    stopped = dbg->stopped;
    pthread_mutex_unlock(&dbg->mtx);
    return stopped;
}

uint64_t adamdebug_stop_serial(adamdebug *dbg)
{
    uint64_t serial;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    serial = dbg->serial;
    pthread_mutex_unlock(&dbg->mtx);
    return serial;
}

int adamdebug_stop_snapshot(adamdebug *dbg, adamdebug_snapshot *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->address = -1;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    out->paused = dbg->stopped;
    out->serial = dbg->serial;
    out->stop = dbg->stop;
    if (out->paused) {
        format_stop(&dbg->stop, out->text, (int)sizeof out->text);
        out->address = dbg->stop.pc;
    } else {
        snprintf(out->text, sizeof out->text, "running");
    }
    pthread_mutex_unlock(&dbg->mtx);
    return out->paused;
}

/* ---- registers, in bulk -------------------------------------------------------- */

int adamdebug_regs_set(adamdebug *dbg, const adamcore_z80_regs *regs, char *err,
                       int errsz)
{
    adamcore *core;
    adamcore_z80_regs before, applied;

    if (err && errsz > 0) err[0] = '\0';
    if (!dbg || !regs) return 0;
    if (regs->im > 2) {
        set_err(err, errsz, "IM is 0, 1 or 2");
        return 0;
    }
    if (regs->iff1 > 1 || regs->iff2 > 1 || regs->halted > 1) {
        set_err(err, errsz, "IFF1, IFF2 and HALT are 0 or 1");
        return 0;
    }
    pthread_mutex_lock(&dbg->mtx);
    core = core_of(dbg);
    if (!core || !dbg->stopped) {
        pthread_mutex_unlock(&dbg->mtx);
        set_err(err, errsz, "the machine is running (stop it first)");
        return 0;
    }
    adamcore_get_regs(core, &before);
    applied = *regs;
    applied.cycles = before.cycles;
    adamcore_set_regs(core, &applied);
    if (applied.pc != before.pc) {
        dbg->reason_addr = applied.pc;
        dbg->stop.pc = applied.pc;
    }
    bump_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
    if (err && errsz > 0 && applied.halted && applied.pc != before.pc)
        snprintf(err, (size_t)errsz,
                 "PC changed while the CPU is halted: it stays halted until "
                 "HALT is cleared or an interrupt arrives");
    return 1;
}

int adamdebug_register_set(adamdebug *dbg, const char *name, long value,
                           char *err, int errsz)
{
    adamcore_z80_regs regs;
    adamcore *core;
    const int code = name ? reg_code(name) : -1;

    if (err && errsz > 0) err[0] = '\0';
    if (!dbg) return 0;
    if (code < 0) {
        if (err && errsz > 0)
            snprintf(err, (size_t)errsz, "unknown register '%s'",
                     name ? name : "");
        return 0;
    }
    if (code == R_IM && (value < 0 || value > 2)) {
        set_err(err, errsz, "IM is 0, 1 or 2");
        return 0;
    }
    if (code == R_IFF1 || code == R_IFF2)
        value = value != 0;
    pthread_mutex_lock(&dbg->mtx);
    core = core_of(dbg);
    if (core)
        adamcore_get_regs(core, &regs);
    pthread_mutex_unlock(&dbg->mtx);
    if (!core) {
        set_err(err, errsz, "the machine is not running");
        return 0;
    }
    reg_store(&regs, code, value);
    return adamdebug_regs_set(dbg, &regs, err, errsz);
}

/* ---- symbols, for a Symbols tab ------------------------------------------------- */

int adamdebug_symbols_count(adamdebug *dbg)
{
    int total = 0, idx;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = 0; idx < dbg->syms.count; idx++)
        total += dbg->syms.tables[idx].count;
    pthread_mutex_unlock(&dbg->mtx);
    return total;
}

/* mtx held. */
static const sym_entry *symbol_entry_locked(const adamdebug *dbg, int index)
{
    int idx;
    if (index < 0) return NULL;
    for (idx = 0; idx < dbg->syms.count; idx++) {
        const sym_table *table = &dbg->syms.tables[idx];
        if (index < table->count)
            return &table->entries[index];
        index -= table->count;
    }
    return NULL;
}

int adamdebug_symbols_get(adamdebug *dbg, int index, uint16_t *addr, char *name,
                          int namesz)
{
    const sym_entry *entry;
    int found = 0;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    entry = symbol_entry_locked(dbg, index);
    if (entry) {
        if (addr) *addr = entry->addr;
        if (name && namesz > 0)
            snprintf(name, (size_t)namesz, "%s", entry->name);
        found = 1;
    }
    pthread_mutex_unlock(&dbg->mtx);
    return found;
}

typedef struct { uint16_t addr; const char *name; int index; } sym_pick;

static int sym_pick_cmp(const void *left, const void *right)
{
    const sym_pick *lhs = left, *rhs = right;
    if (lhs->addr != rhs->addr) return lhs->addr < rhs->addr ? -1 : 1;
    return strcasecmp(lhs->name, rhs->name);
}

static int contains_nocase(const char *hay, const char *needle)
{
    const size_t len = strlen(needle);
    if (!len) return 1;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, len)) return 1;
    return 0;
}

int adamdebug_symbols_filter(adamdebug *dbg, const char *text, int *out, int max)
{
    sym_pick *pick;
    int total = 0, count = 0, idx, base = 0, table_idx;

    if (!dbg || !out || max <= 0) return 0;
    pthread_mutex_lock(&dbg->mtx);
    for (idx = 0; idx < dbg->syms.count; idx++)
        total += dbg->syms.tables[idx].count;
    pick = total > 0 ? malloc((size_t)total * sizeof *pick) : NULL;
    if (!pick) {
        pthread_mutex_unlock(&dbg->mtx);
        return 0;
    }
    for (table_idx = 0; table_idx < dbg->syms.count; table_idx++) {
        const sym_table *table = &dbg->syms.tables[table_idx];
        for (idx = 0; idx < table->count && count < total; idx++)
            if (contains_nocase(table->entries[idx].name, text ? text : "")) {
                pick[count].addr = table->entries[idx].addr;
                pick[count].name = table->entries[idx].name;
                pick[count].index = base + idx;
                count++;
            }
        base += table->count;
    }
    qsort(pick, (size_t)count, sizeof *pick, sym_pick_cmp);
    if (count > max) count = max;
    for (idx = 0; idx < count; idx++)
        out[idx] = pick[idx].index;
    pthread_mutex_unlock(&dbg->mtx);
    free(pick);
    return count;
}

unsigned adamdebug_symbols_generation(adamdebug *dbg)
{
    unsigned gen;
    if (!dbg) return 0;
    pthread_mutex_lock(&dbg->mtx);
    gen = dbg->sym_gen;
    pthread_mutex_unlock(&dbg->mtx);
    return gen;
}

void adamdebug_clear_symbols(adamdebug *dbg)
{
    if (!dbg) return;
    pthread_mutex_lock(&dbg->mtx);
    symtabs_free(&dbg->syms);
    memset(&dbg->syms, 0, sizeof dbg->syms);
    symtabs_load_text(&dbg->syms, adamdebug_builtin_os7_sym, "os7");
    symtabs_load_text(&dbg->syms, adamdebug_builtin_eos_sym, "eos");
    dbg->sym_gen++;
    bump_locked(dbg);
    pthread_mutex_unlock(&dbg->mtx);
}

const sym_tables *adamdebug_symtabs(adamdebug *dbg)
{
    return dbg ? &dbg->syms : NULL;
}

/* ---- search ------------------------------------------------------------------------ */

static int match_at(const uint8_t *mem, unsigned addr, const uint8_t *pat,
                    int count, uint8_t mask, int flags)
{
    int idx;
    for (idx = 0; idx < count; idx++) {
        uint8_t have = mem[addr + (unsigned)idx];
        uint8_t want = pat[idx];
        if (flags & ADAMDEBUG_SEARCH_NOCASE) {
            if (have >= 'a' && have <= 'z') have = (uint8_t)(have - 32);
            if (want >= 'a' && want <= 'z') want = (uint8_t)(want - 32);
        }
        if ((have & mask) != (want & mask)) return 0;
    }
    return 1;
}

/* The CPU's 64K as mapped now; NULL unless stopped. */
static uint8_t *snapshot_64k(adamdebug *dbg)
{
    uint8_t *mem;
    if (!adamdebug_is_stopped(dbg)) return NULL;
    mem = malloc(0x10000);
    if (!mem) return NULL;
    if (adamdebug_read(dbg, 0, mem, 0x10000) != 0x10000) {
        free(mem);
        return NULL;
    }
    return mem;
}

static int search_buf(const uint8_t *mem, const uint8_t *pat, int count,
                      uint8_t mask, int flags, unsigned from, unsigned last,
                      uint16_t *out, int max)
{
    unsigned addr;
    int found = 0;
    for (addr = from; addr <= last && addr + (unsigned)count - 1 <= 0xFFFF &&
                      found < max; addr++)
        if (match_at(mem, addr, pat, count, mask, flags))
            out[found++] = (uint16_t)addr;
    return found;
}

int adamdebug_search(adamdebug *dbg, const uint8_t *pat, int count, uint8_t mask,
                     int flags, uint16_t from, uint16_t last, uint16_t *out,
                     int max)
{
    uint8_t *mem;
    int found;

    if (!dbg || !pat || count <= 0 || !out || max <= 0) return -1;
    mem = snapshot_64k(dbg);
    if (!mem) return -1;
    if (from > last) { uint16_t swap = from; from = last; last = swap; }
    found = search_buf(mem, pat, count, mask, flags, from, last, out, max);
    free(mem);
    return found;
}

int adamdebug_search_next(adamdebug *dbg, const uint8_t *pat, int count,
                          uint8_t mask, int flags, uint16_t from,
                          int backward, uint16_t *found)
{
    uint8_t *mem;
    uint16_t hit = 0;
    int result = 0;

    if (!dbg || !pat || count <= 0 || !found) return -1;
    mem = snapshot_64k(dbg);
    if (!mem) return -1;
    if (!backward) {
        /* after `from` to the end, then 0..from (which includes `from`) */
        if ((unsigned)from + 1 <= 0xFFFF &&
            search_buf(mem, pat, count, mask, flags, (unsigned)from + 1,
                       0xFFFF, &hit, 1))
            result = 1;
        else if (search_buf(mem, pat, count, mask, flags, 0, from, &hit, 1))
            result = 1;
    } else {
        /* the nearest match before `from`, else the last one from `from` up */
        int pass;
        for (pass = 0; pass < 2 && !result; pass++) {
            const int start = pass == 0 ? (int)from - 1 : 0xFFFF;
            const int stop = pass == 0 ? 0 : (int)from;
            int addr;
            for (addr = start; addr >= stop; addr--)
                if (addr + count - 1 <= 0xFFFF &&
                    match_at(mem, (unsigned)addr, pat, count, mask, flags)) {
                    hit = (uint16_t)addr;
                    result = 1;
                    break;
                }
        }
    }
    free(mem);
    if (result) *found = hit;
    return result;
}

/* ---- the Video tab's console text ------------------------------------------------- */

void adamdebug_set_video_text(const char *(*func)(const char *section))
{
    g_video_text = func;
}

const char *(*adamdebug_video_text_hook(void))(const char *section)
{
    return g_video_text;
}

/* ---- for iostate.c and memtools.c -------------------------------------------------- */

void adamdebug_lock(adamdebug *dbg)
{
    pthread_mutex_lock(&dbg->mtx);
}

void adamdebug_unlock(adamdebug *dbg)
{
    pthread_mutex_unlock(&dbg->mtx);
}

adamcore *adamdebug_core_locked(adamdebug *dbg)
{
    return dbg ? core_of(dbg) : NULL;
}

adamsession *adamdebug_session_of(adamdebug *dbg)
{
    return dbg ? dbg->session : NULL;
}

int adamdebug_fuji_packets(adamdebug *dbg, uint32_t packets[2])
{
    packets[0] = packets[1] = 0;
    if (!dbg->fuji) return 0;
    pthread_mutex_lock(&dbg->fuji_mtx);
    packets[0] = dbg->fuji->packets[0];
    packets[1] = dbg->fuji->packets[1];
    pthread_mutex_unlock(&dbg->fuji_mtx);
    return 1;
}
