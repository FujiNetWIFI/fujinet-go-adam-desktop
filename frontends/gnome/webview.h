/*
 * FujiNet configuration (embedded web UI) and console log windows.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "adamsession.h"

G_BEGIN_DECLS

/* Web UI window (WebKitGTK when built with it, otherwise opens the system
 * browser) and the FujiNet console log window. */
void adam_fujinet_config_show(GtkWindow *parent, adamsession *session);
void adam_fujinet_log_show(GtkWindow *parent, adamsession *session);

G_END_DECLS
