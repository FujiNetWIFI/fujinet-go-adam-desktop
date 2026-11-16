/*
 * memtools_test -- the watch list, range parsing, search patterns and
 * breakpoint descriptions (core/debugger/memtools.c) on an engine with no
 * machine behind it: nothing here reads memory, so it runs everywhere.
 * (A watch's live value is checked by engine_test, which has a machine.)
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

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "memtools_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static void check_range(adamdebug *dbg, const char *text, int want_ok, unsigned want_lo, unsigned want_hi)
{
    uint16_t low = 0, high = 0;
    const int parsed = adamsym_parse_range(dbg, text, &low, &high);
    CHECK(parsed == want_ok, "range \"%s\" parsed=%d, want %d", text, parsed, want_ok);
    if (parsed && want_ok)
        CHECK(low == want_lo && high == want_hi, "range \"%s\" is $%04X-$%04X, want $%04X-$%04X", text, low, high,
              want_lo, want_hi);
}

int main(void)
{
    char tmpl[512];
    char *tmp = adam_make_tempdir(tmpl, sizeof(tmpl), "adam-memtools-");
    adamsession_paths paths;
    adamsession *session;
    adamdebug *dbg;

    if (!tmp) return 1;
    memset(&paths, 0, sizeof paths);
    paths.config_dir = tmp;
    paths.data_dir = tmp;
    paths.fujinet_lib = ""; /* disabled */
    session = adamsession_new(&paths);
    dbg = session ? adamsession_debugger(session) : NULL;
    if (!dbg) {
        fprintf(stderr, "memtools_test: no debugger\n");
        return 1;
    }

    /* ranges: bare hex, $-hex, low-high, low..high, expressions and the built-in BIOS labels */
    check_range(dbg, "C000", 1, 0xC000, 0xC000);
    check_range(dbg, "$C000", 1, 0xC000, 0xC000);
    check_range(dbg, "$C000-$C00F", 1, 0xC000, 0xC00F);
    check_range(dbg, "C00F-C000", 1, 0xC000, 0xC00F);
    check_range(dbg, "4000..400F", 1, 0x4000, 0x400F);
    check_range(dbg, "$C000+$10", 1, 0xC010, 0xC010);
    check_range(dbg, "  F3B0  ", 1, 0xF3B0, 0xF3B0);
    check_range(dbg, "", 0, 0, 0);
    check_range(dbg, "nosuchlabel_zz", 0, 0, 0);
    check_range(dbg, "$C000..", 0, 0, 0);
    {
        uint16_t low = 0, high = 0;
        int found = 0, idx;
        int picks[8];
        const int count = adamdebug_symbols_filter(dbg, "PUT_VRAM", picks, 8);
        CHECK(count > 0, "the built-in BIOS table has no names with PUT_VRAM in them");
        for (idx = 0; idx < count; idx++) {
            uint16_t addr;
            char name[64];
            if (adamdebug_symbols_get(dbg, picks[idx], &addr, name, sizeof name) && addr < 0x4000) {
                char expr[80];
                snprintf(expr, sizeof expr, "%s+1", name);
                CHECK(adamsym_parse_range(dbg, name, &low, &high) && low == addr, "label %s did not resolve", name);
                CHECK(adamsym_parse_range(dbg, expr, &low, &high) && low == addr + 1, "label+1 did not resolve for %s", name);
                found = 1;
                break;
            }
        }
        CHECK(found, "no BIOS label to try");
    }

    /* watches */
    {
        char row[200];
        adamwatch_clear();
        CHECK(adamwatch_count() == 0, "clear left watches");
        CHECK(adamwatch_add("   ") == -1, "a blank watch was added");
        CHECK(adamwatch_add("C000  ") == 0 && adamwatch_add("$FC9E-$FC9F") == 1, "add failed");
        CHECK(!strcmp(adamwatch_text(0), "C000") && !strcmp(adamwatch_text(1), "$FC9E-$FC9F"), "text kept as typed (trimmed)");
        CHECK(adamwatch_add("nosuchlabel_zz") == 2, "an unresolved watch is kept");
        adamwatch_format(dbg, 2, row, sizeof row);
        CHECK(strstr(row, "?") != NULL, "an unresolved watch should show ?: %s", row);
        adamwatch_format(dbg, 0, row, sizeof row);
        CHECK(strstr(row, "$C000") != NULL && strstr(row, "  -") != NULL, "running watch row: %s", row);
        {
            uint16_t low = 0, high = 0;
            CHECK(adamwatch_range(dbg, 1, &low, &high) && low == 0xFC9E && high == 0xFC9F, "watch range");
            CHECK(!adamwatch_range(dbg, 2, &low, &high), "an unresolved watch has no range");
        }
        {
            const unsigned gen = adamwatch_generation();
            adamwatch_remove(1);
            CHECK(adamwatch_count() == 2 && adamwatch_generation() != gen, "remove");
            CHECK(!strcmp(adamwatch_text(1), "nosuchlabel_zz"), "remove closed the gap");
            adamwatch_remove(9);
            CHECK(adamwatch_count() == 2, "remove out of range changed the list");
        }
        {
            int idx;
            for (idx = 0; idx < ADAMWATCH_MAX + 4; idx++) adamwatch_add("C000");
            CHECK(adamwatch_count() == ADAMWATCH_MAX, "the list is not capped at %d (%d)", ADAMWATCH_MAX, adamwatch_count());
        }
        adamwatch_clear();
        CHECK(adamwatch_count() == 0 && adamwatch_text(0) == NULL, "clear");
    }

    /* search patterns */
    {
        uint8_t pat[ADAMSEARCH_MAX], mask = 0;
        int count = 0, flags = -1;
        CHECK(adamsearch_parse(ADAMSEARCH_HEX, "$C3 0x00,40 f", pat, &count, &mask, &flags) && count == 4 &&
              pat[0] == 0xC3 && pat[1] == 0x00 && pat[2] == 0x40 && pat[3] == 0x0F && mask == 0xFF && flags == 0,
              "hex pattern: %d bytes", count);
        CHECK(adamsearch_parse(ADAMSEARCH_TEXT, "COL", pat, &count, &mask, &flags) && count == 3 &&
              !memcmp(pat, "COL", 3) && (flags & ADAMDEBUG_SEARCH_NOCASE), "text pattern");
        CHECK(!adamsearch_parse(ADAMSEARCH_HEX, "zz", pat, &count, &mask, &flags), "non-hex accepted");
        CHECK(!adamsearch_parse(ADAMSEARCH_HEX, "  ", pat, &count, &mask, &flags), "empty hex accepted");
        CHECK(!adamsearch_parse(ADAMSEARCH_TEXT, "", pat, &count, &mask, &flags), "empty text accepted");
    }

    /* breakpoint descriptions */
    {
        adamdebug_breakpoint bpt;
        char text[200];
        memset(&bpt, 0, sizeof bpt);
        bpt.type = ADAMDEBUG_BP_WRITE;
        bpt.start = 0xC010;
        bpt.end = 0xC010;
        snprintf(bpt.condition, sizeof bpt.condition, "value != old");
        adambp_describe(dbg, &bpt, text, sizeof text);
        CHECK(!strcmp(text, "Write  $C010  if value != old"), "describe: \"%s\"", text);
        bpt.type = ADAMDEBUG_BP_ACCESS;
        bpt.end = 0xC01F;
        bpt.condition[0] = '\0';
        adambp_describe(dbg, &bpt, text, sizeof text);
        CHECK(!strcmp(text, "Access  $C010-$C01F"), "describe access: \"%s\"", text);
        bpt.type = ADAMDEBUG_BP_OUT;
        bpt.start = 0x98;
        bpt.end = 0x99;
        adambp_describe(dbg, &bpt, text, sizeof text);
        CHECK(!strcmp(text, "Out  $98-$99"), "describe out: \"%s\"", text);
        bpt.type = ADAMDEBUG_BP_EXEC;
        bpt.start = bpt.end = 0x0000;
        adambp_describe(dbg, &bpt, text, sizeof text);
        CHECK(strncmp(text, "Exec  $0000", 11) == 0, "describe exec: \"%s\"", text);
    }

    /* region names */
    {
        const adamdebug_region map[] = {
            { 0x0000, 0x1FFF, 1, 0, "OS7 ROM" },
            { 0x6000, 0x7FFE, 1, 1, "RAM (24K)" },
            { 0x7FFF, 0x7FFF, 1, 1, "last byte" },
        };
        char text[64];
        CHECK(adammem_region_name(map, 3, 0x1000, text, sizeof text) > 0 && !strcmp(text, "OS7 ROM"), "region of $1000");
        CHECK(adammem_region_name(map, 3, 0x7FFF, text, sizeof text) > 0 && strstr(text, "last"), "region of $7FFF");
        CHECK(adammem_region_name(map, 2, 0x7FFF, text, sizeof text) == 0 && !text[0], "a gap has no name");
    }

    adamsession_free(session);
    if (!failures) printf("memtools_test: ok\n");
    return failures;
}
