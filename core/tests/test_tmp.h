/*
 * Portable temp-directory helper for the tests (mkdtemp is POSIX-only).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAM_TEST_TMP_H
#define ADAM_TEST_TMP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#endif

/* Creates a unique temp directory named "<prefix>XXXXXX" under the system
 * temp base, writing its path into out (size outsz). Returns out or NULL. */
static inline char *adam_make_tempdir(char *out, size_t outsz,
                                      const char *prefix)
{
#if defined(_WIN32)
    const char *base = getenv("TEMP");
    if (!base || !*base) base = getenv("TMP");
    if (!base || !*base) base = ".";
    snprintf(out, outsz, "%s\\%sXXXXXX", base, prefix);
    if (!_mktemp(out))
        return NULL;
    if (_mkdir(out) != 0)
        return NULL;
    return out;
#else
    const char *base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    snprintf(out, outsz, "%s/%sXXXXXX", base, prefix);
    return mkdtemp(out);
#endif
}

#endif /* ADAM_TEST_TMP_H */
