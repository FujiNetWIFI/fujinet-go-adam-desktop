/*
 * AdamKeypadWindow -- the clickable controller panel: both hand controllers
 * side by side, the system resets and Map mode, so a mouse alone can drive
 * every button the machine has.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "adamsession.h"

/* Shows the singleton window (creating it on first use), or hides it if
 * already showing. `parent` is only used the first time. */
void adam_keypad_window_toggle(GtkWindow *parent, adamsession *session);
gboolean adam_keypad_window_is_visible(void);

/* Map mode, for the key path of every window: while armed this takes the
 * key (binds it once a target is picked, else swallows it) and returns TRUE.
 * keysym is the session keysym (see adam_window_input_key). */
gboolean adam_keypad_window_map_key(guint32 keysym);
