/*
 * prompt_test -- the debugger's command line (core/debugger/prompt.c) on an
 * engine with no machine behind it: the commands that only touch the engine's
 * own state (expressions, breakpoints with conditions, the watch list,
 * symbols, help, completion) and what is refused. Anything that needs the
 * machine is engine_test's and debug_boot_test's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adamdebug.h"
#include "adammemtools.h"
#include "adamsession.h"
#include "test_tmp.h"

static int failures;
static adamdebug *g_dbg;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "prompt_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static const char *run(const char *line)
{
    static char out[8192];
    adamdebug_command(g_dbg, line, out, sizeof out);
    return out;
}

static int has(const char *line, const char *want)
{
    const char *got = run(line);
    if (!strstr(got, want)) {
        fprintf(stderr, "prompt_test: \"%s\" printed \"%s\", want \"%s\"\n", line, got, want);
        failures = 1;
        return 0;
    }
    return 1;
}

static char section_seen[32];

static const char *video_stub(const char *section)
{
    snprintf(section_seen, sizeof section_seen, "%s", section);
    return "Display 256x192\n";
}

int main(void)
{
    char tmpl[512];
    char *tmp = adam_make_tempdir(tmpl, sizeof(tmpl), "adam-prompt-");
    adamsession_paths paths;
    adamsession *session;
    char text[2048];
    adamdebug_breakpoint list[16];
    int count;

    if (!tmp) return 1;
    memset(&paths, 0, sizeof paths);
    paths.config_dir = tmp;
    paths.data_dir = tmp;
    paths.fujinet_lib = ""; /* disabled */
    session = adamsession_new(&paths);
    g_dbg = session ? adamsession_debugger(session) : NULL;
    if (!g_dbg) {
        fprintf(stderr, "prompt_test: no debugger\n");
        return 1;
    }

    /* print: every notation, with the binary form */
    has("print $6000 + 16", "$6010  24592  %0110000000010000");
    has("print 0x10 * 3 - 1", "$002F  47");
    has("print %1010 | 1", "$000B  11");
    has("p (1+2)*3", "$0009  9");
    has("print nosuchlabel_zz", "error: unknown name 'nosuchlabel_zz'");
    has("print 1 +", "error:");
    has("print 1/0", "division by zero");
    /* with no machine the registers read 0 and memory is open bus */
    has("print hl + 5", "$0005  5");
    has("print [$6000]", "$00FF  255");
    has("print ixh + iyl + vline", "$0000  0");

    /* labels, user and built in */
    has("label $4010 MYSPOT", "$4010 = MYSPOT");
    has("print MYSPOT + 1", "$4011");
    has("symbols MYSPOT", "$4010  MYSPOT");
    has("symbols zzzzqqqq", "no labels match");
    CHECK(adamdebug_label_address(g_dbg, "MYSPOT") == 0x4010, "label not stored");
    has("label $4010", "usage");
    has("symbols PUT_VRAM", "PUT_VRAM");
    has("symbols EOS_FILL_VRAM", "EOS_FILL_VRAM");

    /* breakpoints: every kind, ranges, conditions */
    has("break $4000", "breakpoint #1");
    has("bpw $6000-$63FF value != old", "breakpoint #2");
    has("bpr MYSPOT", "breakpoint #3");
    has("bpa $6100 value == 5 && addr == $6100", "breakpoint #4");
    has("bpin $FC", "breakpoint #5");
    has("bpout $A0-$A1 value >= $80", "breakpoint #6");
    has("break $4020 a == 1", "breakpoint #7");
    has("break $4030 a == 1 && hl > $6000", "breakpoint #8");
    count = adamdebug_breakpoint_list(g_dbg, list, 16);
    CHECK(count == 8, "%d breakpoints, want 8", count);
    CHECK(list[3].type == ADAMDEBUG_BP_ACCESS && list[3].start == 0x6100, "the access entry");
    CHECK(list[1].type == ADAMDEBUG_BP_WRITE && list[1].end == 0x63FF, "the write range");
    CHECK(!strcmp(list[1].condition, "value != old"), "the condition is kept as typed: \"%s\"", list[1].condition);
    has("breaks", "rw  $6100 if value == 5 && addr == $6100");
    has("break $4040 nosuch == 1", "error:");
    has("break $4040 {bad", "error:");
    has("break $4040 a ==", "error:");
    has("bpin $100", "error: ports are 0-255");
    has("bpa", "usage");
    CHECK(adamdebug_breakpoint_list(g_dbg, list, 16) == 8, "a refused breakpoint was added");

    has("disable 2", "");
    adamdebug_breakpoint_list(g_dbg, list, 16);
    CHECK(!list[1].enabled, "disable");
    has("enable #2", "");
    adamdebug_breakpoint_list(g_dbg, list, 16);
    CHECK(list[1].enabled, "enable");
    has("hits clear", "hit counts cleared");
    has("hits", "#1");
    has("delete $4000", "");
    has("delete 3", "");
    has("delete 99", "no such breakpoint");
    CHECK(adamdebug_breakpoint_list(g_dbg, list, 16) == 6, "delete left %d",
          adamdebug_breakpoint_list(g_dbg, list, 16));
    /* a bare hex address is hex unless it is a label */
    has("bpw 6000", "breakpoint");
    has("bpin fc", "breakpoint");
    has("break 4a3c", "breakpoint");
    count = adamdebug_breakpoint_list(g_dbg, list, 16);
    CHECK(list[count - 1].start == 0x4A3C && list[count - 2].start == 0xFC && list[count - 3].start == 0x6000,
          "bare hex addresses: $%04X $%04X $%04X", list[count - 3].start, list[count - 2].start,
          list[count - 1].start);
    has("label $5000 beef", "$5000 = beef");
    has("break beef", "breakpoint");
    count = adamdebug_breakpoint_list(g_dbg, list, 16);
    CHECK(list[count - 1].start == 0x5000, "a label wins over bare hex");
    has("clear", "");
    has("breaks", "no breakpoints");

    /* the older single-address API shares the table */
    CHECK(adamdebug_breakpoint_toggle(g_dbg, 0x1234) == 1 && adamdebug_breakpoint_check(g_dbg, 0x1234), "toggle on");
    has("breaks", "$1234");
    CHECK(adamdebug_breakpoint_toggle(g_dbg, 0x1234) == 0 && !adamdebug_breakpoint_check(g_dbg, 0x1234), "toggle off");

    /* a condition can be replaced, and refused when it is broken */
    {
        char err[96];
        int bp_id = adamdebug_breakpoint_add(g_dbg, ADAMDEBUG_BP_WRITE, 0x6000, 0x6000, "value == 1", err, sizeof err);
        CHECK(bp_id > 0, "add");
        CHECK(adamdebug_breakpoint_set_condition(g_dbg, bp_id, "value == 2 && old == 1", err, sizeof err), "set_condition: %s", err);
        adamdebug_breakpoint_list(g_dbg, list, 16);
        CHECK(!strcmp(list[0].condition, "value == 2 && old == 1"), "the new condition: \"%s\"", list[0].condition);
        CHECK(!adamdebug_breakpoint_set_condition(g_dbg, bp_id, "value ==", err, sizeof err) && err[0], "a broken condition is refused");
        CHECK(adamdebug_breakpoint_set_condition(g_dbg, bp_id, "", err, sizeof err), "an empty condition clears it");
        CHECK(!adamdebug_breakpoint_set_condition(g_dbg, 9999, "1", err, sizeof err), "no such entry");
        adamdebug_breakpoint_clear(g_dbg);
    }

    /* the watch list */
    adamwatch_clear();
    has("watch", "no watches");
    has("watch $6000", "$6000");
    has("watch MYSPOT", "MYSPOT");
    CHECK(adamwatch_count() == 2, "two watches expected");
    has("unwatch 0", "");
    CHECK(adamwatch_count() == 1 && !strcmp(adamwatch_text(0), "MYSPOT"), "unwatch 0 removed the wrong one");
    has("unwatch all", "");
    CHECK(adamwatch_count() == 0, "unwatch all");
    has("unwatch", "unwatch <n|all>");

    /* a machine-less engine says so rather than crashing */
    has("find 3E 00", "stop the machine first");
    has("find text COL", "stop the machine first");
    has("find zz", "find [hex]");
    has("set a 5", "the machine is running");
    has("set a", "usage");
    has("step", "the machine is running");
    has("over", "the machine is running");
    has("map", "not running");
    has("io", "not running");
    has("cart", "no machine");
    has("sound", "no machine");
    has("video", "no video state yet");
    has("regs", "PC=0000");
    has("mem", "usage");
    has("frobnicate", "unknown command");

    /* the video text hook: a stub answers the command */
    {
        adamdebug_set_video_text(video_stub);
        has("video decoded", "Display 256x192");
        CHECK(!strcmp(section_seen, "decoded"), "the section reached the hook");
        adamdebug_set_video_text(NULL);
    }

    /* symbols: the list, the filter, the generation, clear */
    {
        int picks[8], total, got;
        uint16_t addr = 0;
        char name[64];
        unsigned gen = adamdebug_symbols_generation(g_dbg);
        total = adamdebug_symbols_count(g_dbg);
        CHECK(total > 100, "the built-in tables are loaded (%d)", total);
        got = adamdebug_symbols_filter(g_dbg, "myspot", picks, 8);
        CHECK(got == 1 && adamdebug_symbols_get(g_dbg, picks[0], &addr, name, sizeof name) &&
              addr == 0x4010 && !strcmp(name, "MYSPOT"), "a user label is in the list and the filter is case-blind");
        CHECK(!adamdebug_symbols_get(g_dbg, total, &addr, name, sizeof name), "past the end");
        adamdebug_set_label(g_dbg, 0x4012, "OTHER");
        CHECK(adamdebug_symbols_generation(g_dbg) != gen, "labelling bumps the symbols generation");
        gen = adamdebug_symbols_generation(g_dbg);
        adamdebug_clear_symbols(g_dbg);
        CHECK(adamdebug_symbols_generation(g_dbg) != gen, "clearing bumps it too");
        CHECK(adamdebug_label_address(g_dbg, "MYSPOT") == -1 && adamdebug_label_address(g_dbg, "OTHER") == -1,
              "clear dropped the user labels");
        CHECK(adamdebug_label_address(g_dbg, "EOS_FILL_VRAM") == 0xFD26 && adamdebug_label_address(g_dbg, "RST0") == 0,
              "the built-in EOS and OS7 names stay");
        CHECK(adamdebug_symbols_count(g_dbg) > 100 && adamdebug_symbols_count(g_dbg) < total, "the count is back to the built-ins");
    }

    /* help and completion */
    has("help", "bpa <addr>");
    has("HELP", "bpa <addr>");
    has("?", "video [section]");
    CHECK(adamdebug_completions(g_dbg, "wat", text, sizeof text) >= 1 && strstr(text, "watch") != NULL, "completion of wat");
    CHECK(adamdebug_completions(g_dbg, "bp", text, sizeof text) == 6,
          "bp completes six commands (bp bpr bpw bpa bpin bpout), got %s", text);
    CHECK(adamdebug_completions(g_dbg, "print PUT_VR", text, sizeof text) >= 1 && strstr(text, "PUT_VRAM") != NULL,
          "label completion");
    CHECK(adamdebug_completions(g_dbg, "print h", text, sizeof text) >= 1 && strstr(text, "hl") != NULL,
          "register completion");
    CHECK(adamdebug_completions(g_dbg, "print ixh", text, sizeof text) >= 1 && strstr(text, "ixh") != NULL, "ixh completes");
    CHECK(adamdebug_completions(g_dbg, "save d", text, sizeof text) == 1 && strstr(text, "dis") != NULL, "save kinds");
    CHECK(adamdebug_completions(g_dbg, "vid", text, sizeof text) >= 1 && strstr(text, "video") != NULL, "video completes");
    CHECK(adamdebug_completions(g_dbg, "hit", text, sizeof text) >= 1 && strstr(text, "hits") != NULL, "hits completes");

    adamsession_free(session);
    if (!failures) printf("prompt_test: ok\n");
    return failures;
}
