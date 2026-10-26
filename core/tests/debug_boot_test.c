/*
 * debug_boot_test -- the debugger contract (core/include/adamdebug.h)
 * against a running machine, headless (no FujiNet, audio or gamepad).
 *
 * Phase 1 is a ColecoVision session with a hand-assembled cartridge, the
 * debugger attached before the session starts (so it stops at the reset
 * vector) and the PC then pointed at the cartridge's loop; it needs no real
 * ROMs. It covers attach/stop/resume and the generation counter, registers
 * and flags, step / over (CALL and DJNZ) / out / run-to / scanline / frame,
 * execute breakpoints with conditions and hit counts, memory read and write
 * and port IN/OUT watchpoints (they stop AFTER the accessing instruction and
 * say so), disassembly flags, labels and symbol files in every format, the
 * memory map text, the trace ring, Save, the console and its completion,
 * detach (nothing left in the core), and that breakpoints and the attach
 * survive a restart.
 *
 * Phase 2 boots the real ADAM (skipped without the system ROMs): built-in
 * EOS/OS7 symbols, the memory map, trace while running, the VDP snapshot and
 * VRAM pokes, and a detach that lets it run.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "adamcore_debug.h"
#include "adamdebug.h"
#include "adamsession.h"
#include "roms_embedded.h"
#include "session_internal.h"
#include "test_tmp.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_stopped(adamdebug *d, int want, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        if (!!adamdebug_is_stopped(d) == want) return 1;
        sleep_ms(2);
        waited += 2;
    }
    return 0;
}

/* Resume and wait for the next stop. */
static int run_until_stop(adamdebug *d, int timeout_ms)
{
    unsigned g = adamdebug_generation(d);
    int waited = 0;
    adamdebug_resume(d);
    while (waited < timeout_ms) {
        if (adamdebug_is_stopped(d) && adamdebug_generation(d) != g) return 1;
        sleep_ms(2);
        waited += 2;
    }
    return 0;
}

/* Issue a stepping call and wait for the machine to stop again. */
#define STEPPED(d, call) ( { unsigned g_ = adamdebug_generation(d); \
    int w_ = 0, ok_ = 0; call; \
    while (w_ < 5000) { \
        if (adamdebug_is_stopped(d) && adamdebug_generation(d) != g_) { ok_ = 1; break; } \
        sleep_ms(2); w_ += 2; } \
    ok_; } )

static int pc_of(adamdebug *d)
{
    adamdebug_cpu c;
    adamdebug_cpu_get(d, &c);
    return c.pc;
}

static int reason_has(adamdebug *d, const char *what)
{
    char why[128];
    int addr;
    adamdebug_stop_reason(d, why, sizeof why, &addr);
    return strstr(why, what) != NULL;
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* stop callback bookkeeping */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_stops, g_kind, g_pc;

static void on_stop(void *ud, int kind, uint16_t pc)
{
    (void)ud;
    pthread_mutex_lock(&g_mtx);
    g_stops++;
    g_kind = kind;
    g_pc = pc;
    pthread_mutex_unlock(&g_mtx);
}

/* The cartridge: a ColecoVision image whose program loops forever,
 *   8024  LD SP,$6400
 *   8027  LD HL,$6000
 *   802A  XOR A
 *   802B  LD B,3
 *   802D  CALL $8050     ; INC C / RET
 *   8030  DJNZ $802D
 *   8032  INC A
 *   8033  LD (HL),A      ; write $6000
 *   8034  OUT ($FF),A    ; PSG
 *   8036  IN A,($FC)     ; controller 1
 *   8038  LD A,(HL)      ; read $6000
 *   8039  JP $802B
 * The reset and NMI vectors return, so nothing the VDP does can derail it. */
static void make_cart(const char *path)
{
    static const uint8_t prog[] = {
        0x31, 0x00, 0x64, 0x21, 0x00, 0x60, 0xAF, 0x06, 0x03,
        0xCD, 0x50, 0x80, 0x10, 0xFB, 0x3C, 0x77, 0xD3, 0xFF,
        0xDB, 0xFC, 0x7E, 0xC3, 0x2B, 0x80,
    };
    uint8_t img[128];
    FILE *f;
    int i;

    memset(img, 0x00, sizeof img);
    img[0] = 0x55;            /* ColecoVision: skip the title screen */
    img[1] = 0xAA;
    img[0x0A] = 0x24;         /* start address $8024 */
    img[0x0B] = 0x80;
    for (i = 0x0C; i < 0x21; i++)
        img[i] = 0xC9;        /* RST vectors: RET */
    img[0x21] = 0xED;         /* NMI vector: RETN */
    img[0x22] = 0x45;
    memcpy(img + 0x24, prog, sizeof prog);
    img[0x50] = 0x0C;         /* INC C */
    img[0x51] = 0xC9;         /* RET */
    f = fopen(path, "wb");
    if (f) {
        fwrite(img, 1, sizeof img, f);
        fclose(f);
    }
}

static void start_cv(adamsession *s, const char *cart)
{
    adamsession_start_opts o;
    memset(&o, 0, sizeof o);
    o.machine = 1;
    o.cart_path = cart;
    o.joystick_mode = 1;
    if (adamsession_start(s, &o) != 0)
        fprintf(stderr, "debug_boot: start failed: %s\n",
                adamsession_last_error(s));
}

static int phase_cv(adamsession *s, adamdebug *d, const char *dir)
{
    char cart[600], path[700], msg[300], out[8192];
    adamdebug_cpu c, c2;
    adamdebug_line lines[48];
    adamdebug_breakpoint list[8];
    char err[128];
    int pc_line = -1, n, i, id, id2;
    long v;
    uint8_t b[16];

    snprintf(cart, sizeof cart, "%s/loop.rom", dir);
    make_cart(cart);

    /* attach before the machine exists: it stops at the reset vector */
    adamdebug_set_stop_callback(d, on_stop, NULL);
    adamdebug_attach(d);
    check(adamdebug_is_attached(d), "attached before the session starts");
    check(!adamdebug_is_stopped(d), "nothing to stop yet");
    start_cv(s, cart);
    check(wait_stopped(d, 1, 3000), "the new machine stops as it comes up");
    adamdebug_cpu_get(d, &c);
    check(c.pc == 0x0000, "at the reset vector");
    check(g_stops >= 1 && g_kind == ADAMDBG_STOP_PAUSE && g_pc == 0,
          "the stop callback fired with kind PAUSE");
    {
        char why[128];
        int addr = 0;
        adamdebug_stop_reason(d, why, sizeof why, &addr);
        check(strcmp(why, "stopped") == 0 && addr == 0, "stop reason: stopped at $0000");
    }
    {
        adamdebug_memmap m;
        adamdebug_memmap_get(d, &m);
        check(m.mem_ctrl == 0x0F && m.lower == 3 && m.upper == 3 &&
              m.cv_console && m.game_mode,
              "memory map: ColecoVision game mode, OS7 low, cartridge high");
        adamdebug_io_state(d, out, sizeof out);
        check(strstr(out, "$7F=0F") && strstr(out, "OS7 ROM") &&
              strstr(out, "cartridge ROM") && strstr(out, "ColecoVision"),
              "the memory map text says so");
    }

    /* registers and flags */
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x8024);
    adamdebug_cpu_set(d, ADAM_REG_A, 0x42);
    adamdebug_cpu_set(d, ADAM_FLAG_C, 1);
    adamdebug_cpu_set(d, ADAM_REG_BC2, 0x1234);
    adamdebug_cpu_set(d, ADAM_REG_IM, 1);
    adamdebug_cpu_get(d, &c);
    check(c.pc == 0x8024 && (c.af >> 8) == 0x42 && c.cf == 1,
          "registers and flags are editable");
    check(c.bc2 == 0x1234 && c.im == 1, "the shadow set and IM too");
    adamdebug_cpu_set(d, ADAM_REG_A, 0);
    adamdebug_cpu_set(d, ADAM_FLAG_C, 0);

    /* memory */
    check(adamdebug_read(d, 0x8024, b, 4) == 4 && b[0] == 0x31 && b[2] == 0x64,
          "the cartridge reads through the map");
    adamdebug_write(d, 0x6010, 0x5A);
    adamdebug_read(d, 0x6010, b, 1);
    check(b[0] == 0x5A, "a RAM write lands");
    adamdebug_write(d, 0x8024, 0x00);
    adamdebug_read(d, 0x8024, b, 1);
    check(b[0] == 0x31, "a cartridge ROM write is ignored");

    /* single steps */
    check(STEPPED(d, adamdebug_step(d)) && pc_of(d) == 0x8027 && reason_has(d, "step"),
          "step: LD SP,$6400");
    adamdebug_cpu_get(d, &c);
    check(c.sp == 0x6400, "and SP is set");
    check(STEPPED(d, adamdebug_step(d)) && pc_of(d) == 0x802A, "step: LD HL,$6000");
    check(STEPPED(d, adamdebug_step(d)) && pc_of(d) == 0x802B, "step: XOR A");
    check(g_kind == ADAMDBG_STOP_STEP, "the callback says STEP");
    adamdebug_cpu_get(d, &c2);
    check(c2.cycles > c.cycles, "the cycle count moves");
    adamdebug_command(d, "step 3", out, sizeof out);
    check(pc_of(d) == 0x8051 && strstr(out, "step"), "the console: step 3 (LD B, CALL, INC C)");

    /* over a CALL: C counts once */
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x802D);
    adamdebug_cpu_set(d, ADAM_REG_C, 0);
    adamdebug_cpu_set(d, ADAM_REG_B, 3);
    check(STEPPED(d, adamdebug_step_over(d)) && pc_of(d) == 0x8030 &&
          reason_has(d, "step over"), "step over a CALL lands after it");
    adamdebug_cpu_get(d, &c);
    check((c.bc & 0xFF) == 1, "the callee ran once");
    /* over a DJNZ: through the whole loop (B=3: two more calls) */
    adamdebug_cpu_set(d, ADAM_REG_B, 3);
    adamdebug_cpu_set(d, ADAM_REG_C, 0);
    check(STEPPED(d, adamdebug_step_over(d)) && pc_of(d) == 0x8032,
          "step over a DJNZ runs the loop out");
    adamdebug_cpu_get(d, &c);
    check((c.bc >> 8) == 0 && (c.bc & 0xFF) == 2, "B is 0 and the callee ran twice more");
    /* over a plain instruction is a step */
    check(STEPPED(d, adamdebug_step_over(d)) && pc_of(d) == 0x8033, "step over INC A is one step");

    /* run to, out */
    check(STEPPED(d, adamdebug_run_to(d, 0x8050)) && pc_of(d) == 0x8050 &&
          reason_has(d, "run to"), "run to $8050");
    check(g_kind == ADAMDBG_STOP_RUNTO, "the callback says RUNTO");
    adamdebug_cpu_get(d, &c);
    check(STEPPED(d, adamdebug_step_out(d)) && pc_of(d) == 0x8030 &&
          reason_has(d, "step out"), "step out returns to the caller");
    adamdebug_cpu_get(d, &c2);
    check(c2.sp == c.sp + 2, "with the stack popped");
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x802B);

    /* scanline and frame */
    adamdebug_cpu_get(d, &c);
    check(STEPPED(d, adamdebug_scanline(d, 1)), "scanline 1");
    adamdebug_cpu_get(d, &c2);
    check(c2.line == (c.line + 1) % 262 && reason_has(d, "scanline"),
          "the beam moved exactly one line");
    adamdebug_cpu_get(d, &c);
    check(STEPPED(d, adamdebug_scanline(d, 5)), "scanline 5");
    adamdebug_cpu_get(d, &c2);
    check(c2.line == (c.line + 5) % 262, "five lines");
    adamdebug_cpu_get(d, &c);
    check(STEPPED(d, adamdebug_frame(d, 1)), "frame 1");
    adamdebug_cpu_get(d, &c2);
    check(STEPPED(d, adamdebug_scanline(d, 262)), "scanline 262");
    adamdebug_cpu_get(d, &c);
    check(c.frame == c2.frame + 1 && c.line == c2.line,
          "262 lines are exactly one frame, across the frame boundary");
    adamdebug_cpu_get(d, &c);
    check(STEPPED(d, adamdebug_frame(d, 1)), "frame 1 again");
    adamdebug_cpu_get(d, &c2);
    check(c2.frame == c.frame + 1 && c2.line == 0 && reason_has(d, "frame"),
          "one frame, at the frame boundary");
    check(STEPPED(d, adamdebug_frame(d, 3)), "frame 3");
    adamdebug_cpu_get(d, &c);
    check(c.frame == c2.frame + 3, "three more frames");

    /* disassembly: flags, labels, comments */
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x802B);
    adamdebug_set_label(d, 0x8050, "sub");
    adamdebug_set_label(d, 0x802B, "outer");
    n = adamdebug_disassemble(d, 0x802B, lines, 12, &pc_line);
    check(n == 12 && pc_line == 0 && lines[0].is_pc, "disassembly: the PC line is marked");
    check(strcmp(lines[0].disasm, "LD B,$03") == 0 && lines[0].length == 2 &&
          strcmp(lines[0].bytes, "06 03") == 0, "an instruction line");
    check(strcmp(lines[0].label, "outer") == 0, "its label");
    check(strcmp(lines[1].disasm, "CALL $8050") == 0 && strcmp(lines[1].comment, "sub") == 0 &&
          (lines[1].flags & ADAMDASM_CALL) && lines[1].target == 0x8050,
          "a CALL names its target");
    check(strstr(lines[2].disasm, "DJNZ") && (lines[2].flags & ADAMDASM_COND), "DJNZ");
    check(strstr(lines[5].disasm, "OUT") && strcmp(lines[5].comment, "PSG (SN76489)") == 0,
          "OUT ($FF) is the PSG");
    check(strstr(lines[6].disasm, "IN A") && strcmp(lines[6].comment, "controller 1") == 0,
          "IN A,($FC) is controller 1");
    check(!lines[4].has_breakpoint, "no breakpoint yet");
    check(adamdebug_row_address(d, 0x802B, 2) == 0x8030, "two rows down");
    check(adamdebug_row_address(d, 0x8030, -2) == 0x802B, "two rows up (best guess)");
    check(adamdebug_row_address(d, 0x802B, 0) == 0x802B, "zero rows");
    pc_line = 99;
    adamdebug_disassemble(d, 0x8050, lines, 2, &pc_line);
    check(pc_line == -1, "no PC line when the PC is not shown");

    /* execute breakpoints */
    check(adamdebug_breakpoint_toggle(d, 0x8033) == 1 && adamdebug_breakpoint_check(d, 0x8033),
          "a breakpoint on LD (HL),A");
    adamdebug_disassemble(d, 0x802B, lines, 12, NULL);
    check(lines[4].address == 0x8033 && lines[4].has_breakpoint && lines[4].breakpoint_enabled,
          "its line shows it");
    check(run_until_stop(d, 3000) && pc_of(d) == 0x8033, "it stops before the instruction");
    check(reason_has(d, "breakpoint: exec $8033") && g_kind == ADAMDBG_STOP_BREAKPOINT,
          "reason: breakpoint: exec $8033");
    adamdebug_read(d, 0x6000, b, 1);
    {
        uint8_t before = b[0];
        check(run_until_stop(d, 3000) && pc_of(d) == 0x8033,
              "resuming from a breakpoint runs on and hits it again");
        adamdebug_read(d, 0x6000, b, 1);
        check(b[0] == (uint8_t)(before + 1), "one loop later: the write really happened");
    }
    adamdebug_breakpoint_list(d, list, 8);
    check(list[0].type == ADAMDEBUG_BP_EXEC && list[0].hits >= 2 && list[0].enabled,
          "hit counts accumulate");
    id = list[0].id;
    adamdebug_breakpoint_enable(d, id, 0);
    adamdebug_disassemble(d, 0x802B, lines, 12, NULL);
    check(lines[4].has_breakpoint && !lines[4].breakpoint_enabled, "a disabled one: shown, not enabled");
    {
        unsigned g = adamdebug_generation(d);
        adamdebug_resume(d);
        sleep_ms(150);
        check(!adamdebug_is_stopped(d), "a disabled breakpoint never stops");
        adamdebug_stop(d);
        check(wait_stopped(d, 1, 3000) && adamdebug_generation(d) != g, "stop");
        check(reason_has(d, "stopped"), "reason: stopped");
    }
    adamdebug_breakpoint_remove(d, id);
    check(!adamdebug_breakpoint_check(d, 0x8033), "removed");

    /* conditions */
    check(adamdebug_breakpoint_add(d, ADAMDEBUG_BP_EXEC, 0x8033, 0x8033, "this bogus ((",
                                   err, sizeof err) == -1 && err[0], "an unparsable condition is refused, with a reason");
    check(adamdebug_breakpoint_add(d, ADAMDEBUG_BP_EXEC, 0x8033, 0x8033, "nosuchname == 1",
                                   err, sizeof err) == -1 && strstr(err, "nosuchname"),
          "an unknown name is refused");
    check(adamdebug_breakpoint_add(d, ADAMDEBUG_BP_IN, 0x100, 0x100, NULL, err, sizeof err) == -1 &&
          strstr(err, "255"), "a port above 255 is refused");
    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_EXEC, 0x8033, 0x8033, "a == 300", err, sizeof err);
    check(id > 0, "a condition that can never hold is accepted");
    {
        uint8_t m1, m2;
        adamdebug_read(d, 0x6000, &m1, 1);
        adamdebug_resume(d);
        sleep_ms(150);
        check(!adamdebug_is_stopped(d), "a false condition never stops");
        adamdebug_read(d, 0x6000, &m2, 1);
        check(m1 != m2, "and the machine keeps running through it");
        adamdebug_stop(d);
        wait_stopped(d, 1, 3000);
    }
    adamdebug_breakpoint_remove(d, id);
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x8024);
    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_EXEC, 0x8033, 0x8033, "a==3", err, sizeof err);
    check(id > 0 && run_until_stop(d, 3000) && pc_of(d) == 0x8033, "a true condition stops");
    adamdebug_cpu_get(d, &c);
    check((c.af >> 8) == 3, "on the pass where A is 3");
    adamdebug_breakpoint_list(d, list, 8);
    check(list[0].hits == 1 && strcmp(list[0].condition, "a==3") == 0,
          "the hit count counts only the true one");
    adamdebug_breakpoint_clear(d);
    check(adamdebug_breakpoint_list(d, list, 8) == 0, "cleared");

    /* a range of addresses */
    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_EXEC, 0x8038, 0x8039, NULL, err, sizeof err);
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x802B);
    check(run_until_stop(d, 3000) && pc_of(d) == 0x8038, "an exec range stops at its first address");
    check(run_until_stop(d, 3000) && pc_of(d) == 0x8039, "and again at the next one");
    adamdebug_breakpoint_clear(d);

    /* watchpoints: they stop AFTER the accessing instruction */
    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_WRITE, 0x6000, 0x6000, NULL, err, sizeof err);
    check(id > 0, "a write watchpoint");
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x802B);
    check(run_until_stop(d, 3000) && pc_of(d) == 0x8034, "it stops after LD (HL),A");
    adamdebug_read(d, 0x6000, b, 1);
    {
        char want[64], why[128];
        int addr;
        snprintf(want, sizeof want, "write $6000 = $%02X", b[0]);
        adamdebug_stop_reason(d, why, sizeof why, &addr);
        check(strcmp(why, want) == 0 && addr == 0x8034, "reason: write $6000 = $xx, at the next PC");
    }
    check(g_kind == ADAMDBG_STOP_WATCH, "the callback says WATCH");
    adamdebug_breakpoint_list(d, list, 8);
    check(list[0].hits == 1, "one hit");
    adamdebug_breakpoint_clear(d);

    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_WRITE, 0x6000, 0x6000, "value == 5", err, sizeof err);
    check(id > 0 && run_until_stop(d, 5000), "a write watchpoint with a condition on the value");
    adamdebug_read(d, 0x6000, b, 1);
    check(b[0] == 5 && reason_has(d, "write $6000 = $05"), "stopped on the write of 5");
    adamdebug_breakpoint_clear(d);

    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_READ, 0x6000, 0x6000, NULL, err, sizeof err);
    check(run_until_stop(d, 3000) && pc_of(d) == 0x8039 && reason_has(d, "read $6000 = $"),
          "a read watchpoint stops after LD A,(HL)");
    adamdebug_breakpoint_clear(d);
    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_READ, 0x802B, 0x8039, NULL, err, sizeof err);
    adamdebug_resume(d);
    sleep_ms(150);
    check(!adamdebug_is_stopped(d), "a read watchpoint over code ignores opcode fetches");
    adamdebug_stop(d);
    wait_stopped(d, 1, 3000);
    adamdebug_breakpoint_clear(d);

    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_OUT, 0xFF, 0xFF, NULL, err, sizeof err);
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x802B);
    check(run_until_stop(d, 3000) && pc_of(d) == 0x8036 && reason_has(d, "out $FF <- $"),
          "an OUT watchpoint stops after OUT ($FF),A");
    adamdebug_breakpoint_clear(d);
    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_IN, 0xF0, 0xFF, NULL, err, sizeof err);
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x802B);
    check(run_until_stop(d, 3000) && pc_of(d) == 0x8038 && reason_has(d, "in $FC -> $"),
          "an IN watchpoint over a port range stops after IN A,($FC)");
    adamdebug_breakpoint_clear(d);
    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_OUT, 0x00, 0xFE, NULL, err, sizeof err);
    adamdebug_resume(d);
    sleep_ms(150);
    check(!adamdebug_is_stopped(d), "an OUT watchpoint on other ports stays quiet");
    adamdebug_stop(d);
    wait_stopped(d, 1, 3000);
    adamdebug_breakpoint_clear(d);

    /* a watchpoint and an execute breakpoint together; stepping over a write */
    id = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_WRITE | ADAMDEBUG_BP_READ, 0x6000, 0x6000, NULL, err, sizeof err);
    id2 = adamdebug_breakpoint_add(d, ADAMDEBUG_BP_EXEC, 0x8036, 0x8036, NULL, err, sizeof err);
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x8033);
    check(STEPPED(d, adamdebug_step(d)) && pc_of(d) == 0x8034 && reason_has(d, "write $6000"),
          "stepping the write reports the watchpoint");
    check(run_until_stop(d, 3000) && pc_of(d) == 0x8036 && reason_has(d, "breakpoint: exec"),
          "then the execute breakpoint");
    adamdebug_breakpoint_remove(d, id);
    adamdebug_breakpoint_remove(d, id2);
    check(adamdebug_breakpoint_list(d, list, 8) == 0, "removed");

    /* labels and symbol files in every format */
    check(adamdebug_label_address(d, "sub") == 0x8050, "a label set by the user resolves");
    adamdebug_address_label(d, 0x8050, out, sizeof out);
    check(strcmp(out, "sub") == 0, "and the address finds it");
    adamdebug_set_label(d, 0x8050, "");
    check(adamdebug_label_address(d, "sub") == -1, "removed with an empty name");
    check(adamdebug_label_address(d, "EOS_HARD_INIT") == 0xFC5D, "the built-in EOS table");
    check(adamdebug_label_address(d, "RST0") == 0x0000, "the built-in OS7 table");
    snprintf(path, sizeof path, "%s/t.sym", dir);
    write_text(path, "# plain\n8050 plainsub ; comment\n8033 plainstore\n");
    check(adamdebug_load_symbols(d, path, msg, sizeof msg) == 2 &&
          adamdebug_label_address(d, "plainsub") == 0x8050, "a plain .sym loads");
    snprintf(path, sizeof path, "%s/t.wla", dir);
    write_text(path, "; WLA-DX\n[labels]\n00:8024 wlastart\n");
    check(adamdebug_load_symbols(d, path, msg, sizeof msg) == 1 &&
          adamdebug_label_address(d, "wlastart") == 0x8024, "a WLA-DX .sym loads");
    snprintf(path, sizeof path, "%s/t.map", dir);
    write_text(path, "_counter                        = $6000 ; addr, public, , main, , main.c:3\n");
    check(adamdebug_load_symbols(d, path, msg, sizeof msg) == 1 &&
          adamdebug_label_address(d, "_counter") == 0x6000, "a z88dk .map loads");
    snprintf(path, sizeof path, "%s/t.noi", dir);
    write_text(path, "DEF _pad 0x6001\n");
    check(adamdebug_load_symbols(d, path, msg, sizeof msg) == 1 &&
          adamdebug_label_address(d, "_pad") == 0x6001, "an SDCC .noi loads");
    check(adamdebug_label_address(d, "_counter") == -1, "a new file replaces the last");
    check(adamdebug_load_symbols(d, NULL, msg, sizeof msg) == -1, "no symbol file next to the cartridge yet");
    snprintf(path, sizeof path, "%s/loop.sym", dir);
    write_text(path, "8033 nextto\n");
    check(adamdebug_load_symbols(d, NULL, msg, sizeof msg) == 1 &&
          adamdebug_label_address(d, "nextto") == 0x8033, "<cartridge>.sym next to the image loads");
    check(adamdebug_load_symbols(d, "/nonexistent/x.sym", msg, sizeof msg) == -1, "a missing file is refused");

    /* the expression evaluator */
    adamdebug_cpu_set(d, ADAM_REG_HL, 0x6000);
    check(adamdebug_eval(d, "hl + 1", &v, err, sizeof err) == 0 && v == 0x6001, "eval: hl + 1");
    check(adamdebug_eval(d, "$6000", &v, err, sizeof err) == 0 && v == 0x6000 &&
          adamdebug_eval(d, "0x6000", &v, err, sizeof err) == 0 && v == 0x6000 &&
          adamdebug_eval(d, "6000h", &v, err, sizeof err) == 0 && v == 0x6000 &&
          adamdebug_eval(d, "24576", &v, err, sizeof err) == 0 && v == 0x6000,
          "eval: $6000 0x6000 6000h 24576");
    check(adamdebug_eval(d, "[$8024] + {$8024}", &v, err, sizeof err) == 0 && v == 0x31 + 0x0031,
          "eval: [byte] and {word}");
    check(adamdebug_eval(d, "nextto + 2", &v, err, sizeof err) == 0 && v == 0x8035, "eval: a label");
    check(adamdebug_eval(d, "AF' ", &v, err, sizeof err) == 0, "eval: primed registers, any case");
    check(adamdebug_eval(d, "nosuch", &v, err, sizeof err) == -1 && strstr(err, "nosuch"), "eval: an unknown name");

    /* the trace ring */
    {
        adamdebug_trace tr[32];
        adamdebug_cpu_set(d, ADAM_REG_PC, 0x8024);
        adamdebug_trace_enable(d, 1);
        check(adamdebug_trace_enabled(d), "trace on");
        for (i = 0; i < 10; i++)
            STEPPED(d, adamdebug_step(d));
        n = adamdebug_trace_read(d, tr, 32);
        check(n == 10, "the trace holds the ten stepped instructions");
        check(n == 10 && tr[9].pc == 0x8024 && tr[8].pc == 0x8027 && tr[0].cycles > tr[9].cycles,
              "newest first, from the first one");
        check(n == 10 && tr[9].len == 3 && tr[9].bytes[0] == 0x31, "with their bytes");
        adamdebug_command(d, "trace 3", out, sizeof out);
        check(strstr(out, "802") != NULL, "the console shows it");
        adamdebug_trace_clear(d);
        check(adamdebug_trace_read(d, tr, 32) == 0, "cleared");
        adamdebug_trace_enable(d, 0);
    }

    /* Save */
    {
        static const struct { const char *kind; long size; } kinds[] = {
            { "mem", 65536 }, { "vram", 16384 },
        };
        for (i = 0; i < 2; i++) {
            char what[96];
            snprintf(path, sizeof path, "%s/save.%s", dir, kinds[i].kind);
            snprintf(what, sizeof what, "save %s writes %ld bytes", kinds[i].kind, kinds[i].size);
            check(adamdebug_save(d, kinds[i].kind, path, msg, sizeof msg) == 0 &&
                  file_size(path) == kinds[i].size, what);
        }
        snprintf(path, sizeof path, "%s/save.regs", dir);
        check(adamdebug_save(d, "regs", path, msg, sizeof msg) == 0 && file_size(path) > 100, "save regs");
        snprintf(path, sizeof path, "%s/save.dis", dir);
        check(adamdebug_save(d, "dis", path, msg, sizeof msg) == 0 && file_size(path) > 10000, "save dis");
        check(adamdebug_save(d, "nonsense", path, msg, sizeof msg) == -1, "an unknown kind is refused");
    }

    /* the console */
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x8024);
    adamdebug_cpu_set(d, ADAM_REG_A, 0x66);
    adamdebug_command(d, "print a", out, sizeof out);
    check(strstr(out, "$0066") != NULL, "print evaluates an expression");
    adamdebug_command(d, "regs", out, sizeof out);
    check(strstr(out, "PC=8024") && strstr(out, "SP=") && strstr(out, "AF'="), "regs");
    adamdebug_command(d, "mem $8024 4", out, sizeof out);
    check(strstr(out, "31 00 64 21") != NULL, "mem dumps CPU memory");
    adamdebug_command(d, "poke $6020 $11 $22", out, sizeof out);
    adamdebug_read(d, 0x6020, b, 2);
    check(b[0] == 0x11 && b[1] == 0x22, "poke writes memory");
    adamdebug_command(d, "label $8033 store", out, sizeof out);
    check(adamdebug_label_address(d, "store") == 0x8033, "label sets one");
    adamdebug_command(d, "disasm $8024 3", out, sizeof out);
    check(strstr(out, "LD SP,$6400") != NULL && strstr(out, "LD HL,$6000") != NULL, "disasm");
    adamdebug_command(d, "bp $8033 a==1", out, sizeof out);
    check(strstr(out, "breakpoint #") != NULL, "bp with a condition");
    adamdebug_command(d, "bp store", out, sizeof out);
    check(adamdebug_breakpoint_check(d, 0x8033), "bp takes a label");
    adamdebug_command(d, "bpw $6000 value==2", out, sizeof out);
    adamdebug_command(d, "bpout $FF", out, sizeof out);
    adamdebug_command(d, "bpin $F0-$FF", out, sizeof out);
    adamdebug_command(d, "bpr $6001-$6002", out, sizeof out);
    adamdebug_command(d, "breaks", out, sizeof out);
    check(strstr(out, "if a==1") && strstr(out, "xw") == NULL && strstr(out, " w ") &&
          strstr(out, "$00F0-$00FF"), "breaks lists them");
    adamdebug_command(d, "bpout $200", out, sizeof out);
    check(strstr(out, "error") != NULL, "bpout refuses a port above 255");
    adamdebug_command(d, "bp $8033 nosuch==1", out, sizeof out);
    check(strstr(out, "error") && strstr(out, "nosuch"), "bp refuses an unknown name");
    n = adamdebug_breakpoint_list(d, list, 8);
    check(n == 6, "six breakpoints");
    adamdebug_breakpoint_list(d, list, 8);
    snprintf(path, sizeof path, "disable #%d", list[0].id);
    adamdebug_command(d, path, out, sizeof out);
    adamdebug_breakpoint_list(d, list, 8);
    check(!list[0].enabled, "disable by id");
    snprintf(path, sizeof path, "enable %d", list[0].id);
    adamdebug_command(d, path, out, sizeof out);
    adamdebug_breakpoint_list(d, list, 8);
    check(list[0].enabled, "enable by id");
    adamdebug_command(d, "delete $8033", out, sizeof out);
    check(!adamdebug_breakpoint_check(d, 0x8033) && adamdebug_breakpoint_list(d, list, 8) == 4,
          "delete by address takes the breakpoints there");
    adamdebug_command(d, "clear", out, sizeof out);
    check(adamdebug_breakpoint_list(d, list, 8) == 0, "clear removes them");
    adamdebug_command(d, "io", out, sizeof out);
    check(strstr(out, "$A0-$BF") && strstr(out, "VDP"), "io: the ports");
    adamdebug_command(d, "vdp", out, sizeof out);
    check(strstr(out, "R0") != NULL || strstr(out, "Name table") != NULL, "vdp");
    adamdebug_command(d, "vram 0 16", out, sizeof out);
    check(out[0] != '\0', "vram");
    adamdebug_command(d, "help", out, sizeof out);
    check(strstr(out, "runto") != NULL && strstr(out, "bpout") != NULL, "help lists the commands");
    adamdebug_command(d, "frobnicate", out, sizeof out);
    check(strstr(out, "unknown command") != NULL, "an unknown command says so");
    adamdebug_command(d, "step", out, sizeof out);
    check(strstr(out, "step") && strstr(out, "$8027") && pc_of(d) == 0x8027,
          "step says where it ended up");
    adamdebug_command(d, "over", out, sizeof out);
    check(pc_of(d) == 0x802A, "over");
    adamdebug_command(d, "runto $802D", out, sizeof out);
    check(pc_of(d) == 0x802D && strstr(out, "run to"), "runto");
    adamdebug_command(d, "frame", out, sizeof out);
    check(adamdebug_is_stopped(d) && strstr(out, "frame"), "frame");
    adamdebug_command(d, "set pc $8024", out, sizeof out);
    check(pc_of(d) == 0x8024, "set pc");
    adamdebug_command(d, "set zf 1", out, sizeof out);
    adamdebug_cpu_get(d, &c);
    check(c.zf == 1, "set a flag");
    adamdebug_command(d, "go", out, sizeof out);
    sleep_ms(100);
    check(!adamdebug_is_stopped(d), "go runs");
    adamdebug_command(d, "step", out, sizeof out);
    check(strstr(out, "running") != NULL, "step while running says to stop first");
    adamdebug_command(d, "stop", out, sizeof out);
    check(adamdebug_is_stopped(d) && strstr(out, "stopped") != NULL, "stop stops, and says where");
    adamdebug_completions(d, "ru", out, sizeof out);
    check(strstr(out, "run\n") && strstr(out, "runto") != NULL, "Tab completion of commands");
    adamdebug_completions(d, "print h", out, sizeof out);
    check(strstr(out, "hl\n") && strstr(out, "hl'") && strstr(out, "halted") != NULL, "of registers");
    adamdebug_completions(d, "print sto", out, sizeof out);
    check(strstr(out, "store") != NULL, "of labels");
    adamdebug_completions(d, "save v", out, sizeof out);
    check(strstr(out, "vram") != NULL, "and of save kinds");

    /* detach: nothing left in the core, the machine runs freely */
    adamdebug_breakpoint_add(d, ADAMDEBUG_BP_EXEC, 0x8033, 0x8033, NULL, err, sizeof err);
    adamdebug_breakpoint_add(d, ADAMDEBUG_BP_WRITE, 0x6000, 0x6000, NULL, err, sizeof err);
    adamdebug_trace_enable(d, 1);
    check(run_until_stop(d, 3000), "stopped on a breakpoint with trace on");
    adamdebug_cpu_get(d, &c);
    adamdebug_detach(d);
    check(!adamdebug_is_attached(d) && !adamdebug_is_stopped(d), "detached");
    check(s->core && adamcore_bp_count(s->core) == 0 && !s->dbg_engaged,
          "nothing is left in the core and the fast path is back");
    sleep_ms(200);
    adamdebug_cpu_get(d, &c2);
    check(c2.frame > c.frame + 3, "the machine runs freely (frames advance)");
    check(adamdebug_breakpoint_list(d, list, 8) == 2, "the breakpoints are kept");
    adamdebug_trace_enable(d, 0);

    /* attach again: stops; breakpoints survive a restart of the session */
    adamdebug_attach(d);
    check(wait_stopped(d, 1, 3000), "attach stops the running machine");
    adamdebug_breakpoint_clear(d);
    adamdebug_breakpoint_add(d, ADAMDEBUG_BP_EXEC, 0x8033, 0x8033, NULL, err, sizeof err);
    adamsession_stop(s);
    check(!adamdebug_is_stopped(d) && adamdebug_is_attached(d), "the session stops with the engine attached");
    start_cv(s, cart);
    check(wait_stopped(d, 1, 3000) && pc_of(d) == 0x0000, "the restarted machine stops at its first instruction");
    check(adamdebug_breakpoint_check(d, 0x8033), "the breakpoint survived");
    adamdebug_cpu_set(d, ADAM_REG_PC, 0x8024);
    check(run_until_stop(d, 3000) && pc_of(d) == 0x8033, "and fires on the new machine");
    adamdebug_breakpoint_clear(d);
    adamdebug_detach(d);
    sleep_ms(50);
    return 0;
}

static int phase_adam(adamsession *s, adamdebug *d)
{
    adamsession_start_opts o;
    adamdebug_cpu c, c2;
    adamdebug_memmap m;
    char out[4096];
    int i;

    memset(&o, 0, sizeof o);
    o.machine = 0;
    o.joystick_mode = 1;
    adamsession_stop(s);
    if (adamsession_start(s, &o) != 0) {
        fprintf(stderr, "debug_boot: ADAM start failed: %s\n", adamsession_last_error(s));
        failures++;
        return 1;
    }
    sleep_ms(500); /* let SmartWriter get going */

    adamdebug_attach(d);
    check(wait_stopped(d, 1, 3000), "ADAM: attach stops the booted machine");
    adamdebug_memmap_get(d, &m);
    check(!m.cv_console && !m.game_mode, "ADAM: not a ColecoVision, computer reset");
    adamdebug_io_state(d, out, sizeof out);
    check(strstr(out, "ADAM") != NULL, "ADAM: the memory map text");
    check(adamdebug_label_address(d, "EOS_FILL_VRAM") == 0xFD26, "ADAM: built-in EOS symbols");

    /* a real stop address is steppable and disassembles */
    {
        adamdebug_line lines[8];
        adamdebug_cpu_get(d, &c);
        check(adamdebug_disassemble(d, (uint16_t)c.pc, lines, 8, NULL) == 8 &&
              lines[0].disasm[0] && lines[0].length >= 1, "ADAM: disassembly at the stop PC");
        adamdebug_step(d);
        sleep_ms(30);
        check(adamdebug_is_stopped(d), "ADAM: a step stops again");
    }

    /* tracing records instructions while running */
    adamdebug_trace_enable(d, 1);
    adamdebug_trace_clear(d);
    adamdebug_resume(d);
    sleep_ms(300);
    adamdebug_stop(d);
    check(wait_stopped(d, 1, 3000), "ADAM: stop");
    {
        static adamdebug_trace tr[512];
        int n = adamdebug_trace_read(d, tr, 512), ordered = 1;
        check(n >= 100, "ADAM: the trace recorded the running machine");
        for (i = 1; i < n; i++)
            if (tr[i].cycles > tr[i - 1].cycles) ordered = 0;
        check(ordered, "ADAM: newest first");
    }
    adamdebug_trace_enable(d, 0);

    /* the VDP snapshot has the SmartWriter screen loaded */
    {
        static adamvdp_snapshot snap;
        static uint8_t rgba[256 * 192 * 4];
        static const uint8_t probe[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        uint8_t saved[4], back[4];
        char text[4096];
        int nonzero = 0;
        adamvdp_tables t;

        adamdebug_vdp_snapshot(d, &snap);
        adamvdp_render_nametable(&snap, rgba);
        for (i = 0; i < 256 * 192; i++)
            if (rgba[i * 4] || rgba[i * 4 + 1] || rgba[i * 4 + 2]) nonzero++;
        check(nonzero > 0, "ADAM: the name table renders");
        adamdebug_vdp_read(d, 0x3FFE, saved, 4);
        adamdebug_vdp_write(d, 0x3FFE, probe, 4);
        adamdebug_vdp_read(d, 0x3FFE, back, 4);
        adamdebug_vdp_snapshot(d, &snap);
        check(memcmp(back, probe, 4) == 0 && snap.vram[0x3FFE] == 0xDE &&
              snap.vram[0x0001] == 0xEF, "ADAM: a VRAM poke wraps at 16K and reaches the snapshot");
        adamdebug_vdp_write(d, 0x3FFE, saved, 4);
        adamvdp_get_tables(&snap, &t);
        adamvdp_format_state(&snap, text, (int)sizeof text);
        check(t.mode != ADAMVDP_MODE_INVALID && strstr(text, "Name table") && strstr(text, "R7 $"),
              "ADAM: the VDP state decodes");
    }

    adamdebug_detach(d);
    adamdebug_cpu_get(d, &c);
    sleep_ms(200);
    adamdebug_cpu_get(d, &c2);
    check(c2.frame > c.frame + 3 && !s->dbg_engaged, "ADAM: detach lets it run on the fast path");
    return 0;
}

int main(void)
{
    char tmpl[512];
    char *tmp = adam_make_tempdir(tmpl, sizeof(tmpl), "adamdbg-test-");
    adamsession_paths paths;
    adamsession *s;
    adamdebug *d;

    if (!tmp) return 1;
    memset(&paths, 0, sizeof(paths));
    paths.config_dir = tmp;
    paths.data_dir = tmp;
    paths.fujinet_lib = ""; /* disabled */

    s = adamsession_new(&paths);
    if (!s) {
        fprintf(stderr, "debug_boot: session creation failed\n");
        return 1;
    }
    d = adamsession_debugger(s);
    if (!d) {
        fprintf(stderr, "debug_boot: no debugger engine\n");
        return 1;
    }
    check(adamsession_debugger(s) == d, "the engine is created once");

    phase_cv(s, d, tmp);
    if (adam_roms_placeholder)
        fprintf(stderr, "debug_boot: built without real ROMs; skipping the ADAM boot phase\n");
    else
        phase_adam(s, d);

    adamsession_stop(s);
    adamsession_free(s);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
