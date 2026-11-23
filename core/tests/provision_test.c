/*
 * provision_test -- paths_provision_fujinet() refreshes the user's data/
 * from the installed runtime on later runs (so a new CONFIG image reaches
 * existing users) and never touches their fnconfig.ini or SD/.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"
#include "test_tmpdir.h"

static int failures;

static void put(const char *dir, const char *name, const char *text)
{
    char p[ADAM_PATH_MAX];
    FILE *f;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    f = fopen(p, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static void expect(const char *dir, const char *name, const char *want)
{
    char p[ADAM_PATH_MAX], got[64] = "";
    FILE *f;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    f = fopen(p, "rb");
    if (f) { size_t n = fread(got, 1, sizeof(got) - 1, f); got[n] = 0; fclose(f); }
    if (strcmp(got, want)) {
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", name, got, want);
        failures++;
    }
}

int main(void)
{
    char tmp[ADAM_PATH_MAX], src[ADAM_PATH_MAX], data[ADAM_PATH_MAX];
    char sub[ADAM_PATH_MAX], lib[ADAM_PATH_MAX];
    adamsession *s = calloc(1, sizeof(*s));

    test_tmpdir(tmp, sizeof(tmp), "provision");
    snprintf(src, sizeof(src), "%s/src", tmp);
    snprintf(data, sizeof(data), "%s/user", tmp);
    mkdir_p(src);
    snprintf(sub, sizeof(sub), "%s/data", src);  mkdir_p(sub);
    snprintf(sub, sizeof(sub), "%s/SD", src);    mkdir_p(sub);
    put(src, "fnconfig.ini", "SRC");
    put(src, "data/autorun.ddp", "NEW");
    put(src, "data/fnconfig.ini", "SRCDATA");
    put(src, "SD/x", "SRCSD");
    snprintf(lib, sizeof(lib), "%s/libfujinet.so", tmp);
    put(tmp, "libfujinet.so", "");
    setenv("FUJINET_LIB", lib, 1);

    snprintf(s->data_dir, sizeof(s->data_dir), "%s", data);
    snprintf(s->fujinet_src, sizeof(s->fujinet_src), "%s", src);

    /* First run copies everything. */
    if (paths_provision_fujinet(s) != 0) { fprintf(stderr, "FAIL first run\n"); failures++; }
    expect(s->fujinet_root, "fnconfig.ini", "SRC");
    expect(s->fujinet_data, "autorun.ddp", "NEW");
    expect(s->fujinet_sd, "x", "SRCSD");

    /* The user changes things; the installed runtime gets a newer CONFIG. */
    put(s->fujinet_root, "fnconfig.ini", "USER");
    put(s->fujinet_data, "fnconfig.ini", "USERDATA");
    put(s->fujinet_sd, "x", "MINE");
    put(s->fujinet_data, "autorun.ddp", "OLD");

    if (paths_provision_fujinet(s) != 0) { fprintf(stderr, "FAIL second run\n"); failures++; }
    expect(s->fujinet_data, "autorun.ddp", "NEW");
    expect(s->fujinet_root, "fnconfig.ini", "USER");
    expect(s->fujinet_data, "fnconfig.ini", "USERDATA");
    expect(s->fujinet_sd, "x", "MINE");

    free(s);
    puts(failures ? "provision_test: FAILED" : "provision_test: ok");
    return failures ? 1 : 0;
}
