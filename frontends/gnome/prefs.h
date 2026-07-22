/*
 * Preferences dialog for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "adamsession.h"

G_BEGIN_DECLS

typedef struct _AdamWindow AdamWindow;

/* Shows the preferences dialog. display_changed is invoked immediately when
 * a display-only option changes; machine options restart the session when
 * the dialog closes. */
void adam_prefs_show(AdamWindow *parent, adamsession *session,
                     void (*display_changed)(AdamWindow *parent));

G_END_DECLS
