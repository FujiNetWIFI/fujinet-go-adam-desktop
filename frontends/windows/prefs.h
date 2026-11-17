/*
 * The Input settings window -- see prefs.c.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAM_WIN_PREFS_H
#define ADAM_WIN_PREFS_H

#include "win_common.h"
#include "adamsession.h"

/* What the window asks of the main window. */
typedef struct {
    /* the window closed; dirty = a setting that needs a restart changed */
    void (*closed)(int dirty);
} adam_prefs_hooks;

/* Shows the window, or raises it when it is open. */
void adam_prefs_show(HWND parent, adamsession *session, const adam_prefs_hooks *hooks);
/* Closing without applying anything (quitting): the settings are saved. */
void adam_prefs_abandon(void);
/* The gamepad set changed. */
void adam_prefs_gamepads_changed(void);
/* Tab and the arrow keys move between its controls; 1 if the message was used. */
int adam_prefs_pretranslate(MSG *msg);

#endif
