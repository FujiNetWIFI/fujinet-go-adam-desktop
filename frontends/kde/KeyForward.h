/*
 * Qt key events to the session's keysym space, and the one key path every
 * window that types on the ADAM shares (the display and the keypad window).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QKeyEvent>
#include <cstdint>

#include "adamsession.h"

/* The key's keysym, or 0 when it has none. The physical key through its
 * evdev code (so Shift cannot turn 8 into '*', and the numeric keypad and
 * the left/right modifiers stay distinct), else Qt's own key code. */
uint32_t adamKeysymFromQt(const QKeyEvent *event);

/* Press or release: the session's controller bindings first, then (press
 * only) the ADAM keyboard. True when the event was consumed. */
bool adamForwardKey(adamsession *session, QKeyEvent *event, bool down);
