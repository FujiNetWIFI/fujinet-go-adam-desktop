/*
 * Debugger window for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "adamsession.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session. */
void adam_debugger_show(GtkWindow *parent, adamsession *session);

G_END_DECLS
