/*
 * The Controllers window: both ColecoVision hand controllers side by side.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAM_WIN_KEYPAD_WINDOW_H
#define ADAM_WIN_KEYPAD_WINDOW_H

#include <windows.h>

#include "adamsession.h"

/* Shows the singleton window (creating it, hidden until now, on the first
 * call), or hides it if it is showing. */
void adam_keypad_window_toggle(HWND parent, adamsession *session);

#endif
