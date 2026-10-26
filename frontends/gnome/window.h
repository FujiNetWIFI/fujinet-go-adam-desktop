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

/* Display-wide CSS, installed once at startup before any window: toggled-on
 * push buttons (class "adam-toggle") and highlighted ones ("adam-accent")
 * in the accent colour with black text, plus "adam-accent-text" (bold accent
 * coloured text). */
void adam_css_install(void);

/* A system action (ADAM_SYSACT_*) from the keypad window, with the same
 * feedback as the Machine menu. */
void adam_window_run_sysaction(AdamWindow *self, int sysact);

/* The key path shared by the main and keypad windows. Step 1: Map-mode
 * capture, then the controller bindings (adamsession_input_key); TRUE = the
 * key is consumed. Step 3 (after a window's own shortcuts): type it on the
 * ADAM keyboard; TRUE = typed. */
gboolean adam_window_input_key(adamsession *session, guint keyval,
                               guint keycode, GdkModifierType state,
                               gboolean down);
gboolean adam_window_type_key(adamsession *session, guint keyval,
                              GdkModifierType state);

G_END_DECLS
