/*
 * engine_test -- the newer debugger engine (core/include/adamdebug.h) against a
 * real running machine, with no BIOS and no real ROMs: a hand-assembled
 * ColecoVision cartridge holds the programs, the engine is attached before the
 * session starts (so the machine stops at its first instruction) and the PC is
 * pointed into the cartridge. It runs everywhere, including CI, and never
 * skips.
 *
 * Program A is a loop that touches everything the engine can watch:
 *
 *   8024  DI / LD SP,$6400 / LD B,0
 *   802A  INC B                 ; the loop top
 *   802B  LD A,B
 *   802C  LD ($6000),A          ; a write
 *   802F  LD A,($6001)          ; a read
 *   8032  CALL $8050            ; INC C / RET
 *   8035  IN A,($FC)            ; a port read
 *   8037  OUT ($44),A           ; a port write
 *   8039  JP $802A
 *
 * Program B writes the SN76489 and halts; program C writes the VDP's ports.
 *
 * It covers the engine's contract and the latent bugs the Coleco port found
 * (a resume from a breakpoint that re-fires the same breakpoint; a pause that
 * can only land on a frame boundary; the core freed under a polling window;
 * breakpoints, hooks and trace lost across a session restart; stop state read
 * unlocked), and the ADAM memory configuration.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "adamcore_debug.h"
#include "adamdebug.h"
#include "adammemtools.h"
#include "adamsession.h"
#include "session_internal.h"
#include "test_tmp.h"

static int failures;
static adamsession *g_s;
static adamdebug *g_d;
static char g_cart[600];

static void check(const char *what, int passed)
{
    printf("  %s %s\n", passed ? "ok  " : "FAIL", what);
    if (!passed) failures++;
}

static void nap(int millis)
{
    struct timespec delay = { millis / 1000, (long)(millis % 1000) * 1000000L };
    nanosleep(&delay, NULL);
}

#define LOOP    0x8024
#define LOOPTOP 0x802A
#define SUB     0x8050
#define SOUND   0x8100
#define VDPLOOP 0x8140

static void build_cart(const char *path)
{
    static const uint8_t loop_main[] = {
        0xF3,                   /* 8024 DI */
        0x31, 0x00, 0x64,       /* 8025 LD SP,$6400 */
        0x06, 0x00,             /* 8028 LD B,0 */
        0x04,                   /* 802A INC B */
        0x78,                   /* 802B LD A,B */
        0x32, 0x00, 0x60,       /* 802C LD ($6000),A */
        0x3A, 0x01, 0x60,       /* 802F LD A,($6001) */
        0xCD, 0x50, 0x80,       /* 8032 CALL $8050 */
        0xDB, 0xFC,             /* 8035 IN A,($FC) */
        0xD3, 0x44,             /* 8037 OUT ($44),A */
        0xC3, 0x2A, 0x80,       /* 8039 JP $802A */
    };
    static const uint8_t sub[] = { 0x0C, 0xC9 }; /* 8050 INC C / RET */
    static const uint8_t sound[] = {
        0xF3,                               /* 8100 DI */
        0x31, 0x00, 0x64,                   /* LD SP,$6400 */
        0x3E, 0x8E, 0xD3, 0xFF,             /* tone 1 period, low nibble E */
        0x3E, 0x04, 0xD3, 0xFF,             /* ... high bits 4: period $4E */
        0x3E, 0x90, 0xD3, 0xFF,             /* tone 1 attenuation 0 */
        0x3E, 0xBF, 0xD3, 0xFF,             /* tone 2 attenuation 15 */
        0x3E, 0xE5, 0xD3, 0xFF,             /* noise: white, rate 1 */
        0x3E, 0xFF, 0xD3, 0xFF,             /* noise attenuation 15 */
        0x76,                               /* HALT */
        0x18, 0xFD,                         /* JR -3 (never reached) */
    };
    static const uint8_t vdp[] = {
        0xF3, 0x31, 0x00, 0x64,             /* 8140 DI / LD SP,$6400 */
        0x3E, 0x00, 0xD3, 0xA1,             /* 8144 VDP address low 0 */
        0x3E, 0x40, 0xD3, 0xA1,             /* 8148 high $40: write VRAM at 0 */
        0x3E, 0x55, 0xD3, 0xA0,             /* 814C data $55 */
        0xC3, 0x44, 0x81,                   /* 8150 JP $8144 */
    };
    uint8_t img[512];
    FILE *file;
    int idx;

    memset(img, 0x00, sizeof img);
    img[0] = 0x55;            /* ColecoVision: skip the title screen */
    img[1] = 0xAA;
    img[0x0A] = 0x24;         /* start address $8024 */
    img[0x0B] = 0x80;
    for (idx = 0x0C; idx < 0x21; idx++)
        img[idx] = 0xC9;        /* RST vectors: RET */
    img[0x21] = 0xED;         /* NMI vector: RETN */
    img[0x22] = 0x45;
    memcpy(img + 0x24, loop_main, sizeof loop_main);
    memcpy(img + 0x50, sub, sizeof sub);
    memcpy(img + 0x100, sound, sizeof sound);
    memcpy(img + 0x140, vdp, sizeof vdp);
    file = fopen(path, "wb");
    if (file) {
        fwrite(img, 1, sizeof img, file);
        fclose(file);
    }
}

static int start_cv(void)
{
    adamsession_start_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.machine = 1;
    opts.cart_path = g_cart;
    opts.joystick_mode = 1;
    return adamsession_start(g_s, &opts);
}

static int wait_stopped(int millis)
{
    int waited;
    for (waited = 0; waited < millis; waited += 5) {
        if (adamdebug_is_stopped(g_d)) return 1;
        nap(5);
    }
    return 0;
}

/* Stopped at a stop newer than `serial`. */
static int wait_new_stop(uint64_t serial, int millis)
{
    int waited;
    for (waited = 0; waited < millis; waited += 5) {
        if (adamdebug_is_stopped(g_d) && adamdebug_stop_serial(g_d) != serial) return 1;
        nap(5);
    }
    return 0;
}

static int pause_machine(void)
{
    adamdebug_stop(g_d);
    return wait_stopped(3000);
}

static adamcore_z80_regs regs_now(void)
{
    adamdebug_cpu cpu;
    adamcore_z80_regs regs;

    adamdebug_cpu_get(g_d, &cpu);
    memset(&regs, 0, sizeof regs);
    regs.a = (uint8_t)(cpu.af >> 8);
    regs.f = (uint8_t)cpu.af;
    regs.b = (uint8_t)(cpu.bc >> 8);
    regs.c = (uint8_t)cpu.bc;
    regs.d = (uint8_t)(cpu.de >> 8);
    regs.e = (uint8_t)cpu.de;
    regs.h = (uint8_t)(cpu.hl >> 8);
    regs.l = (uint8_t)cpu.hl;
    regs.a2 = (uint8_t)(cpu.af2 >> 8);
    regs.f2 = (uint8_t)cpu.af2;
    regs.b2 = (uint8_t)(cpu.bc2 >> 8);
    regs.c2 = (uint8_t)cpu.bc2;
    regs.d2 = (uint8_t)(cpu.de2 >> 8);
    regs.e2 = (uint8_t)cpu.de2;
    regs.h2 = (uint8_t)(cpu.hl2 >> 8);
    regs.l2 = (uint8_t)cpu.hl2;
    regs.ix = (uint16_t)cpu.ix;
    regs.iy = (uint16_t)cpu.iy;
    regs.sp = (uint16_t)cpu.sp;
    regs.pc = (uint16_t)cpu.pc;
    regs.wz = (uint16_t)cpu.wz;
    regs.i = (uint8_t)cpu.i;
    regs.r = (uint8_t)cpu.r;
    regs.iff1 = (uint8_t)cpu.iff1;
    regs.iff2 = (uint8_t)cpu.iff2;
    regs.im = (uint8_t)cpu.im;
    regs.halted = (uint8_t)cpu.halted;
    regs.cycles = cpu.cycles;
    return regs;
}

/* Lets the machine go and waits for the next stop. */
static int go_and_wait(void)
{
    const uint64_t serial = adamdebug_stop_serial(g_d);
    adamdebug_resume(g_d);
    return wait_new_stop(serial, 3000);
}

static adamdebug_snapshot snap_now(void)
{
    adamdebug_snapshot snap;
    adamdebug_stop_snapshot(g_d, &snap);
    return snap;
}

/* A fresh machine, stopped at its first instruction, the PC then moved to
 * `pc`. */
static int boot_at(uint16_t start_pc)
{
    if (!adamdebug_is_attached(g_d)) adamdebug_attach(g_d);
    adamsession_stop(g_s);
    if (start_cv() != 0) return 0;
    if (!wait_stopped(3000)) return 0;
    adamdebug_cpu_set(g_d, ADAM_REG_PC, start_pc);
    return regs_now().pc == start_pc;
}

/* ---------------------------------------------------------------------------- */

static void test_expressions(void)
{
    long value = 0;
    char err[96];
    adamcore_z80_regs regs = regs_now();

    printf("expressions against a stopped machine:\n");
    check("a register", adamdebug_eval(g_d, "b + 1", &value, err, sizeof err) == 0 && value == regs.b + 1);
    check("a 16-bit register", adamdebug_eval(g_d, "sp", &value, err, sizeof err) == 0 && value == regs.sp);
    check("a byte read", adamdebug_eval(g_d, "[$8024]", &value, err, sizeof err) == 0 && value == 0xF3);
    check("a word read", adamdebug_eval(g_d, "{$8025}", &value, err, sizeof err) == 0 && value == 0x0031);
    check("a flag", adamdebug_eval(g_d, "zf", &value, err, sizeof err) == 0 && (value == 0 || value == 1));
    check("cycles", adamdebug_eval(g_d, "cycles", &value, err, sizeof err) == 0 && value >= 0);
    check("frame and vline", adamdebug_eval(g_d, "frame", &value, err, sizeof err) == 0 && value >= 0 &&
                             adamdebug_eval(g_d, "vline", &value, err, sizeof err) == 0 && value >= 0);
    check("ixh and iyl", adamdebug_eval(g_d, "ixh + iyl", &value, err, sizeof err) == 0);
    check("vdpaddr and vdpsecond", adamdebug_eval(g_d, "vdpaddr", &value, err, sizeof err) == 0 &&
                                   adamdebug_eval(g_d, "vdpsecond", &value, err, sizeof err) == 0);
    check("an unknown name is refused", adamdebug_eval(g_d, "nosuch", &value, err, sizeof err) != 0 && err[0]);
    check("value/old/addr are unknown outside a watch stop",
          adamdebug_eval(g_d, "value", &value, err, sizeof err) != 0);
}

static void test_pause_and_breakpoints(void)
{
    int idx, mid_frame = 0, bp_id;
    adamcore_z80_regs regs;
    uint64_t serial;
    adamdebug_breakpoint list[8];

    printf("pause lands on an instruction, not a frame boundary:\n");
    /* a false condition and the trace keep the emulator thread busy for a good
     * part of each frame, so a pause has a chance to arrive mid-frame */
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_EXEC, 0x3000, 0x3000, "", NULL, 0);
    check("a breakpoint can be added to a stopped machine", bp_id > 0);
    adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_EXEC, LOOPTOP, LOOPTOP, "b == 999", NULL, 0);
    adamdebug_trace_enable(g_d, 1);
    adamdebug_resume(g_d);
    nap(100);
    for (idx = 0; idx < 600 && (idx < 60 || mid_frame == 0); idx++) {
        int line = 0, tstate;
        if (!pause_machine()) break;
        {
            adamdebug_io iostate;
            adamdebug_io_get(g_d, &iostate);
            line = iostate.beam_line;
            tstate = iostate.beam_tstate;
        }
        if (line != 0 || tstate != 0) mid_frame++;
        adamdebug_resume(g_d);
        nap(3 + (idx * 7) % 13);
    }
    printf("      %d of %d pauses landed mid-frame\n", mid_frame, idx);
    check("pauses land mid-frame (a frame-boundary-only pause would give none)", mid_frame >= 1);
    adamdebug_trace_enable(g_d, 0);
    adamdebug_breakpoint_clear(g_d);
    adamdebug_resume(g_d);
    pause_machine();

    printf("a resume from a breakpoint does not fire it again:\n");
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_EXEC, LOOPTOP, LOOPTOP, "", NULL, 0);
    check("break at the loop top", bp_id > 0);
    check("it stops", go_and_wait());
    {
        adamdebug_snapshot snap = snap_now();
        check("the reason is a breakpoint", snap.stop.reason == ADAMDEBUG_STOP_BREAKPOINT && snap.stop.bp_id == bp_id);
        check("the text says so", !strcmp(snap.text, "breakpoint: exec $802A"));
        check("at $802A", snap.address == LOOPTOP && regs_now().pc == LOOPTOP);
    }
    regs = regs_now();
    {
        int steady = 1;
        uint8_t last = regs.b;
        for (idx = 0; idx < 5; idx++) {
            if (!go_and_wait()) { steady = 0; break; }
            regs = regs_now();
            if (regs.pc != LOOPTOP || regs.b != (uint8_t)(last + 1)) steady = 0;
            last = regs.b;
        }
        check("each resume runs exactly one loop (B +1) and stops at $802A again", steady);
    }
    adamdebug_breakpoint_list(g_d, list, 8);
    check("every stop counted a hit", list[0].hits >= 6);
    check("the generation moved", adamdebug_generation(g_d) > 0);

    printf("conditions and enable/disable:\n");
    check("a condition is refused if it does not parse",
          !adamdebug_breakpoint_set_condition(g_d, bp_id, "b ==", NULL, 0));
    check("a good condition is taken", adamdebug_breakpoint_set_condition(g_d, bp_id, "b == 200", NULL, 0));
    adamdebug_breakpoint_clear_hits(g_d);
    check("it stops when the condition holds", go_and_wait());
    regs = regs_now();
    check("with B at 200", regs.b == 200 && regs.pc == LOOPTOP);
    adamdebug_breakpoint_list(g_d, list, 8);
    check("only the true evaluation is a hit", list[0].hits == 1);
    adamdebug_breakpoint_enable(g_d, bp_id, 0);
    serial = adamdebug_stop_serial(g_d);
    adamdebug_resume(g_d);
    nap(150);
    check("a disabled breakpoint never stops it", !adamdebug_is_stopped(g_d) && adamdebug_stop_serial(g_d) == serial);
    adamdebug_breakpoint_enable(g_d, bp_id, 1);
    check("enabled again it does", wait_new_stop(serial, 3000));
    adamdebug_breakpoint_remove(g_d, bp_id);
    check("removed, the table is empty", adamdebug_breakpoint_list(g_d, list, 8) == 0);
}

static void test_watchpoints(void)
{
    adamdebug_snapshot snap;
    adamcore_z80_regs regs;
    int bp_id;
    char want[96];
    adamdebug_breakpoint list[8];

    printf("watchpoints stop at the next instruction:\n");
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_WRITE, 0x6000, 0x6000, "", NULL, 0);
    check("bpw $6000", bp_id > 0);
    check("it stops", go_and_wait());
    snap = snap_now();
    regs = regs_now();
    check("reason write", snap.stop.reason == ADAMDEBUG_STOP_WATCH_WRITE && snap.stop.bp_id == bp_id);
    check("the address is $6000", snap.stop.addr == 0x6000);
    check("stopped at the instruction after the store", regs.pc == 0x802F && snap.stop.pc == 0x802F);
    check("from_pc names the store", snap.stop.from_pc == 0x802C);
    check("value is what was stored", snap.stop.value == regs.a);
    snprintf(want, sizeof want, "wrote $6000 = $%02X (was $%02X) at $802F", snap.stop.value, snap.stop.old_value);
    check("the text", !strcmp(snap.text, want));
    printf("      %s\n", snap.text);
    {
        char why[128];
        snprintf(want, sizeof want, "write $6000 = $%02X", snap.stop.value);
        adamdebug_stop_reason(g_d, why, sizeof why, NULL);
        check("the older terse wording is unchanged", !strcmp(why, want));
    }
    {
        long value = 0;
        char err[64];
        check("value in an expression", adamdebug_eval(g_d, "value", &value, err, sizeof err) == 0 && value == snap.stop.value);
        check("old in an expression", adamdebug_eval(g_d, "old", &value, err, sizeof err) == 0 && value == snap.stop.old_value);
        check("addr in an expression", adamdebug_eval(g_d, "addr", &value, err, sizeof err) == 0 && value == 0x6000);
    }
    {
        const int first = snap.stop.value;
        check("the next stop is the next iteration", go_and_wait());
        snap = snap_now();
        check("old is the byte the last store left", snap.stop.old_value == first);
    }
    adamdebug_breakpoint_remove(g_d, bp_id);

    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_WRITE, 0x6000, 0x6000, "value == 5 && old == 4", NULL, 0);
    check("a write condition in the grammar", bp_id > 0);
    check("it stops on that store", go_and_wait());
    snap = snap_now();
    check("value 5 over 4", snap.stop.value == 5 && snap.stop.old_value == 4);
    adamdebug_breakpoint_remove(g_d, bp_id);
    check("an unknown name is refused in an exec condition",
          adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_EXEC, LOOPTOP, LOOPTOP, "nosuchname", NULL, 0) < 0);

    printf("an opcode fetch is not a read:\n");
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_READ, 0x8024, 0x8051, "", NULL, 0);
    check("bpr over the whole program", bp_id > 0);
    {
        const uint64_t serial = adamdebug_stop_serial(g_d);
        adamdebug_resume(g_d);
        nap(200);
        check("it never stops (fetches only)", !adamdebug_is_stopped(g_d) && adamdebug_stop_serial(g_d) == serial);
        adamdebug_breakpoint_list(g_d, list, 8);
        check("and counts nothing", list[0].hits == 0);
        pause_machine();
    }
    adamdebug_breakpoint_remove(g_d, bp_id);

    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_READ, 0x6001, 0x6001, "", NULL, 0);
    check("bpr $6001", bp_id > 0);
    check("it stops", go_and_wait());
    snap = snap_now();
    regs = regs_now();
    check("reason read, after the load", snap.stop.reason == ADAMDEBUG_STOP_WATCH_READ && regs.pc == 0x8032);
    check("value is what was read", snap.stop.value == regs.a && snap.stop.old_value == snap.stop.value);
    snprintf(want, sizeof want, "read $6001 = $%02X at $8032", snap.stop.value);
    check("the text", !strcmp(snap.text, want));
    adamdebug_breakpoint_remove(g_d, bp_id);

    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_IN, 0xFC, 0xFC, "", NULL, 0);
    check("bpin $FC", bp_id > 0);
    check("it stops", go_and_wait());
    snap = snap_now();
    regs = regs_now();
    check("reason in, at the OUT", snap.stop.reason == ADAMDEBUG_STOP_WATCH_IN && regs.pc == 0x8037);
    check("value is the byte the IN got", snap.stop.value == regs.a && snap.stop.addr == 0xFC);
    snprintf(want, sizeof want, "in $FC = $%02X at $8037", snap.stop.value);
    check("the text", !strcmp(snap.text, want));
    adamdebug_breakpoint_remove(g_d, bp_id);

    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_OUT, 0x40, 0x4F, "value == a", NULL, 0);
    check("bpout $40-$4F with a condition", bp_id > 0);
    check("it stops", go_and_wait());
    snap = snap_now();
    regs = regs_now();
    check("reason out, at the JP", snap.stop.reason == ADAMDEBUG_STOP_WATCH_OUT && regs.pc == 0x8039);
    check("port $44", snap.stop.addr == 0x44);
    check("an OUT has no old value", snap.stop.old_value == -1);
    adamdebug_breakpoint_remove(g_d, bp_id);

    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_ACCESS, 0x6000, 0x6001, "", NULL, 0);
    check("bpa $6000-$6001", bp_id > 0);
    check("it stops on the store", go_and_wait());
    check("reason write", snap_now().stop.reason == ADAMDEBUG_STOP_WATCH_WRITE);
    check("then on the load", go_and_wait() && snap_now().stop.reason == ADAMDEBUG_STOP_WATCH_READ);
    adamdebug_breakpoint_list(g_d, list, 8);
    check("one id, one hit count for both", list[0].hits >= 2 && list[0].type == ADAMDEBUG_BP_ACCESS);
    adamdebug_breakpoint_remove(g_d, bp_id);

    {
        char err[64];
        check("ports are 0-255", adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_IN, 0, 0x100, "", err, sizeof err) < 0 && err[0]);
    }

    printf("a watch and an execute breakpoint on the same boundary:\n");
    pause_machine();
    adamdebug_cpu_set(g_d, ADAM_REG_PC, LOOPTOP);
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_WRITE, 0x6000, 0x6000, "", NULL, 0);
    {
        const int exec_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_EXEC, 0x802F, 0x802F, "", NULL, 0);
        check("both set", bp_id > 0 && exec_id > 0);
        check("the stop is the write watch, not lost to the breakpoint", go_and_wait() && regs_now().pc == 0x802F &&
              snap_now().stop.reason == ADAMDEBUG_STOP_WATCH_WRITE && snap_now().stop.bp_id == bp_id);
        adamdebug_breakpoint_list(g_d, list, 8);
        check("the breakpoint counted its hit all the same", list[1].hits == 1);
        adamdebug_breakpoint_remove(g_d, exec_id);
    }
    adamdebug_breakpoint_remove(g_d, bp_id);
}

static void test_stepping(void)
{
    adamcore_z80_regs before, regs;
    uint64_t serial;
    adamdebug_snapshot snap;
    int bp_id;

    printf("run to, step into, over, out, and frame:\n");
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_EXEC, LOOPTOP, LOOPTOP, "", NULL, 0);
    check("stop at the loop top", go_and_wait() && regs_now().pc == LOOPTOP);
    adamdebug_breakpoint_remove(g_d, bp_id);

    before = regs_now();
    serial = adamdebug_stop_serial(g_d);
    adamdebug_run_to(g_d, LOOPTOP);
    check("run to the current PC goes all the way round", wait_new_stop(serial, 3000));
    regs = regs_now();
    check("...and stops there with the loop run once", regs.pc == LOOPTOP && regs.b == (uint8_t)(before.b + 1));
    snap = snap_now();
    check("a one-shot reads 'stopped at'", snap.stop.reason == ADAMDEBUG_STOP_ONESHOT && !strcmp(snap.text, "stopped at $802A"));

    serial = adamdebug_stop_serial(g_d);
    adamdebug_run_to(g_d, 0x8032);
    check("run to the CALL", wait_new_stop(serial, 3000) && regs_now().pc == 0x8032);

    before = regs_now();
    serial = adamdebug_stop_serial(g_d);
    adamdebug_step_over(g_d);
    check("step over a CALL", wait_new_stop(serial, 3000));
    regs = regs_now();
    check("...lands after it with the subroutine run", regs.pc == 0x8035 && regs.c == (uint8_t)(before.c + 1));
    check("...and the stack where it was", regs.sp == before.sp);

    serial = adamdebug_stop_serial(g_d);
    adamdebug_step(g_d);
    check("step into an IN", wait_new_stop(serial, 3000) && regs_now().pc == 0x8037);
    snap = snap_now();
    check("...reason step", snap.stop.reason == ADAMDEBUG_STOP_STEP && !strcmp(snap.text, "step at $8037"));

    serial = adamdebug_stop_serial(g_d);
    adamdebug_run_to(g_d, LOOPTOP);
    check("back to the loop top", wait_new_stop(serial, 3000));
    serial = adamdebug_stop_serial(g_d);
    adamdebug_run_to(g_d, 0x8032);
    wait_new_stop(serial, 3000);
    serial = adamdebug_stop_serial(g_d);
    adamdebug_step(g_d);
    check("step into a CALL", wait_new_stop(serial, 3000) && regs_now().pc == SUB);

    before = regs_now();
    serial = adamdebug_stop_serial(g_d);
    adamdebug_step_out(g_d);
    check("step out of it", wait_new_stop(serial, 3000));
    regs = regs_now();
    check("...to the return address with the stack popped", regs.pc == 0x8035 && regs.sp == (uint16_t)(before.sp + 2));

    {
        adamdebug_io iostate, after;
        adamdebug_io_get(g_d, &iostate);
        serial = adamdebug_stop_serial(g_d);
        adamdebug_frame(g_d, 1);
        check("frame", wait_new_stop(serial, 3000));
        adamdebug_io_get(g_d, &after);
        snap = snap_now();
        check("it ended the frame", after.frames == iostate.frames + 1 && after.beam_line == 0 && after.beam_tstate == 0);
        check("reason frame", snap.stop.reason == ADAMDEBUG_STOP_FRAME && strstr(snap.text, "frame at $") == snap.text);
        serial = adamdebug_stop_serial(g_d);
        adamdebug_scanline(g_d, 3);
        check("scanline", wait_new_stop(serial, 3000));
        snap = snap_now();
        check("reason scanline", snap.stop.reason == ADAMDEBUG_STOP_SCANLINE && strstr(snap.text, "scanline at $") == snap.text);
    }

    printf("a stop after the engine was told to step reports through the callback path:\n");
    {
        adamdebug_snapshot first = snap_now(), second;
        adamdebug_step(g_d);
        wait_new_stop(first.serial, 3000);
        second = snap_now();
        check("the serial moves with every stop", second.serial > first.serial);
        check("a snapshot is one critical section (paused, serial, text agree)",
              second.paused && second.stop.serial == second.serial && second.address == second.stop.pc);
    }
}

static void test_registers(void)
{
    adamcore_z80_regs regs, back;
    char err[200];
    long value;

    printf("registers:\n");
    regs = regs_now();
    regs.b = 0x42;
    regs.ix = 0x1234;
    regs.a2 = 0x11;
    regs.f2 = 0x22;
    regs.iff1 = 1;
    regs.iff2 = 0;
    regs.im = 2;
    check("regs_set while stopped", adamdebug_regs_set(g_d, &regs, err, sizeof err) == 1);
    back = regs_now();
    check("main, index, shadow, IFF and IM took", back.b == 0x42 && back.ix == 0x1234 && back.a2 == 0x11 &&
                                                        back.f2 == 0x22 && back.iff1 == 1 && back.im == 2);
    regs.im = 3;
    check("IM above 2 is refused", adamdebug_regs_set(g_d, &regs, err, sizeof err) == 0 && strstr(err, "IM"));
    regs.im = 2;
    regs.iff1 = 2;
    check("IFF is 0 or 1", adamdebug_regs_set(g_d, &regs, err, sizeof err) == 0);
    check("a register by name", adamdebug_register_set(g_d, "bc'", 0xBEEF, err, sizeof err) == 1 &&
                                    regs_now().b2 == 0xBE && regs_now().c2 == 0xEF);
    check("a flag by name", adamdebug_register_set(g_d, "zf", 1, err, sizeof err) == 1 && (regs_now().f & 0x40));
    check("...and cleared", adamdebug_register_set(g_d, "zf", 0, err, sizeof err) == 1 && !(regs_now().f & 0x40));
    check("a half register", adamdebug_register_set(g_d, "ixl", 0x99, err, sizeof err) == 1 && regs_now().ix == 0x1299);
    check("an unknown register", adamdebug_register_set(g_d, "xyz", 1, err, sizeof err) == 0 && strstr(err, "unknown"));
    check("eval sees it", adamdebug_eval(g_d, "bc'", &value, err, sizeof err) == 0 && value == 0xBEEF);
    check("IM through register_set is range-checked", adamdebug_register_set(g_d, "im", 3, err, sizeof err) == 0);
    {
        uint64_t serial = adamdebug_stop_serial(g_d);
        adamdebug_register_set(g_d, "pc", 0x8030, err, sizeof err);
        check("a PC edit moves the stop's address but is not a new stop",
              snap_now().address == 0x8030 && adamdebug_stop_serial(g_d) == serial);
    }

    printf("while running:\n");
    adamdebug_resume(g_d);
    nap(30);
    back = regs_now();
    check("regs_set is refused", adamdebug_regs_set(g_d, &back, err, sizeof err) == 0 && strstr(err, "running"));
    check("search is refused", adamdebug_search(g_d, (const uint8_t *)"\xF3", 1, 0xFF, 0, 0, 0xFFFF,
                                                (uint16_t[4]){ 0 }, 4) == -1);
    check("while running the stop reads running", snap_now().paused == 0 && !strcmp(snap_now().text, "running"));
    pause_machine();
}

static void test_trace(void)
{
    adamdebug_trace rows[40];
    adamhist_entry hist[40];
    char text[160];
    int count, idx, bp_id;
    uint8_t loop_b;

    printf("trace and history (state before each instruction):\n");
    adamdebug_cpu_set(g_d, ADAM_REG_PC, LOOPTOP);
    adamdebug_trace_enable(g_d, 1);
    adamdebug_history_enable(g_d, 1);
    check("both asked for", adamdebug_trace_enabled(g_d) && adamdebug_history_enabled(g_d));
    adamdebug_trace_clear(g_d);
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_EXEC, LOOPTOP, LOOPTOP, "", NULL, 0);
    check("run to a breakpoint", go_and_wait() && regs_now().pc == LOOPTOP);
    adamdebug_trace_clear(g_d);
    check("and once round the loop", go_and_wait() && regs_now().pc == LOOPTOP);
    loop_b = regs_now().b;
    count = adamdebug_trace_read(g_d, rows, 40);
    check("records exist", count >= 10);
    check("the stop at $802A did not record an instruction that never ran", rows[0].pc == 0x8039);
    {
        static const uint16_t k_expect[] = { 0x8039, 0x8037, 0x8035, 0x8051, 0x8050, 0x8032, 0x802F, 0x802C, 0x802B, 0x802A };
        int same = 1;
        for (idx = 0; idx < 10; idx++)
            if (rows[idx].pc != k_expect[idx]) same = 0;
        check("newest first: the loop, in reverse", same);
    }
    check("the registers are the state BEFORE the instruction (INC B)",
          (rows[9].bc >> 8) == (uint8_t)(loop_b - 1) && (rows[8].bc >> 8) == loop_b);
    check("records carry their bytes and a length", rows[0].len == 3 && rows[0].bytes[0] == 0xC3 && rows[2].len == 2);
    check("cycles increase", rows[0].cycles > rows[9].cycles);
    check("sp is recorded", rows[5].sp == 0x6400 && rows[3].sp == 0x63FE);
    check("the beam line is recorded", rows[0].line >= 0 && rows[0].line < 262);
    adamdebug_trace_format(&rows[0], text, sizeof text);
    check("a trace line reads like the disassembly", strstr(text, "JP $802A") != NULL && strncmp(text, "8039", 4) == 0);
    printf("      %s\n", text);
    check("count and total", adamdebug_trace_count(g_d) >= 10 && adamdebug_trace_total(g_d) >= 10);
    check("branch filter keeps JP/CALL/RET",
          adamdebug_trace_match(&rows[0], ADAMDEBUG_TRACE_BRANCHES, 0, 0) &&
          !adamdebug_trace_match(&rows[9], ADAMDEBUG_TRACE_BRANCHES, 0, 0) &&
          adamdebug_trace_match(&rows[3], ADAMDEBUG_TRACE_BRANCHES, 0, 0));
    check("range filter", adamdebug_trace_match(&rows[4], ADAMDEBUG_TRACE_RANGE, 0x8050, 0x805F) &&
                          !adamdebug_trace_match(&rows[0], ADAMDEBUG_TRACE_RANGE, 0x8050, 0x805F));

    count = adamdebug_history_copy(g_d, hist, 40);
    check("history is the same ring, oldest first",
          count >= 10 && count <= 40 && hist[count - 1].pc == 0x8039 && hist[count - 2].pc == 0x8037);
    adamdebug_history_format(&hist[count - 1], text, sizeof text);
    check("a history line has the flags", strstr(text, "JP $802A") != NULL && strstr(text, "SP=6400") != NULL);
    check("history match", adamdebug_history_match(&hist[count - 1], ADAMDEBUG_TRACE_BRANCHES, 0, 0));

    adamdebug_trace_clear(g_d);
    check("clear", adamdebug_trace_count(g_d) == 0 && adamdebug_trace_total(g_d) == 0);
    adamdebug_breakpoint_remove(g_d, bp_id);
    adamdebug_trace_enable(g_d, 0);
    check("trace off, history still wanted", !adamdebug_trace_enabled(g_d) && adamdebug_history_enabled(g_d));
    adamdebug_history_enable(g_d, 0);
    check("both off", !adamdebug_trace_enabled(g_d) && !adamdebug_history_enabled(g_d));

    printf("a single step is recorded:\n");
    adamdebug_trace_enable(g_d, 1);
    adamdebug_trace_clear(g_d);
    {
        const uint16_t start_pc = regs_now().pc;
        const uint64_t serial = adamdebug_stop_serial(g_d);
        adamdebug_step(g_d);
        wait_new_stop(serial, 3000);
        count = adamdebug_trace_read(g_d, rows, 4);
        check("the stepped instruction is in the ring", count == 1 && rows[0].pc == start_pc);
        adamdebug_step(g_d);
        wait_new_stop(adamdebug_stop_serial(g_d), 3000);
        nap(10);
        count = adamdebug_trace_read(g_d, rows, 4);
        check("and every step adds one, no more", count == 2);
    }
    adamdebug_trace_enable(g_d, 0);
}

static void test_machine_getters(void)
{
    adamdebug_io iostate;
    adamdebug_cart cart;
    adamdebug_region map[16];
    adamdebug_sound sound;
    char text[4096];
    int count, idx, gapfree = 1;

    printf("controllers, VDP, CPU and the map:\n");
    /* up + left fire on port 1, key 5 */
    {
        uint16_t word = 0x7F7F;
        word = (uint16_t)(word & ~0x0100 & ~0x4000);
        word = (uint16_t)((word & 0xFFF0) | 5);
        adamcore_set_joystick(g_s->core, 0, word);
    }
    check("the engine reads the machine", adamdebug_io_get(g_d, &iostate) && iostate.valid);
    check("up and the left fire", iostate.pad[0].up && iostate.pad[0].fire_left && !iostate.pad[0].down && !iostate.pad[0].fire_right);
    check("key 5", iostate.pad[0].keypad == 5 && !strcmp(iostate.pad[0].keypad_name, "5"));
    check("what the console reads in each half", iostate.pad[0].joystick_byte == 0xBE && iostate.pad[0].keypad_byte == 0xF3);
    check("the idle pad has no key", iostate.pad[1].keypad == -1 && iostate.pad[1].joystick_byte == 0xFF);
    check("the strobe is a defined half", iostate.joy_mode == 0 || iostate.joy_mode == 1);
    check("beam and frames", iostate.beam_line >= 0 && iostate.beam_line < 262 && iostate.frames > 0);
    check("a stock console: no SGM", !iostate.sgm);
    check("the memory configuration", iostate.mem_ctrl == 0x0F && iostate.lower == 3 && iostate.upper == 3 && iostate.cv_console && iostate.game_mode);
    check("no FujiNet runtime in this session", !iostate.fujinet_runtime && !iostate.fujinet_link);
    adamdebug_io_state(g_d, text, sizeof text);
    check("the text form", strstr(text, "Controllers") && strstr(text, "key 5") && strstr(text, "VDP") &&
                           strstr(text, "$7F=0F") && strstr(text, "FujiNet      not running"));
    adamcore_set_joystick(g_s->core, 0, 0x7F7F);

    count = adamdebug_memory_map(g_d, map, 16);
    check("a memory map", count >= 4);
    for (idx = 0; idx < count; idx++) {
        const unsigned expect = idx == 0 ? 0 : (unsigned)map[idx - 1].high + 1;
        if (map[idx].low != expect || map[idx].high < map[idx].low) gapfree = 0;
    }
    check("it starts at $0000 and ends at $FFFF with no gaps", gapfree && map[0].low == 0 && map[count - 1].high == 0xFFFF);
    check("OS7, open bus, the 1K RAM and its mirror, the cartridge window",
          strstr(map[0].name, "OS7") && map[0].high == 0x1FFF && !map[0].writable &&
          strstr(map[1].name, "open bus") && !map[1].readable && map[2].low == 0x6000 &&
          strstr(map[2].name, "1K") && map[2].high == 0x63FF && strstr(map[3].name, "mirror") &&
          map[3].high == 0x7FFF && strstr(map[4].name, "Cartridge") && strstr(map[4].name, "512 bytes"));
    for (idx = 0; idx < count; idx++)
        printf("      $%04X-$%04X  %s\n", map[idx].low, map[idx].high, map[idx].name);

    adamdebug_cart_info(g_d, &cart);
    check("cart info", cart.valid && !strcmp(cart.machine, "ColecoVision") && !cart.sgm && cart.cartridge &&
                       cart.cart_size == 512 && cart.cart_visible && cart.cart_header == 2 && cart.mem_ctrl == 0x0F);
    adamdebug_cart_summary(g_d, text, sizeof text);
    check("cart summary", strstr(text, "ColecoVision") && strstr(text, "loop.rom (512 bytes)") && strstr(text, "no FujiNet"));
    printf("      %s\n", text);

    check("sound getters", adamdebug_sound_get(g_d, &sound) && sound.sn.present && !sound.ay.present);
    check("the SN76489 is at its reset values: every channel muted", sound.sn.atten[0] == 15 && sound.sn.level[0] == 0);
}

static void test_search_and_memtools(void)
{
    uint16_t hits[8], found = 0;
    static const uint8_t pat[] = { 0xF3, 0x31, 0x00, 0x64 };
    static const uint8_t jump_pat[] = { 0xC3, 0x2A, 0x80 };
    char row[160];
    int count;

    printf("search, watches and symbols against the live machine:\n");
    count = adamdebug_search(g_d, pat, 4, 0xFF, 0, 0x8000, 0x80FF, hits, 8);
    check("find the program", count == 1 && hits[0] == 0x8024);
    check("find with a range", adamdebug_search(g_d, jump_pat, 3, 0xFF, 0, 0x8000, 0x80FF, hits, 8) == 1 && hits[0] == 0x8039);
    /* the 512-byte image is mirrored through the 32K window */
    check("search next goes on to the mirror", adamdebug_search_next(g_d, jump_pat, 3, 0xFF, 0, 0x8039, 0, &found) == 1 && found == 0x8239);
    check("and wraps round the end", adamdebug_search_next(g_d, jump_pat, 3, 0xFF, 0, 0xFFF0, 0, &found) == 1 && found == 0x8039);
    check("search backward", adamdebug_search_next(g_d, jump_pat, 3, 0xFF, 0, 0x803A, 1, &found) == 1 && found == 0x8039);
    check("a mask", adamdebug_search(g_d, (const uint8_t *)"\x31", 1, 0xF0, 0, 0x8024, 0x8030, hits, 8) >= 1);
    check("not found", adamdebug_search(g_d, (const uint8_t *)"\x12\x34\x56\x78\x9A", 5, 0xFF, 0, 0x8000, 0xFFFF, hits, 8) == 0);

    adamwatch_clear();
    adamwatch_add("$8024");
    adamwatch_add("$8025-$8028");
    adamwatch_format(g_d, 0, row, sizeof row);
    check("a watch shows its value while stopped", strstr(row, "$F3 (243)") != NULL);
    adamwatch_format(g_d, 1, row, sizeof row);
    check("a range shows its first bytes", strstr(row, "31 00 64") != NULL);
    adamwatch_clear();

    {
        char out[900];
        adamdebug_command(g_d, "mem $8024 8", out, sizeof out);
        check("mem", strstr(out, "8024  F3 31 00 64") != NULL);
        adamdebug_command(g_d, "disasm 8024 3", out, sizeof out);
        check("disasm, a bare hex address", strstr(out, "DI") != NULL && strstr(out, "LD SP,$6400") != NULL);
        adamdebug_command(g_d, "find F3 31", out, sizeof out);
        check("find", strstr(out, "$8024") != NULL);
        adamdebug_command(g_d, "poke 6100 12 $34", out, sizeof out);
        adamdebug_command(g_d, "mem 6100 2", out, sizeof out);
        check("poke then mem (bare hex address, decimal value 12)", strstr(out, "6100  0C 34") != NULL);
        adamdebug_command(g_d, "map", out, sizeof out);
        check("map", strstr(out, "OS7 ROM") != NULL);
        adamdebug_command(g_d, "sound", out, sizeof out);
        check("sound", strstr(out, "SN76489") != NULL);
        adamdebug_command(g_d, "cart", out, sizeof out);
        check("cart", strstr(out, "ColecoVision") != NULL);
        adamdebug_command(g_d, "step 3", out, sizeof out);
        check("step n from the console", strstr(out, "$") != NULL && adamdebug_is_stopped(g_d));
        adamdebug_command(g_d, "set b 7", out, sizeof out);
        check("set from the console", strstr(out, "AF=") != NULL && regs_now().b == 7);
        adamdebug_command(g_d, "set ixh 5", out, sizeof out);
        check("set a half register", regs_now().ix >> 8 == 5);
        adamdebug_command(g_d, "runto 8050", out, sizeof out);
        check("runto from the console waits for the stop", strstr(out, "$8050") != NULL && regs_now().pc == SUB);
        adamdebug_command(g_d, "over", out, sizeof out);
        check("over from the console", adamdebug_is_stopped(g_d));
        adamdebug_command(g_d, "frame", out, sizeof out);
        check("frame from the console", strstr(out, "frame") != NULL);
        adamdebug_command(g_d, "hits", out, sizeof out);
        check("hits", strstr(out, "no breakpoints") != NULL);
    }
}

typedef struct {
    atomic_int quit;
    atomic_uint polls;
} poller;

static void *poll_thread(void *arg)
{
    poller *ctx = arg;
    adamdebug_snapshot snap;
    adamdebug_region map[16];
    adamdebug_breakpoint list[8];
    adamdebug_line lines[8];
    adamdebug_cpu cpu;
    adamdebug_io iostate;
    adamdebug_cart cart;
    adamdebug_sound sound;
    adamdebug_memmap memcfg;
    uint8_t bytes[64];
    char text[512];
    long value;

    while (!atomic_load(&ctx->quit)) {
        adamdebug_stop_snapshot(g_d, &snap);
        adamdebug_cpu_get(g_d, &cpu);
        adamdebug_read(g_d, 0x6000, bytes, sizeof bytes);
        adamdebug_memory_map(g_d, map, 16);
        adamdebug_memmap_get(g_d, &memcfg);
        adamdebug_breakpoint_list(g_d, list, 8);
        adamdebug_disassemble(g_d, 0x8024, lines, 8, NULL);
        adamdebug_io_get(g_d, &iostate);
        adamdebug_cart_info(g_d, &cart);
        adamdebug_sound_get(g_d, &sound);
        adamdebug_eval(g_d, "[hl] + sp", &value, text, sizeof text);
        adamdebug_vdp_read(g_d, 0, bytes, sizeof bytes);
        adamdebug_stop_text(g_d, text, sizeof text, NULL);
        adamdebug_io_state(g_d, text, sizeof text);
        atomic_fetch_add(&ctx->polls, 1);
    }
    return NULL;
}

static void test_session_restart(void)
{
    poller ctx;
    pthread_t thread;
    adamdebug_breakpoint list[8];
    int round, bp_id, passed = 1;
    unsigned before_polls;

    printf("breakpoints, hooks and trace survive a session restart; polling never touches a freed core:\n");
    memset(&ctx, 0, sizeof ctx);
    adamdebug_breakpoint_clear(g_d);
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_EXEC, LOOPTOP, LOOPTOP, "", NULL, 0);
    adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_WRITE, 0x6000, 0x6000, "value == 9", NULL, 0);
    adamdebug_trace_enable(g_d, 1);
    adamdebug_resume(g_d);
    pthread_create(&thread, NULL, poll_thread, &ctx);

    for (round = 0; round < 6; round++) {
        if (round % 2 == 1) {
            /* stopped (parked) when the machine is torn down: must not hang */
            if (!pause_machine()) passed = 0;
        } else {
            nap(30);
        }
        adamsession_stop(g_s);
        if (!adamdebug_is_attached(g_d)) passed = 0;
        if (start_cv() != 0) { passed = 0; break; }
        /* the attached engine stops the new machine at its first instruction */
        if (!wait_stopped(3000)) { passed = 0; break; }
        if (adamdebug_stop_serial(g_d) == 0 || regs_now().pc != 0) passed = 0;
        adamdebug_cpu_set(g_d, ADAM_REG_PC, LOOPTOP);
        if (!go_and_wait()) { passed = 0; break; }
        {
            const adamdebug_snapshot snap = snap_now();
            if (snap.stop.reason != ADAMDEBUG_STOP_BREAKPOINT || snap.stop.pc != LOOPTOP) passed = 0;
        }
    }
    check("six restarts, half of them from a parked machine, each ends at the breakpoint", passed);
    check("the table came through", adamdebug_breakpoint_list(g_d, list, 8) == 2 && list[0].id == bp_id);
    check("the trace is still on", adamdebug_trace_enabled(g_d));
    {
        adamdebug_trace rows[4];
        adamdebug_resume(g_d);
        wait_new_stop(adamdebug_stop_serial(g_d), 3000);
        check("and recording in the new machine", adamdebug_trace_read(g_d, rows, 4) >= 1);
    }
    before_polls = atomic_load(&ctx.polls);
    nap(50);
    check("the poller kept running through it all", atomic_load(&ctx.polls) > before_polls);
    atomic_store(&ctx.quit, 1);
    pthread_join(thread, NULL);
    adamdebug_trace_enable(g_d, 0);
    adamdebug_breakpoint_clear(g_d);
    pause_machine();
}

static void test_vdp_write_break(void)
{
    adamdebug_snapshot snap;
    int bp_id;

    printf("a Break-on-writes condition on the VDP (vdpaddr, vdpsecond):\n");
    adamdebug_breakpoint_clear(g_d);
    check("start the VDP program", boot_at(VDPLOOP));
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_OUT, 0xA0, 0xBF,
                                  "(addr&1)==0&&vdpaddr>=0&&vdpaddr<=0x10", NULL, 0);
    check("the condition is accepted", bp_id > 0);
    check("it stops", go_and_wait());
    snap = snap_now();
    check("on the data port with $55", snap.stop.reason == ADAMDEBUG_STOP_WATCH_OUT &&
                                       snap.stop.addr == 0xA0 && snap.stop.value == 0x55);
    adamdebug_breakpoint_remove(g_d, bp_id);
    bp_id = adamdebug_breakpoint_add(g_d, ADAMDEBUG_BP_OUT, 0xA0, 0xBF,
                                  "(addr&1)==1&&vdpsecond&&value==0x40", NULL, 0);
    check("a register-pair condition", bp_id > 0);
    check("it stops on the second control byte", go_and_wait());
    snap = snap_now();
    check("at $A1 with $40", snap.stop.addr == 0xA1 && snap.stop.value == 0x40);
    adamdebug_breakpoint_remove(g_d, bp_id);
}

static void test_halt_and_sound(void)
{
    adamdebug_sound sound;
    adamdebug_trace rows[200];
    char err[200];
    int count;

    printf("a halted machine and the SN76489:\n");
    adamdebug_trace_enable(g_d, 1);
    check("start program B", boot_at(SOUND));
    adamdebug_trace_clear(g_d);
    adamdebug_resume(g_d);
    nap(250);
    check("pause a halted machine", pause_machine());
    check("the CPU is halted", regs_now().halted == 1);

    count = adamdebug_trace_read(g_d, rows, 200);
    check("a HALT flood collapses into a handful of records", count > 5 && count < 100);
    check("...with a spin count on the last", rows[0].halted == 1 && rows[0].repeat > 1000);
    check("...after the HALT itself ran", rows[1].bytes[0] == 0x76 && rows[1].halted == 0);
    {
        char text[160];
        adamdebug_history_format(&rows[0], text, sizeof text);
        check("...and the history line says how many", strstr(text, "HALT") && strstr(text, " x"));
    }

    check("sound getters", adamdebug_sound_get(g_d, &sound) == 1);
    check("tone 1 period $4E", sound.sn.period[0] == 0x4E);
    check("...and its frequency", sound.sn.freq[0] > 1434.0 && sound.sn.freq[0] < 1436.0);
    check("tone 1 loudest, tone 2 and noise muted", sound.sn.atten[0] == 0 && sound.sn.level[0] == 15 &&
                                                    sound.sn.atten[1] == 15 && sound.sn.level[1] == 0 &&
                                                    sound.sn.atten[3] == 15);
    check("noise: white, rate 1", sound.sn.noise_white && sound.sn.noise_rate == 1);
    check("no AY without the Super Game Module", !sound.ay.present);

    check("a PC edit while halted warns", adamdebug_register_set(g_d, "pc", 0x8010, err, sizeof err) == 1 &&
                                              strstr(err, "halted") != NULL);
    check("...and the CPU stays halted", regs_now().halted == 1);
    {
        adamcore_z80_regs regs = regs_now();
        regs.halted = 0;
        check("regs_set clears halted", adamdebug_regs_set(g_d, &regs, err, sizeof err) == 1 && regs_now().halted == 0);
    }
    adamdebug_trace_enable(g_d, 0);
}

/* The ADAM memory configuration: ports $7F (lower and upper bank) and $3F (EOS
 * ROM), each driven by the guest's own OUT. */
static void to_config(uint8_t mem_ctrl, uint8_t net_ctrl)
{
    static const uint8_t k_stub_fmt[] = { 0x3E, 0, 0xD3, 0x3F, 0x3E, 0, 0xD3, 0x7F, 0x18, 0xFE };
    uint8_t stub[sizeof k_stub_fmt];
    adamdebug_memmap memcfg;
    uint16_t where;

    memcpy(stub, k_stub_fmt, sizeof stub);
    stub[1] = net_ctrl;
    stub[5] = mem_ctrl;
    adamdebug_memmap_get(g_d, &memcfg);
    /* a RAM place the CPU can run from right now */
    if (memcfg.lower == 1 || memcfg.lower == 2) where = 0x0100;
    else if (memcfg.lower == 3) where = 0x2100;
    else where = 0x9000;
    if (memcfg.lower == 0 && memcfg.upper != 0 && memcfg.upper != 2) {
        /* nowhere writable: take the power-on state's upper RAM by force */
        where = 0x9000;
    }
    {
        size_t idx;
        for (idx = 0; idx < sizeof stub; idx++)
            adamdebug_write(g_d, (uint16_t)(where + idx), stub[idx]);
    }
    adamdebug_cpu_set(g_d, ADAM_REG_PC, where);
    {
        int idx;
        for (idx = 0; idx < 4; idx++) {
            const uint64_t serial = adamdebug_stop_serial(g_d);
            adamdebug_step(g_d);
            wait_new_stop(serial, 3000);
        }
    }
}

static int region_has(const adamdebug_region *map, int count, uint16_t low, uint16_t high, const char *name)
{
    int idx;
    for (idx = 0; idx < count; idx++)
        if (map[idx].low == low && map[idx].high == high && strstr(map[idx].name, name))
            return 1;
    return 0;
}

static void test_adam_memory_map(void)
{
    adamsession_start_opts opts;
    adamdebug_region map[16];
    int count, idx, gapfree;
    adamdebug_memmap memcfg;

    printf("the ADAM memory configuration, gap-free in every bank combination:\n");
    adamsession_stop(g_s);
    memset(&opts, 0, sizeof opts);
    opts.machine = 0;
    opts.joystick_mode = 1;
    check("start an ADAM", adamsession_start(g_s, &opts) == 0);
    check("the attached engine stops it", wait_stopped(3000));
    adamdebug_memmap_get(g_d, &memcfg);
    printf("      power-on: $7F=%02X $3F=%02X\n", memcfg.mem_ctrl, memcfg.net_ctrl);
    /* the first OUT has to run from where the machine has RAM: upper bank 0 */
    if (memcfg.upper != 0) {
        printf("      (the power-on upper bank is %d, not RAM: skipping the OUT-driven combinations)\n", memcfg.upper);
        check("ADAM mode is not a ColecoVision", !memcfg.cv_console);
        return;
    }
    {
        static const struct {
            uint8_t mem, net;
            int regions;
            struct { uint16_t low, high; const char *name; } expect[4];
        } k_cases[] = {
            { 0x00, 0x00, 2, { { 0x0000, 0x7FFF, "SmartWriter ROM" }, { 0x8000, 0xFFFF, "upper bank 0" } } },
            { 0x00, 0x02, 3, { { 0x0000, 0x5FFF, "SmartWriter ROM" }, { 0x6000, 0x7FFF, "EOS ROM" }, { 0x8000, 0xFFFF, "upper bank 0" } } },
            { 0x01, 0x00, 2, { { 0x0000, 0x7FFF, "lower bank 1" }, { 0x8000, 0xFFFF, "upper bank 0" } } },
            { 0x02, 0x00, 2, { { 0x0000, 0x7FFF, "Expansion RAM" }, { 0x8000, 0xFFFF, "upper bank 0" } } },
            { 0x03, 0x00, 3, { { 0x0000, 0x1FFF, "OS7 ROM" }, { 0x2000, 0x7FFF, "24K" }, { 0x8000, 0xFFFF, "upper bank 0" } } },
            { 0x09, 0x00, 2, { { 0x0000, 0x7FFF, "lower bank 1" }, { 0x8000, 0xFFFF, "Expansion RAM" } } },
            { 0x05, 0x00, 2, { { 0x0000, 0x7FFF, "lower bank 1" }, { 0x8000, 0xFFFF, "open bus" } } },
            { 0x0D, 0x00, 2, { { 0x0000, 0x7FFF, "lower bank 1" }, { 0x8000, 0xFFFF, "Cartridge (none" } } },
        };
        size_t case_idx;
        for (case_idx = 0; case_idx < sizeof k_cases / sizeof k_cases[0]; case_idx++) {
            char what[96];
            int match = 1;
            size_t expect_idx;

            to_config(k_cases[case_idx].mem, k_cases[case_idx].net);
            adamdebug_memmap_get(g_d, &memcfg);
            snprintf(what, sizeof what, "$7F=%02X $3F=%02X took", k_cases[case_idx].mem, k_cases[case_idx].net);
            check(what, memcfg.mem_ctrl == k_cases[case_idx].mem && (memcfg.net_ctrl & 3) == k_cases[case_idx].net);
            count = adamdebug_memory_map(g_d, map, 16);
            gapfree = count > 0 && map[0].low == 0 && map[count - 1].high == 0xFFFF;
            for (idx = 1; idx < count; idx++)
                if ((unsigned)map[idx - 1].high + 1 != map[idx].low) gapfree = 0;
            snprintf(what, sizeof what, "  map for $7F=%02X $3F=%02X is gap-free with %d regions", k_cases[case_idx].mem,
                     k_cases[case_idx].net, k_cases[case_idx].regions);
            for (expect_idx = 0; expect_idx < (size_t)k_cases[case_idx].regions; expect_idx++)
                if (!region_has(map, count, k_cases[case_idx].expect[expect_idx].low, k_cases[case_idx].expect[expect_idx].high, k_cases[case_idx].expect[expect_idx].name))
                    match = 0;
            check(what, gapfree && count == k_cases[case_idx].regions && match);
            if (!(gapfree && count == k_cases[case_idx].regions && match))
                for (idx = 0; idx < count; idx++)
                    printf("        $%04X-$%04X  %s\n", map[idx].low, map[idx].high, map[idx].name);
        }
        {
            adamdebug_io iostate;
            char text[4096];
            adamdebug_io_get(g_d, &iostate);
            adamdebug_io_state(g_d, text, sizeof text);
            check("the io text names the configuration", strstr(text, "$7F=0D") && strstr(text, "ADAM"));
        }
    }
}

int main(void)
{
    char tmpl[512];
    char *tmp = adam_make_tempdir(tmpl, sizeof(tmpl), "adam-engine-");
    adamsession_paths paths;

    if (!tmp) return 1;
    memset(&paths, 0, sizeof paths);
    paths.config_dir = tmp;
    paths.data_dir = tmp;
    paths.fujinet_lib = ""; /* disabled */
    snprintf(g_cart, sizeof g_cart, "%s/loop.rom", tmp);
    build_cart(g_cart);

    g_s = adamsession_new(&paths);
    g_d = g_s ? adamsession_debugger(g_s) : NULL;
    check("the engine is created on demand", g_d != NULL);
    if (!g_d) return 1;

    adamdebug_attach(g_d);
    if (start_cv() != 0) {
        fprintf(stderr, "engine_test: could not start: %s\n", adamsession_last_error(g_s));
        return 1;
    }
    check("the attached engine stops the machine as it comes up", wait_stopped(3000) && regs_now().pc == 0);
    adamdebug_cpu_set(g_d, ADAM_REG_PC, LOOP);
    check("pointed at the loop", regs_now().pc == LOOP);

    test_expressions();
    test_pause_and_breakpoints();
    test_watchpoints();
    test_stepping();
    test_registers();
    test_trace();
    test_machine_getters();
    test_search_and_memtools();
    test_session_restart();
    test_vdp_write_break();
    test_halt_and_sound();
    test_adam_memory_map();

    adamdebug_detach(g_d);
    adamsession_stop(g_s);
    adamsession_free(g_s);
    if (failures) {
        fprintf(stderr, "\nengine_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nengine_test: PASS\n");
    return 0;
}
