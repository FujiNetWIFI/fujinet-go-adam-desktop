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
 *     IN or OUT breakpoint) records the hit in `pending`, and the exec hook
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

#include "adamcore_debug.h"
#include "../src/compat.h"
#include "../src/session_internal.h"
#include "debugger_internal.h"
#include "expr.h"
#include "fujitrace.h"
#include "z80dasm.h"

#define MAX_BP  128
#define TRACE_N 4096
#define SLICE   4096 /* instructions between looks at the stop request */

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
    int pending;             /* a watchpoint fired during the instruction */
    char pend_reason[128];
    int hook_kind;
    char hook_why[128];
    int tr_last;

    /* The expression context (mtx held while evaluating). */
    adamcore_z80_regs rs;
    const adamcore *ec;
    long acc_value, acc_addr;
    int in_access;

    pthread_mutex_t trace_mtx;
    int trace_on;
    adamdebug_trace ring[TRACE_N];
    int ring_head, ring_count;

    sym_tables syms;

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
    "cf", "nf", "pf", "hf", "zf", "sf", "yf", "xf", "halted", "line", "frame",
    "cycles", "value", "addr", NULL,
};

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

    for (i = 0; name[i] && i + 1 < sizeof n; i++)
        n[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    n[i] = '\0';
    if (name[i]) n[0] = '\0'; /* too long to be a register */

    if (!strcmp(n, "value") || !strcmp(n, "addr")) {
        if (!d->in_access)
            return 0;
        *out = n[0] == 'v' ? d->acc_value : d->acc_addr;
        return 1;
    }
#define REG(s, v) if (!strcmp(n, s)) { *out = (v); return 1; }
    REG("a", r->a) REG("f", r->f) REG("b", r->b) REG("c", r->c)
    REG("d", r->d) REG("e", r->e) REG("h", r->h) REG("l", r->l)
    REG("af", (r->a << 8) | r->f) REG("bc", (r->b << 8) | r->c)
    REG("de", (r->d << 8) | r->e) REG("hl", (r->h << 8) | r->l)
    REG("ix", r->ix) REG("iy", r->iy) REG("sp", r->sp) REG("pc", r->pc)
    REG("i", r->i) REG("r", r->r) REG("im", r->im)
    REG("iff1", r->iff1) REG("iff2", r->iff2)
    REG("af'", (r->a2 << 8) | r->f2) REG("bc'", (r->b2 << 8) | r->c2)
    REG("de'", (r->d2 << 8) | r->e2) REG("hl'", (r->h2 << 8) | r->l2)
    REG("cf", r->f & 1) REG("nf", (r->f >> 1) & 1) REG("pf", (r->f >> 2) & 1)
    REG("yf", (r->f >> 5) & 1) REG("xf", (r->f >> 3) & 1)
    REG("hf", (r->f >> 4) & 1) REG("zf", (r->f >> 6) & 1)
    REG("sf", (r->f >> 7) & 1)
    REG("halted", r->halted) REG("cycles", (long)r->cycles)
#undef REG
    if (!strcmp(n, "line") || !strcmp(n, "frame")) {
        int line = 0;
        uint64_t frame = 0;
        if (d->ec)
            adamcore_debug_beam(d->ec, &line, &frame);
        *out = n[0] == 'l' ? line : (long)frame;
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
    if (!d) return -1;
    pthread_mutex_lock(&d->mtx);
    d->in_access = 0;
    rc = eval_locked(d, text, value, err, errsz);
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

static void trace_record(adamdebug *d, adamcore *c, uint16_t pc)
{
    adamdebug_trace *t;
    adamcore_z80_regs r;
    uint8_t code[4];
    z80d_insn ins;
    int line = 0;

    adamcore_get_regs(c, &r);
    adamcore_peek_block(c, pc, code, 4);
    z80_disassemble(&ins, pc, code);
    adamcore_debug_beam(c, &line, NULL);

    pthread_mutex_lock(&d->trace_mtx);
    t = &d->ring[d->ring_head];
    t->pc = pc;
    t->sp = r.sp;
    t->af = (uint16_t)((r.a << 8) | r.f);
    t->bc = (uint16_t)((r.b << 8) | r.c);
    t->de = (uint16_t)((r.d << 8) | r.e);
    t->hl = (uint16_t)((r.h << 8) | r.l);
    t->len = ins.len;
    memcpy(t->bytes, ins.bytes, 4);
    t->cycles = r.cycles;
    t->line = (int16_t)line;
    d->ring_head = (d->ring_head + 1) % TRACE_N;
    if (d->ring_count < TRACE_N)
        d->ring_count++;
    pthread_mutex_unlock(&d->trace_mtx);
    d->tr_last = pc;
}

/* ---- the hooks (emulation thread) ------------------------------------------- */

static int exec_hook(void *ud, adamcore *c, uint16_t pc)
{
    adamdebug *d = ud;
    const int first = d->skip;

    d->skip = 0;
    if (d->trace_on && !(first && d->tr_last == pc))
        trace_record(d, c, pc);
    if (first)
        return 0; /* the instruction the machine was released at */

    if (d->pending) {
        d->pending = 0;
        snprintf(d->hook_why, sizeof d->hook_why, "%s", d->pend_reason);
        d->hook_kind = ADAMDBG_STOP_WATCH;
        return 1;
    }
    switch (d->act) {
    case ACT_OVER:
    case ACT_OUT:
    case ACT_RUNTO:
        if (pc == d->target) {
            adamcore_z80_regs r;
            if (d->act == ACT_RUNTO) {
                snprintf(d->hook_why, sizeof d->hook_why, "run to");
                d->hook_kind = ADAMDBG_STOP_RUNTO;
                return 1;
            }
            adamcore_get_regs(c, &r);
            if (d->act == ACT_OVER ? (r.sp >= d->sp_guard && !r.halted)
                                   : r.sp > d->sp_guard) {
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
    return 0;
}

static void bus_hook(void *ud, int kind, uint16_t addr, uint8_t val)
{
    adamdebug *d = ud;
    int type, i, io = 0;

    switch (kind) {
    case ADAMCORE_BUS_MEM_R:
        type = ADAMDEBUG_BP_READ;
        if (!(d->map_mem[addr] & type)) return;
        break;
    case ADAMCORE_BUS_MEM_W:
        type = ADAMDEBUG_BP_WRITE;
        if (!(d->map_mem[addr] & type)) return;
        break;
    case ADAMCORE_BUS_IO_R:
        type = ADAMDEBUG_BP_IN;
        io = 1;
        if (!(d->map_io[addr & 0xFF] & type)) return;
        break;
    case ADAMCORE_BUS_IO_W:
        type = ADAMDEBUG_BP_OUT;
        io = 1;
        if (!(d->map_io[addr & 0xFF] & type)) return;
        break;
    default:
        return;
    }
    if (d->pending)
        return; /* the first hit in an instruction is the one reported */
    if (type == ADAMDEBUG_BP_READ) {
        /* the fetches of the instruction itself go through the same bus:
         * the byte just fetched is at PC-1 */
        adamcore_z80_regs r;
        adamcore_get_regs(core_of(d), &r);
        if ((uint16_t)(r.pc - 1) == addr)
            return;
    }
    if (io)
        addr &= 0xFF;

    pthread_mutex_lock(&d->mtx);
    d->acc_value = val;
    d->acc_addr = addr;
    d->in_access = 1;
    for (i = 0; i < d->nbp; i++) {
        const adamdebug_breakpoint *b = &d->bp[i];
        if (b->enabled && (b->type & type) && addr >= b->start &&
            addr <= b->end && cond_true_locked(d, b)) {
            d->bp[i].hits++;
            if (io)
                snprintf(d->pend_reason, sizeof d->pend_reason,
                         "%s $%02X %s $%02X",
                         type == ADAMDEBUG_BP_IN ? "in" : "out", addr,
                         type == ADAMDEBUG_BP_IN ? "->" : "<-", val);
            else
                snprintf(d->pend_reason, sizeof d->pend_reason,
                         "%s $%04X = $%02X",
                         type == ADAMDEBUG_BP_READ ? "read" : "write", addr,
                         val);
            d->pending = 1;
            break;
        }
    }
    d->in_access = 0;
    pthread_mutex_unlock(&d->mtx);
}

/* An enabled execute breakpoint at pc whose condition holds. */
static int exec_hit_locked(adamdebug *d, uint16_t pc)
{
    int i, hit = 0;

    d->acc_addr = pc;
    d->acc_value = peek(d, pc);
    d->in_access = 1;
    for (i = 0; i < d->nbp; i++) {
        adamdebug_breakpoint *b = &d->bp[i];
        if (b->enabled && (b->type & ADAMDEBUG_BP_EXEC) && pc >= b->start &&
            pc <= b->end && cond_true_locked(d, b)) {
            b->hits++;
            hit = 1;
        }
    }
    d->in_access = 0;
    return hit;
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
            d->pending = 0;
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
    want_exec = d->trace_on || d->nwatch > 0 ||
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
    d->pending = 0;
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
    update_engagement_locked(d);
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- the emulation thread's frame driver ------------------------------------ */

/* Park (mtx held; drops it around the callback). */
static void park_locked(adamdebug *d, adamcore *c, const char *reason,
                        int kind)
{
    adamcore_z80_regs r;
    void (*cb)(void *, int, uint16_t);
    void *ud;

    adamcore_get_regs(c, &r);
    snprintf(d->reason, sizeof d->reason, "%s", reason);
    d->reason_addr = r.pc;
    d->stopped = 1;
    d->stop_req = 0;
    d->pending = 0;
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
            park_locked(d, c, "stopped", ADAMDBG_STOP_PAUSE);
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
            if (d->pending) {
                park_locked(d, c, d->pend_reason, ADAMDBG_STOP_WATCH);
            } else if (d->act == ACT_STEP && --d->count <= 0) {
                park_locked(d, c, "step", ADAMDBG_STOP_STEP);
            }
            break;
        case ADAMCORE_RUN_BREAKPOINT: {
            adamcore_z80_regs r;
            adamcore_get_regs(c, &r);
            if (exec_hit_locked(d, r.pc)) {
                char why[48];
                if (d->trace_on)
                    trace_record(d, c, r.pc);
                snprintf(why, sizeof why, "breakpoint: exec $%04X", r.pc);
                park_locked(d, c, why, ADAMDBG_STOP_BREAKPOINT);
            } else {
                d->next1 = NEXT_PASS;
            }
            break;
        }
        case ADAMCORE_RUN_HOOK_STOP:
            park_locked(d, c, d->hook_why, d->hook_kind);
            break;
        case ADAMCORE_RUN_FRAME_DONE:
            if (d->act == ACT_FRAME && --d->count <= 0) {
                park_locked(d, c, "frame", ADAMDBG_STOP_STEP);
            } else if (d->act == ACT_SCANLINE) {
                d->last_line = 0; /* the beam wrapped: that was a line */
                if (--d->count <= 0) {
                    park_locked(d, c, "scanline", ADAMDBG_STOP_STEP);
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
            d->tr_last = -1;
            d->fuji = calloc(1, sizeof(*d->fuji));
            if (d->fuji)
                adamfuji_init(d->fuji);
            symtabs_load_text(&d->syms, adamdebug_builtin_os7_sym, "os7");
            symtabs_load_text(&d->syms, adamdebug_builtin_eos_sym, "eos");
            s->debugger = d;
        }
    }
    pthread_mutex_unlock(&s->lifecycle_mtx);
    return s->debugger;
}

void adamdebug_destroy(adamdebug *d)
{
    if (!d) return;
    symtabs_free(&d->syms);
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
    d->pending = 0;
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

int adamdebug_io_state(adamdebug *d, char *dst, int dstsz)
{
    adamdebug_memmap m;
    adamcore *c;
    uint8_t regs[8] = { 0 }, status = 0;
    uint16_t vaddr = 0;
    int n = 0, i;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&d->mtx);
    c = core_of(d);
    memmap_locked(d, &m);
    if (c)
        adamcore_vdp_state(c, regs, &status, &vaddr);
    pthread_mutex_unlock(&d->mtx);

#define PUT(...) do { if (n < dstsz - 1) { \
        int w_ = snprintf(dst + n, (size_t)(dstsz - n), __VA_ARGS__); \
        if (w_ > 0) n += w_ < dstsz - n ? w_ : dstsz - n - 1; } } while (0)

    if (!c) {
        PUT("The machine is not running.\n");
        return n;
    }
    PUT("Console      %s, last reset %s\n",
        m.cv_console ? "ColecoVision" : "ADAM",
        m.game_mode ? "game" : "computer");
    PUT("Memory map   $7F=%02X  lower bank %d (%s)  upper bank %d (%s)\n",
        m.mem_ctrl, m.lower, m.lower_name, m.upper, m.upper_name);
    switch (m.lower) {
    case 0:
        if (m.eos_rom) {
            PUT("  $0000-$5FFF  SmartWriter ROM\n");
            PUT("  $6000-$7FFF  EOS ROM\n");
        } else {
            PUT("  $0000-$7FFF  SmartWriter ROM\n");
        }
        break;
    case 1: PUT("  $0000-$7FFF  RAM (intrinsic 64K)\n"); break;
    case 2: PUT("  $0000-$7FFF  expansion RAM\n"); break;
    default:
        PUT("  $0000-$1FFF  OS7 ROM\n");
        if (m.cv_console) {
            PUT("  $2000-$5FFF  open bus\n");
            PUT("  $6000-$7FFF  1K RAM, mirrored every $400\n");
        } else {
            PUT("  $2000-$7FFF  RAM\n");
        }
        break;
    }
    switch (m.upper) {
    case 0: PUT("  $8000-$FFFF  RAM (intrinsic 64K)\n"); break;
    case 1: PUT("  $8000-$FFFF  expansion ROM socket (empty, reads $FF)\n");
            break;
    case 2: PUT("  $8000-$FFFF  expansion RAM\n"); break;
    default: PUT("  $8000-$FFFF  cartridge ROM\n"); break;
    }
    PUT("Net control  $3F=%02X  net reset %s, EOS ROM %s\n", m.net_ctrl,
        m.net_reset ? "high" : "low", m.eos_rom ? "on" : "off");
    PUT("\nPorts (A7-A5 pick the device)\n");
    PUT("  $20-$3F  net control ($3F): D0 net reset, D1 EOS ROM at $6000\n");
    PUT("  $40-$5F  Super Game Module $50-$53 (not on a stock console)\n");
    PUT("  $60-$7F  memory control ($7F): D1-D0 lower bank, D3-D2 upper%s\n",
        m.cv_console ? " (ADAM only)" : "");
    PUT("  $80-$9F  strobe: keypad     $C0-$DF  strobe: joystick\n");
    PUT("  $A0-$BF  VDP: even data, odd control (write) / status (read)\n");
    PUT("  $E0-$FF  read: controllers (bit 1 picks the port); write: PSG\n");
    PUT("\nVDP          ");
    for (i = 0; i < 8; i++)
        PUT("R%d=%02X ", i, regs[i]);
    PUT("\n             status %02X, address %04X\n", status, vaddr);
#undef PUT
    return n;
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
        d->acc_value = d->acc_addr = 0;
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

/* ---- trace -------------------------------------------------------------------- */

void adamdebug_trace_enable(adamdebug *d, int on)
{
    if (!d) return;
    pthread_mutex_lock(&d->mtx);
    d->trace_on = on != 0;
    bump_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int adamdebug_trace_enabled(adamdebug *d)
{
    return d && d->trace_on;
}

int adamdebug_trace_read(adamdebug *d, adamdebug_trace *out, int max)
{
    int n, i;
    if (!d) return 0;
    pthread_mutex_lock(&d->trace_mtx);
    n = d->ring_count < max ? d->ring_count : max;
    for (i = 0; i < n; i++)
        out[i] = d->ring[(d->ring_head - 1 - i + TRACE_N) % TRACE_N];
    pthread_mutex_unlock(&d->trace_mtx);
    return n;
}

void adamdebug_trace_clear(adamdebug *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->trace_mtx);
    d->ring_head = d->ring_count = 0;
    pthread_mutex_unlock(&d->trace_mtx);
    d->tr_last = -1;
    touch(d);
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
