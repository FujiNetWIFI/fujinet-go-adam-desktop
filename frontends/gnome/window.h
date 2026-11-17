/*
 * AdamWindow: the main window of the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "adamsession.h"

G_BEGIN_DECLS

#define ADAM_TYPE_WINDOW (adam_window_get_type())
G_DECLARE_FINAL_TYPE(AdamWindow, adam_window, ADAM, WINDOW, AdwApplicationWindow)

GtkWidget *adam_window_new(AdwApplication *app, adamsession *session);

/* Themed icon name for this app: the desktop-entry id once installed, the
 * in-tree artwork when running uninstalled (defined in main.c). */
const char *adam_icon_name(void);

/* Stop and relaunch the session with the current persisted settings (used
 * after preferences / cartridge changes). */
void adam_window_restart_session(AdamWindow *self);
void adam_window_toast(AdamWindow *self, const char *message);

/* The Help context the main window shows now: "main", or "prefs/Machine"
 * while Preferences is open. */
const char *adam_window_help_context(AdamWindow *self);

/* Display-wide CSS, installed once at startup before any window: toggled-on
 * push buttons (class "adam-toggle") and highlighted ones ("adam-accent")
 * in the accent colour with black text, plus "adam-accent-text" (bold accent
 * coloured text). */
void adam_css_install(void);

/* A system action (ADAM_SYSACT_*) from a key, a pad button or the
 * Controllers window's strip, with the same feedback as the Machine menu.
 * UI thread. */
void adam_window_run_sysaction(AdamWindow *self, int sysact);

/* The one keyboard path: the main window's own key handler and the
 * Controllers window's both end here, so Map mode, the controller bindings,
 * F9 (Controllers), F10, F11, F12 and typing on the ADAM keyboard behave the
 * same whichever has the focus. `keycode` is the GDK hardware keycode.
 * Returns whether the key was consumed. */
gboolean adam_window_key(AdamWindow *self, guint keyval, guint keycode,
                         GdkModifierType state, gboolean down);

/* Lets go of every key the keyboard holds (focus loss). */
void adam_window_release_keys(AdamWindow *self);

G_END_DECLS
