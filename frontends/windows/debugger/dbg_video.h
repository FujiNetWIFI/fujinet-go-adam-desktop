/*
 * The debugger's Video tab (Win32): a thin view over core/include/adamvidtab.h. It is one
 * child window holding all its own controls, so the debugger shows, hides, places and pops it
 * out as a unit.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAM_WINDOWS_DBG_VIDEO_H
#define ADAM_WINDOWS_DBG_VIDEO_H

#include <windows.h>

#include "adamdebug.h"

/* The tab's page, a child of `parent`. ui and mono are the fonts it uses. */
HWND dbg_video_create(HWND parent, adamdebug *engine, HFONT ui_font, HFONT mono, int dpi);
/* Move and size the page (it lays out its own controls). */
void dbg_video_place(HWND page, int pos_x, int pos_y, int width, int height);
/* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
void dbg_video_refresh(HWND page, int force);
/* The debugger's fonts or DPI changed. */
void dbg_video_fonts(HWND page, HFONT ui_font, HFONT mono, int dpi);

#endif
