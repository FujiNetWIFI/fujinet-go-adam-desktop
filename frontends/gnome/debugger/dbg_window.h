/*
 * Debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "adamsession.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session;
 * opening attaches the engine, which stops the machine. */
void adam_debugger_show(GtkWindow *parent, adamsession *session);

/* Shows it if absent, closes it if showing (detaching, so the machine runs):
 * the main window's F12 and menu item. */
void adam_debugger_toggle(GtkWindow *parent, adamsession *session);

G_END_DECLS
