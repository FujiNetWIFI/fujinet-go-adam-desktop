/*
 * The Controllers window (F9): the two ADAM hand controllers drawn as
 * a player sees them -- stick, side fire buttons and 12-key keypad -- with the
 * gamepads and their assignments, and Map mode for rebinding any control.
 * Hidden at start; a fixed-size floating window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "adamsession.h"

/* Shows the window, or hides it when it is showing. */
void adamControllersToggle(QWidget *parent, adamsession *session);
/* Shows (and raises) the window; leaves it showing if it already is. */
void adamControllersShow(QWidget *parent, adamsession *session);
bool adamControllersVisible();
/* Preferences closed or the session restarted: redraw. */
void adamControllersRefresh();
