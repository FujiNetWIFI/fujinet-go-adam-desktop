/*
 * The Controllers window -- both ADAM hand controllers drawn as a player sees
 * them: the stick's disc ringed by eight direction wedges, a fire button down
 * either side and the twelve-key keypad on its overlay-card frame, and under
 * them, when asked for, the ADAM's own keyboard. Held controls light up in
 * the accent whoever holds them, the pointer works every control, and Map
 * mode rebinds any of them to a key or a gamepad button.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "adamsession.h"

/* Toggles visibility: shows the singleton window (creating it on first
 * call), or hides it if already showing. `parent` is the main window; it is
 * only used the first time, to set the transient-for relationship, and keys
 * typed in this window go through it. */
void adam_controllers_toggle(GtkWindow *parent, adamsession *session);
/* Shows it (or raises it). */
void adam_controllers_show(GtkWindow *parent, adamsession *session);
gboolean adam_controllers_visible(void);

/* Map mode's share of a key, for the main window's key path as well (Map can
 * be armed with either window focused): while Map is armed this takes the key
 * and returns TRUE. F9, F11, F12 and Ctrl chords are never Map's. */
gboolean adam_controllers_map_key(guint keyval, guint code, GdkModifierType mods,
                                  gboolean down);
