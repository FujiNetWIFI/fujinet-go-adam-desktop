/*
 * The Win32 Controllers window (F9): both ADAM hand controllers drawn
 * as a player sees them -- stick, side buttons and keypad -- with the computer's
 * buttons, the gamepads and their assignments, Map mode and the ADAM keyboard,
 * in a fixed-size floating tool window. Hidden at start.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <windows.h>

#include "adamsession.h"

/* Show / hide (F9). Created on first use, hidden not destroyed after. */
void adam_controllers_window_toggle(HWND parent, adamsession *session);
/* Show it (and raise it) if it is not showing. */
void adam_controllers_window_show(HWND parent, adamsession *session);
int  adam_controllers_window_visible(void);

/* The gamepad set, the bindings or the machine changed: refresh. */
void adam_controllers_window_refresh(void);

/* Map mode is armed: the next host key is a binding, not typing. The main
 * window gives its keys here first; 1 if consumed. */
int adam_controllers_map_key(UINT msg, WPARAM wparam, LPARAM lparam);
