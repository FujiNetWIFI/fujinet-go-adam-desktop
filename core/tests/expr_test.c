/*
 * expr_test -- the debugger's expression grammar (core/debugger/expr.c) on a
 * made-up machine: numbers in every notation, the operators and their
 * precedence, byte and word reads, names, the errors. No machine, so it runs
 * everywhere.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "expr_test: " __VA_ARGS__); fputc('\n', stderr); failures = 1; } } while (0)

static uint8_t fake_byte(void *user, uint16_t addr)
{
    (void)user;
    return (uint8_t)(addr & 0xFF) ^ 0x5A;
}

static int fake_name(void *user, const char *name, long *out)
{
    (void)user;
    if (!strcmp(name, "hl")) { *out = 0x6000; return 1; }
    if (!strcmp(name, "a")) { *out = 5; return 1; }
    if (!strcmp(name, "af'")) { *out = 0x1234; return 1; }
    if (!strcmp(name, "label_one")) { *out = 0x4010; return 1; }
    return 0;
}

static long value_of(const char *text)
{
    expr_ctx ctx = { fake_name, fake_byte, NULL };
    char err[96];
    long val = -12345;
    if (expr_eval(text, &ctx, &val, err, sizeof err) != 0) {
        fprintf(stderr, "expr_test: \"%s\" failed: %s\n", text, err);
        failures = 1;
    }
    return val;
}

static void check_fails(const char *text)
{
    expr_ctx ctx = { fake_name, fake_byte, NULL };
    char err[96] = "";
    long val;
    CHECK(expr_eval(text, &ctx, &val, err, sizeof err) != 0 && err[0], "\"%s\" was accepted", text);
}

int main(void)
{
    /* numbers */
    CHECK(value_of("$C000") == 0xC000, "$C000");
    CHECK(value_of("0xC000") == 0xC000, "0xC000");
    CHECK(value_of("0C000h") == 0xC000, "0C000h");
    CHECK(value_of("49152") == 49152, "49152");
    CHECK(value_of("%1010") == 10, "%%1010");
    CHECK(value_of("  7 ") == 7, "spaces");

    /* operators and precedence */
    CHECK(value_of("1+2*3") == 7, "precedence of *");
    CHECK(value_of("(1+2)*3") == 9, "parentheses");
    CHECK(value_of("10-3-2") == 5, "left associativity");
    CHECK(value_of("1<<4|1") == 17, "<< binds tighter than |");
    CHECK(value_of("$ff & %1010 ^ 3") == 9, "& above ^");
    CHECK(value_of("5 == 5 && 2 < 3") == 1, "&& of comparisons");
    CHECK(value_of("0 || 0") == 0, "||");
    CHECK(value_of("!0 + ~0") == 0, "unary ! and ~");
    CHECK(value_of("-3 + 10 % 4") == -1, "unary minus, %%");
    CHECK(value_of("7 >= 7") == 1 && value_of("7 > 7") == 0 && value_of("6 <= 7") == 1 && value_of("7 != 7") == 0, "relations");

    /* names and memory */
    CHECK(value_of("hl + 1") == 0x6001, "a name");
    CHECK(value_of("a * 2") == 10, "a one-letter name");
    CHECK(value_of("af'") == 0x1234, "a primed name");
    CHECK(value_of("label_one+2") == 0x4012, "a label");
    CHECK(value_of("[$10]") == (0x10 ^ 0x5A), "a byte read");
    CHECK(value_of("{$10}") == ((0x10 ^ 0x5A) | ((0x11 ^ 0x5A) << 8)), "a word read");
    CHECK(value_of("[hl]") == (0x00 ^ 0x5A), "a read through a register");
    CHECK(value_of("[[$10] & 0]") == (0 ^ 0x5A), "nested reads");

    /* errors */
    check_fails("");
    check_fails("1+");
    check_fails("(1+2");
    check_fails("[1");
    check_fails("{1");
    check_fails("nosuchname");
    check_fails("1/0");
    check_fails("1 2");
    check_fails("$");
    check_fails("#");

    /* a syntax check needs no machine: names are 0 */
    {
        long val;
        char err[96];
        CHECK(expr_eval("anything + [3]", NULL, &val, err, sizeof err) == 0 && val == 0, "NULL context check");
        CHECK(expr_eval("anything +", NULL, &val, err, sizeof err) != 0, "NULL context still finds syntax errors");
    }

    if (!failures) printf("expr_test: ok\n");
    return failures;
}
