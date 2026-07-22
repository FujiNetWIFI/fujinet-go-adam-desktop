/*
 * Headless debugger-engine test over a live session: boots the ADAM (no
 * FujiNet/audio/gamepad), then exercises pause, single-step, breakpoint
 * re-hit, instruction tracing, symbol lookup, disassembly annotation, and
 * resume -- the same call sequence the native debugger UIs make.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "adamdebug.h"
#include "adamsession.h"
#include "roms_embedded.h"
#include "test_tmp.h"

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_stops;
static adamdebug_stop_reason g_last_reason;
static uint16_t g_last_pc;

static void on_stop(void *ud, adamdebug_stop_reason reason, uint16_t pc)
{
    (void)ud;
    pthread_mutex_lock(&g_mtx);
    g_stops++;
    g_last_reason = reason;
    g_last_pc = pc;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mtx);
}

static int wait_stop(int have, int timeout_ms)
{
    struct timespec ts;
    int ok = 1;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_mtx);
    while (g_stops <= have && ok)
        ok = pthread_cond_timedwait(&g_cv, &g_mtx, &ts) == 0;
    pthread_mutex_unlock(&g_mtx);
    return ok;
}

int main(void)
{
    char tmpl[512];
    char *tmp = adam_make_tempdir(tmpl, sizeof(tmpl), "adamdbg-test-");
    adamsession_paths paths;
    adamsession *s;
    adamdebug *d;
    adamsession_start_opts opts;
    uint16_t addr = 0;
    int have;

    if (!tmp) return 1;
    if (adam_roms_placeholder) {
        fprintf(stderr, "debug_boot: built without real ROMs; skipping\n");
        return 77;
    }
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
    adamdebug_set_stop_callback(d, on_stop, NULL);

    /* Built-in symbol tables. */
    if (!adamdebug_symbol_find(d, "EOS_HARD_INIT", &addr) || addr != 0xFC5D) {
        fprintf(stderr, "debug_boot: EOS_HARD_INIT lookup failed (%04X)\n",
                addr);
        return 1;
    }
    if (!adamdebug_symbol_find(d, "EOS_FILL_VRAM", &addr) || addr != 0xFD26) {
        fprintf(stderr, "debug_boot: EOS_FILL_VRAM lookup failed\n");
        return 1;
    }
    if (!adamdebug_symbol_find(d, "RST0", &addr) || addr != 0x0000) {
        fprintf(stderr, "debug_boot: RST0 lookup failed\n");
        return 1;
    }

    memset(&opts, 0, sizeof(opts));
    opts.machine = 0;
    opts.joystick_mode = 1;
    if (adamsession_start(s, &opts) != 0) {
        fprintf(stderr, "debug_boot: start failed: %s\n",
                adamsession_last_error(s));
        return 1;
    }

    usleep(400 * 1000); /* let SmartWriter get going */

    /* Pause halts within a frame. */
    have = g_stops;
    adamdebug_pause(d);
    if (!wait_stop(have, 2000) || g_last_reason != ADAMDBG_STOP_PAUSE) {
        fprintf(stderr, "debug_boot: pause did not stop (reason %d)\n",
                g_last_reason);
        return 1;
    }

    /* Disassembly at the stop PC is sane and steppable. */
    {
        adamdasm_line lines[8];
        adamcore_z80_regs r;
        int n, i;
        adamdebug_get_regs(d, &r);
        if (r.pc != g_last_pc) {
            fprintf(stderr, "debug_boot: callback pc %04X != regs %04X\n",
                    g_last_pc, r.pc);
            return 1;
        }
        n = adamdebug_disassemble(d, r.pc, 8, lines);
        if (n != 8) return 1;
        for (i = 0; i < n; i++)
            if (lines[i].len < 1 || lines[i].len > 4 || !lines[i].text[0]) {
                fprintf(stderr, "debug_boot: bad disasm line %d\n", i);
                return 1;
            }
    }

    /* Single-step fires a STEP stop. */
    have = g_stops;
    adamdebug_step_into(d);
    if (!wait_stop(have, 2000) || g_last_reason != ADAMDBG_STOP_STEP) {
        fprintf(stderr, "debug_boot: step_into did not stop\n");
        return 1;
    }

    /* A breakpoint at the paused PC re-hits after resume (the machine is
     * in its idle/main loop). */
    {
        adamcore_z80_regs r;
        adamdebug_get_regs(d, &r);
        adamdebug_bp_set(d, r.pc);
        if (!adamdebug_bp_is_set(d, r.pc)) return 1;
        have = g_stops;
        adamdebug_resume(d);
        if (!wait_stop(have, 5000) ||
            g_last_reason != ADAMDBG_STOP_BREAKPOINT || g_last_pc != r.pc) {
            fprintf(stderr,
                    "debug_boot: breakpoint at %04X not re-hit (reason %d "
                    "pc %04X)\n", r.pc, g_last_reason, g_last_pc);
            return 1;
        }
        adamdebug_bp_clear_all(d);
    }

    /* Tracing records instructions while running. */
    adamdebug_trace_enable(d, 1);
    have = g_stops;
    adamdebug_resume(d);
    usleep(300 * 1000);
    adamdebug_pause(d);
    if (!wait_stop(have, 2000)) return 1;
    {
        static adamtrace_entry entries[512];
        int n = adamdebug_trace_read(d, entries, 512), i;
        if (n < 100) {
            fprintf(stderr, "debug_boot: trace recorded only %d entries\n",
                    n);
            return 1;
        }
        for (i = 0; i < 10; i++)
            if (entries[i].len < 1 || entries[i].len > 4) {
                fprintf(stderr, "debug_boot: bad trace entry\n");
                return 1;
            }
        /* Newest-first: cycles must be non-increasing. */
        for (i = 1; i < n; i++)
            if (entries[i].cycles > entries[i - 1].cycles) {
                fprintf(stderr, "debug_boot: trace order broken\n");
                return 1;
            }
    }
    adamdebug_trace_enable(d, 0);

    /* VDP snapshot has the SmartWriter screen loaded. */
    {
        static adamvdp_snapshot snap;
        static uint8_t rgba[256 * 192 * 4];
        int i, nonzero = 0;
        adamdebug_vdp_snapshot(d, &snap);
        adamvdp_render_nametable(&snap, rgba);
        for (i = 0; i < 256 * 192; i++)
            if (rgba[i * 4] || rgba[i * 4 + 1] || rgba[i * 4 + 2])
                nonzero++;
        if (nonzero == 0) {
            fprintf(stderr, "debug_boot: nametable render is empty\n");
            return 1;
        }
    }

    adamdebug_resume(d);
    adamsession_stop(s);
    adamsession_free(s);
    printf("debug_boot: pause/step/breakpoint/trace/symbols/vdp ok\n");
    return 0;
}
