/*
 * A fresh, private XDG tree for a test, so it never reads or writes the
 * developer's real settings, and the same test can run twice in parallel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAM_TEST_TMPDIR_H
#define ADAM_TEST_TMPDIR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define test_mkdir(path) _mkdir(path)
#define test_getpid() _getpid()
#define test_chdir(path) _chdir(path)
#define test_getcwd(buf, num) _getcwd(buf, (int)(num))
#else
#include <sys/stat.h>
#include <unistd.h>
#define test_mkdir(path) mkdir(path, 0755)
#define test_getpid() getpid()
#define test_chdir(path) chdir(path)
#define test_getcwd(buf, num) getcwd(buf, num)
#endif

static inline void test_tmpdir(char *dst, size_t dstsz, const char *tag)
{
    const char *base = getenv("TMPDIR");
#if defined(_WIN32)
    if (!base || !*base) base = getenv("TEMP");
#endif
    if (!base || !*base) base = "/tmp";
    snprintf(dst, dstsz, "%s/adam-%s-%ld-%ld", base, tag,
             (long)test_getpid(), (long)time(NULL));
    test_mkdir(dst);
}

#endif
