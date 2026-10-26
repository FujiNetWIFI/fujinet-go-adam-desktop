/*
 * The emulator picture -- see display.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAM_WIN_DISPLAY_H
#define ADAM_WIN_DISPLAY_H

#include <windows.h>

#include "adamsession.h"

/* Creates the picture as a child of parent (size it with MoveWindow) and
 * starts its render thread. One per process. */
HWND adam_display_create(HWND parent, adamsession *session);
/* Stops the render thread; call before the parent is destroyed. */
void adam_display_destroy(void);
/* aspect: 0 square pixels (256:212), 1 TV (4:3), 2 integer scale; smooth:
 * bilinear (else sharp pixels). Safe from any thread. */
void adam_display_set_options(int aspect, int smooth);
/* "Direct3D 11", "Direct3D 11 (WARP)" or "GDI": what is drawing. */
const char *adam_display_renderer(void);

#endif
