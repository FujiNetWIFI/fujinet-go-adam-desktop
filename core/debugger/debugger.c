/*
 * adamdebug engine: execution control (pause / step into/over/out /
 * run-to), breakpoint management, the instruction-history ring, symbol
 * annotation, and the emulator-thread frame driver the session switches to
 * while the debugger is engaged.
 *
 * Execution model: all core mutation happens on the emulator thread.
 * Control calls from the UI thread flip state under the engine mutex and
 * signal the condvar the emulator parks on while paused. Transient stop
 * conditions (step-over/out, run-to) ride adamcore's exec hook, so they
 * survive frame boundaries, interrupts, and recursion (the SP floor).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../src/session_internal.h"
#include "adamdebug.h"
#include "symbols.h"
#include "z80dasm.h"

#define TRACE_RING_LEN 65536 /* power of two */

struct adamdebug {
    adamsession *session;
    pthread_mutex_t mtx;
    pthread_cond_t cv; /* wakes the parked emulator thread */

    int paused;        /* target run state */
    int pause_notify;  /* fire ADAMDBG_STOP_PAUSE when the park happens */
    int pending_step;  /* instructions to execute while paused */

    /* Transient stop condition, evaluated in the exec hook. */
    struct {
        int active;
        adamdebug_stop_reason reason;
        uint16_t target_pc;
        int has_sp_floor;
        int sp_strict; /* stop only when SP > floor (step-out) */
        uint16_t sp_floor;
    } trans;

    void (*stop_cb)(void *ud, adamdebug_stop_reason reason, uint16_t pc);
    void *stop_ud;

    /* Breakpoint shadow (survives session restarts; applied to each new
     * core). */
    uint8_t bp_map[0x2000];
    int bp_count;

    /* Instruction-history ring, written by the exec hook. */
    int trace_on;
    adamtrace_entry *ring;
    uint32_t ring_w;
    int ring_filled;

    sym_tables syms;
};

/* ---- engagement ----------------------------------------------------------- */

static void update_engagement_locked(adamdebug *d)
{
    adamsession *s = d->session;
    s->dbg_engaged = d->paused || d->pending_step || d->trans.active ||
                     d->trace_on || d->bp_count > 0;
}

static void apply_hook_locked(adamdebug *d);

/* Applies breakpoints + hook to a freshly created core (emulator thread,
 * from session.c right after adamcore_create). */
void adamdebug_on_core_created(adamsession *s)
{
    adamdebug *d = s->debugger;
    adamcore *c = s->core;
    int i;
    if (!d || !c) return;
    pthread_mutex_lock(&d->mtx);
    adamcore_bp_clear_all(c);
    for (i = 0; i < 0x10000; i++)
        if ((d->bp_map[i >> 3] >> (i & 7)) & 1)
            adamcore_bp_set(c, (uint16_t)i);
    apply_hook_locked(d);
    update_engagement_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

/* ---- exec hook ------------------------------------------------------------ */

static int exec_hook(void *ud, adamcore *c, uint16_t pc)
{
    adamdebug *d = ud;

    if (d->trace_on && d->ring) {
        adamtrace_entry *e = &d->ring[d->ring_w & (TRACE_RING_LEN - 1)];
        adamcore_z80_regs r;
        adamcore_get_regs(c, &r);
        e->pc = pc;
        e->sp = r.sp;
        e->af = (uint16_t)((r.a << 8) | r.f);
        e->bc = (uint16_t)((r.b << 8) | r.c);
        e->de = (uint16_t)((r.d << 8) | r.e);
        e->hl = (uint16_t)((r.h << 8) | r.l);
        e->cycles = r.cycles;
        adamcore_peek_block(c, pc, e->bytes, 4);
        {
            z80d_insn insn;
            e->len = (uint8_t)z80_disassemble(&insn, pc, e->bytes);
        }
        d->ring_w++;
        if (d->ring_w >= TRACE_RING_LEN)
            d->ring_filled = 1;
    }

    if (d->trans.active && pc == d->trans.target_pc) {
        if (!d->trans.has_sp_floor)
            return 1;
        {
            adamcore_z80_regs r;
            adamcore_get_regs(c, &r);
            if (d->trans.sp_strict ? r.sp > d->trans.sp_floor
                                   : r.sp >= d->trans.sp_floor)
                return 1;
        }
    }
    return 0;
}

static void apply_hook_locked(adamdebug *d)
{
    adamcore *c = d->session->core;
    if (!c) return;
    if (d->trace_on || d->trans.active)
        adamcore_set_exec_hook(c, exec_hook, d);
    else
        adamcore_set_exec_hook(c, NULL, NULL);
}

/* ---- emulator-thread frame driver ---------------------------------------- */

static void notify_stop(adamdebug *d, adamdebug_stop_reason reason,
                        uint16_t pc)
{
    void (*cb)(void *, adamdebug_stop_reason, uint16_t) = d->stop_cb;
    void *ud = d->stop_ud;
    if (cb)
        cb(ud, reason, pc);
}

int adamdebug_session_frame(adamsession *s)
{
    adamdebug *d = s->debugger;
    adamcore *c = s->core;
    int changed = 0;

    if (!d || !c)
        return adamcore_run_frame(c);

    pthread_mutex_lock(&d->mtx);
    for (;;) {
        if (s->stop_flag)
            break;

        if (d->paused) {
            if (d->pending_step > 0) {
                int n = d->pending_step;
                int chg = 0;
                uint16_t pc;
                adamcore_z80_regs r;
                d->pending_step = 0;
                pthread_mutex_unlock(&d->mtx);
                adamcore_debug_run(c, (uint32_t)n, 0, &chg);
                adamcore_get_regs(c, &r);
                pc = r.pc;
                pthread_mutex_lock(&d->mtx);
                changed |= chg;
                update_engagement_locked(d);
                pthread_mutex_unlock(&d->mtx);
                notify_stop(d, ADAMDBG_STOP_STEP, pc);
                pthread_mutex_lock(&d->mtx);
                continue;
            }
            if (d->pause_notify) {
                adamcore_z80_regs r;
                d->pause_notify = 0;
                adamcore_get_regs(c, &r);
                pthread_mutex_unlock(&d->mtx);
                notify_stop(d, ADAMDBG_STOP_PAUSE, r.pc);
                pthread_mutex_lock(&d->mtx);
                continue; /* re-evaluate: the callback may have resumed */
            }
            s->audio_mute = 1;
            pthread_cond_wait(&d->cv, &d->mtx);
            continue;
        }

        s->audio_mute = 0;

        /* Running with the debugger engaged. */
        {
            adamcore_run_status st;
            int chg = 0;
            pthread_mutex_unlock(&d->mtx);
            st = adamcore_debug_run(c, 0, 1, &chg);
            pthread_mutex_lock(&d->mtx);
            changed |= chg;

            if (st == ADAMCORE_RUN_FRAME_DONE)
                break; /* frame boundary: hand back to the paced loop */

            if (st == ADAMCORE_RUN_BREAKPOINT ||
                st == ADAMCORE_RUN_HOOK_STOP) {
                adamdebug_stop_reason reason =
                    st == ADAMCORE_RUN_BREAKPOINT ? ADAMDBG_STOP_BREAKPOINT
                                                  : d->trans.reason;
                adamcore_z80_regs r;
                uint16_t pc;
                d->paused = 1;
                d->trans.active = 0;
                apply_hook_locked(d);
                update_engagement_locked(d);
                adamcore_get_regs(c, &r);
                pc = r.pc;
                pthread_mutex_unlock(&d->mtx);
                notify_stop(d, reason, pc);
                pthread_mutex_lock(&d->mtx);
            }
        }
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
    pthread_mutex_unlock(&d->mtx);
}

/* ---- lifecycle ------------------------------------------------------------ */

adamdebug *adamsession_debugger(adamsession *s)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->debugger) {
        adamdebug *d = calloc(1, sizeof(*d));
        if (d) {
            d->session = s;
            pthread_mutex_init(&d->mtx, NULL);
            pthread_cond_init(&d->cv, NULL);
            d->ring = calloc(TRACE_RING_LEN, sizeof(adamtrace_entry));
            symtabs_load_text(&d->syms, adamdebug_builtin_os7_sym, "os7");
            symtabs_load_text(&d->syms, adamdebug_builtin_eos_sym, "eos");
            s->debugger = d;
        }
    }
    pthread_mutex_unlock(&s->lifecycle_mtx);
    return s->debugger;
}

/* ---- execution control ---------------------------------------------------- */

void adamdebug_pause(adamdebug *d)
{
    pthread_mutex_lock(&d->mtx);
    if (!d->paused) {
        d->paused = 1;
        d->pause_notify = 1;
        d->trans.active = 0;
        apply_hook_locked(d);
        update_engagement_locked(d);
    }
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_resume(adamdebug *d)
{
    pthread_mutex_lock(&d->mtx);
    d->paused = 0;
    d->trans.active = 0;
    apply_hook_locked(d);
    update_engagement_locked(d);
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

int adamdebug_is_paused(adamdebug *d)
{
    int p;
    pthread_mutex_lock(&d->mtx);
    p = d->paused;
    pthread_mutex_unlock(&d->mtx);
    return p;
}

void adamdebug_step_into(adamdebug *d)
{
    pthread_mutex_lock(&d->mtx);
    if (d->paused) {
        d->pending_step = 1;
        update_engagement_locked(d);
        pthread_cond_broadcast(&d->cv);
    }
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_step_over(adamdebug *d)
{
    adamsession *s = d->session;
    adamcore *c = s->core;
    adamcore_z80_regs r;
    uint8_t code[4];
    z80d_insn insn;

    if (!c) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    adamcore_get_regs(c, &r);
    adamcore_peek_block(c, r.pc, code, 4);
    z80_disassemble(&insn, r.pc, code);

    if (insn.flags & (ADAMDASM_CALL | ADAMDASM_HALT | ADAMDASM_BLOCK)) {
        /* Run until control returns to the next instruction with the stack
         * back at (or above) this level -- survives recursion and any NMI
         * that fires while the callee runs. */
        d->trans.active = 1;
        d->trans.reason = ADAMDBG_STOP_STEP;
        d->trans.target_pc = (uint16_t)(r.pc + insn.len);
        d->trans.has_sp_floor = 1;
        d->trans.sp_strict = 0;
        d->trans.sp_floor = r.sp;
        d->paused = 0;
        apply_hook_locked(d);
    } else {
        d->pending_step = 1;
    }
    update_engagement_locked(d);
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_step_out(adamdebug *d)
{
    adamsession *s = d->session;
    adamcore *c = s->core;
    adamcore_z80_regs r;
    uint8_t lo, hi;

    if (!c) return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused) {
        pthread_mutex_unlock(&d->mtx);
        return;
    }
    adamcore_get_regs(c, &r);
    lo = adamcore_peek(c, r.sp);
    hi = adamcore_peek(c, (uint16_t)(r.sp + 1));
    d->trans.active = 1;
    d->trans.reason = ADAMDBG_STOP_STEP;
    d->trans.target_pc = (uint16_t)(lo | (hi << 8));
    d->trans.has_sp_floor = 1;
    d->trans.sp_strict = 1;
    d->trans.sp_floor = r.sp;
    d->paused = 0;
    apply_hook_locked(d);
    update_engagement_locked(d);
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_run_to(adamdebug *d, uint16_t addr)
{
    pthread_mutex_lock(&d->mtx);
    d->trans.active = 1;
    d->trans.reason = ADAMDBG_STOP_RUNTO;
    d->trans.target_pc = addr;
    d->trans.has_sp_floor = 0;
    d->trans.sp_strict = 0;
    d->paused = 0;
    apply_hook_locked(d);
    update_engagement_locked(d);
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_set_stop_callback(adamdebug *d,
                                 void (*cb)(void *, adamdebug_stop_reason,
                                            uint16_t),
                                 void *ud)
{
    pthread_mutex_lock(&d->mtx);
    d->stop_cb = cb;
    d->stop_ud = ud;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- breakpoints ---------------------------------------------------------- */

static void bp_shadow_set(adamdebug *d, uint16_t addr, int set)
{
    int had = (d->bp_map[addr >> 3] >> (addr & 7)) & 1;
    if (set && !had) {
        d->bp_map[addr >> 3] |= (uint8_t)(1u << (addr & 7));
        d->bp_count++;
    } else if (!set && had) {
        d->bp_map[addr >> 3] &= (uint8_t)~(1u << (addr & 7));
        d->bp_count--;
    }
}

void adamdebug_bp_set(adamdebug *d, uint16_t addr)
{
    adamcore *c;
    pthread_mutex_lock(&d->mtx);
    bp_shadow_set(d, addr, 1);
    c = d->session->core;
    if (c) adamcore_bp_set(c, addr);
    update_engagement_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_bp_clear(adamdebug *d, uint16_t addr)
{
    adamcore *c;
    pthread_mutex_lock(&d->mtx);
    bp_shadow_set(d, addr, 0);
    c = d->session->core;
    if (c) adamcore_bp_clear(c, addr);
    update_engagement_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void adamdebug_bp_toggle(adamdebug *d, uint16_t addr)
{
    if (adamdebug_bp_is_set(d, addr))
        adamdebug_bp_clear(d, addr);
    else
        adamdebug_bp_set(d, addr);
}

void adamdebug_bp_clear_all(adamdebug *d)
{
    adamcore *c;
    pthread_mutex_lock(&d->mtx);
    memset(d->bp_map, 0, sizeof(d->bp_map));
    d->bp_count = 0;
    c = d->session->core;
    if (c) adamcore_bp_clear_all(c);
    update_engagement_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int adamdebug_bp_is_set(adamdebug *d, uint16_t addr)
{
    int set;
    pthread_mutex_lock(&d->mtx);
    set = (d->bp_map[addr >> 3] >> (addr & 7)) & 1;
    pthread_mutex_unlock(&d->mtx);
    return set;
}

int adamdebug_bp_list(adamdebug *d, uint16_t *out, int max)
{
    int n = 0, i;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < 0x10000 && n < max; i++)
        if ((d->bp_map[i >> 3] >> (i & 7)) & 1)
            out[n++] = (uint16_t)i;
    pthread_mutex_unlock(&d->mtx);
    return n;
}

/* ---- CPU / memory ---------------------------------------------------------- */

void adamdebug_get_regs(adamdebug *d, adamcore_z80_regs *out)
{
    adamcore *c = d->session->core;
    if (c)
        adamcore_get_regs(c, out);
    else
        memset(out, 0, sizeof(*out));
}

void adamdebug_set_regs(adamdebug *d, const adamcore_z80_regs *in)
{
    adamcore *c = d->session->core;
    if (c)
        adamcore_set_regs(c, in);
}

int adamdebug_read_mem(adamdebug *d, uint16_t addr, uint8_t *dst, int n)
{
    adamcore *c = d->session->core;
    if (!c) {
        memset(dst, 0, (size_t)n);
        return 0;
    }
    adamcore_peek_block(c, addr, dst, (uint32_t)n);
    return n;
}

int adamdebug_write_mem(adamdebug *d, uint16_t addr, const uint8_t *src,
                        int n)
{
    adamcore *c = d->session->core;
    int i;
    if (!c) return 0;
    for (i = 0; i < n; i++)
        adamcore_poke(c, (uint16_t)(addr + i), src[i]);
    return n;
}

/* ---- disassembly ----------------------------------------------------------- */

int adamdebug_disassemble(adamdebug *d, uint16_t addr, int count,
                          adamdasm_line *out)
{
    adamcore *c = d->session->core;
    int i;
    uint16_t pc = addr;

    if (!c) return 0;
    for (i = 0; i < count; i++) {
        uint8_t code[4];
        z80d_insn insn;
        adamdasm_line *l = &out[i];
        uint16_t off = 0;
        const char *sym;

        adamcore_peek_block(c, pc, code, 4);
        z80_disassemble(&insn, pc, code);
        l->addr = pc;
        l->len = insn.len;
        memcpy(l->bytes, insn.bytes, 4);
        memcpy(l->text, insn.text, sizeof(l->text));
        l->target = insn.target;
        l->flags = insn.flags;
        sym = symtabs_at(&d->syms, pc, &off);
        l->symbol = (sym && off == 0) ? sym : NULL;
        pc = (uint16_t)(pc + insn.len);
    }
    return count;
}

/* ---- trace ----------------------------------------------------------------- */

void adamdebug_trace_enable(adamdebug *d, int enable)
{
    pthread_mutex_lock(&d->mtx);
    d->trace_on = enable ? 1 : 0;
    apply_hook_locked(d);
    update_engagement_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

int adamdebug_trace_enabled(adamdebug *d)
{
    int on;
    pthread_mutex_lock(&d->mtx);
    on = d->trace_on;
    pthread_mutex_unlock(&d->mtx);
    return on;
}

int adamdebug_trace_read(adamdebug *d, adamtrace_entry *out, int max)
{
    int n = 0;
    uint32_t avail, i;
    pthread_mutex_lock(&d->mtx);
    avail = d->ring_filled ? TRACE_RING_LEN : d->ring_w;
    for (i = 0; i < avail && n < max; i++) {
        uint32_t idx = (d->ring_w - 1 - i) & (TRACE_RING_LEN - 1);
        out[n++] = d->ring[idx];
    }
    pthread_mutex_unlock(&d->mtx);
    return n;
}

void adamdebug_trace_clear(adamdebug *d)
{
    pthread_mutex_lock(&d->mtx);
    d->ring_w = 0;
    d->ring_filled = 0;
    pthread_mutex_unlock(&d->mtx);
}

/* ---- symbols ---------------------------------------------------------------- */

int adamdebug_symbols_load(adamdebug *d, const char *path, const char *table)
{
    int rc;
    pthread_mutex_lock(&d->mtx);
    rc = symtabs_load_file(&d->syms, path, table);
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

/* ---- VDP -------------------------------------------------------------------- */

void adamdebug_vdp_snapshot(adamdebug *d, adamvdp_snapshot *out)
{
    adamcore *c = d->session->core;
    if (!c) {
        memset(out, 0, sizeof(*out));
        return;
    }
    memcpy(out->vram, adamcore_vdp_vram(c), sizeof(out->vram));
    adamcore_vdp_state(c, out->regs, &out->status, &out->addr);
    memcpy(out->palette565, adamcore_palette565(c), sizeof(out->palette565));
}
