/*
 * AdamDisplay: the emulator video widget for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "adamsession.h"

G_BEGIN_DECLS

#define ADAM_TYPE_DISPLAY (adam_display_get_type())
G_DECLARE_FINAL_TYPE(AdamDisplay, adam_display, ADAM, DISPLAY, GtkWidget)

typedef enum {
    ADAM_ASPECT_SQUARE_PIXELS = 0, /* 256:212, matches the Android app */
    ADAM_ASPECT_TV_4_3 = 1,
    ADAM_ASPECT_INTEGER = 2,
} AdamAspectMode;

GtkWidget *adam_display_new(adamsession *session);
void adam_display_set_aspect_mode(AdamDisplay *self, AdamAspectMode mode);
void adam_display_set_smooth(AdamDisplay *self, gboolean smooth);

G_END_DECLS
