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

/* Stop and relaunch the session with the current persisted settings (used
 * after preferences / cartridge changes). */
void adam_window_restart_session(AdamWindow *self);
void adam_window_toast(AdamWindow *self, const char *message);

G_END_DECLS
