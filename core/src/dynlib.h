/*
 * Cross-platform dynamic-library seam: dlopen/dlsym on POSIX,
 * LoadLibrary/GetProcAddress on Windows. Kept header-only so the one
 * consumer (fujinet_runtime.c) stays simple.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAMSESSION_DYNLIB_H
#define ADAMSESSION_DYNLIB_H

#include <stdint.h>
#include <stdio.h>

#if defined(_WIN32)

#include <windows.h>

typedef HMODULE adam_dynlib;

static inline adam_dynlib adam_dynlib_open(const char *path)
{
    return LoadLibraryA(path);
}

static inline void *adam_dynlib_sym(adam_dynlib h, const char *name)
{
    return (void *)(uintptr_t)GetProcAddress(h, name);
}

/* Formats the last load/sym error into buf; returns buf. */
static inline const char *adam_dynlib_error(char *buf, int buflen)
{
    DWORD e = GetLastError();
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                            FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL, e, 0, buf, (DWORD)buflen, NULL))
        snprintf(buf, (size_t)buflen, "error %lu", (unsigned long)e);
    return buf;
}

#else

#include <dlfcn.h>

typedef void *adam_dynlib;

static inline adam_dynlib adam_dynlib_open(const char *path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static inline void *adam_dynlib_sym(adam_dynlib h, const char *name)
{
    return dlsym(h, name);
}

static inline const char *adam_dynlib_error(char *buf, int buflen)
{
    const char *e = dlerror();
    snprintf(buf, (size_t)buflen, "%s", e ? e : "(unknown)");
    return buf;
}

#endif

#endif /* ADAMSESSION_DYNLIB_H */
