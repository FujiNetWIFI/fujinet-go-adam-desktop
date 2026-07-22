/*
 * AdamDisplay: paints the emulator's latest RGB565 frame with a
 * GdkMemoryTexture, letterboxed to the chosen aspect inside whatever size
 * the window manager gives us (tiling or floating). A frame-clock tick
 * callback doubles as the vsync source for the session's phase-lock:
 * GTK4's GPU renderer presents on the compositor's vsync, so pacing the
 * emulator on these ticks is pacing it on display vsync.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

struct _AdamDisplay {
    GtkWidget parent_instance;

    adamsession *session;
    GdkTexture *texture;
    /* Heap-allocated: a GObject instance must stay under 64K. GTK has no
     * 16-bit 565 memory format, so frames are expanded to RGB888. */
    uint16_t *fb;
    uint8_t *rgb;
    uint64_t serial;
    guint tick_id;
    AdamAspectMode aspect_mode;
    gboolean smooth;
};

G_DEFINE_FINAL_TYPE(AdamDisplay, adam_display, GTK_TYPE_WIDGET)

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    AdamDisplay *self = ADAM_DISPLAY(widget);
    (void)user_data;

    if (!self->session)
        return G_SOURCE_CONTINUE;

    /* Frame-clock time is CLOCK_MONOTONIC microseconds. */
    adamsession_notify_vsync(self->session,
                             gdk_frame_clock_get_frame_time(clock) * 1000);

    if (adamsession_copy_frame(self->session, self->fb, &self->serial)) {
        GBytes *bytes;
        int i;
        for (i = 0; i < ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT; i++) {
            uint16_t p = self->fb[i];
            uint8_t r = (uint8_t)((p >> 11) & 0x1F);
            uint8_t g = (uint8_t)((p >> 5) & 0x3F);
            uint8_t b = (uint8_t)(p & 0x1F);
            self->rgb[i * 3 + 0] = (uint8_t)(r << 3 | r >> 2);
            self->rgb[i * 3 + 1] = (uint8_t)(g << 2 | g >> 4);
            self->rgb[i * 3 + 2] = (uint8_t)(b << 3 | b >> 2);
        }
        bytes = g_bytes_new(self->rgb, (gsize)ADAMSESSION_FB_WIDTH *
                                           ADAMSESSION_FB_HEIGHT * 3);
        g_clear_object(&self->texture);
        self->texture = gdk_memory_texture_new(
            ADAMSESSION_FB_WIDTH, ADAMSESSION_FB_HEIGHT, GDK_MEMORY_R8G8B8,
            bytes, ADAMSESSION_FB_WIDTH * 3);
        g_bytes_unref(bytes);
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

static void adam_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    AdamDisplay *self = ADAM_DISPLAY(widget);
    const float w = (float)gtk_widget_get_width(widget);
    const float h = (float)gtk_widget_get_height(widget);
    float dw, dh, aspect;
    graphene_rect_t dest;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){0, 0, 0, 1},
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w < 1 || h < 1)
        return;

    aspect = self->aspect_mode == ADAM_ASPECT_TV_4_3
                 ? 4.0f / 3.0f
                 : (float)ADAMSESSION_FB_WIDTH / (float)ADAMSESSION_FB_HEIGHT;

    if (self->aspect_mode == ADAM_ASPECT_INTEGER) {
        int scale = (int)MIN(w / ADAMSESSION_FB_WIDTH,
                             h / ADAMSESSION_FB_HEIGHT);
        if (scale < 1) scale = 1;
        dw = (float)(scale * ADAMSESSION_FB_WIDTH);
        dh = (float)(scale * ADAMSESSION_FB_HEIGHT);
    } else if (w / h > aspect) {
        dh = h;
        dw = h * aspect;
    } else {
        dw = w;
        dh = w / aspect;
    }

    dest = GRAPHENE_RECT_INIT((w - dw) / 2.0f, (h - dh) / 2.0f, dw, dh);
    gtk_snapshot_append_scaled_texture(snapshot, self->texture,
                                       self->smooth ? GSK_SCALING_FILTER_LINEAR
                                                    : GSK_SCALING_FILTER_NEAREST,
                                       &dest);
}

static void adam_display_dispose(GObject *object)
{
    AdamDisplay *self = ADAM_DISPLAY(object);
    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_pointer(&self->fb, g_free);
    g_clear_pointer(&self->rgb, g_free);
    G_OBJECT_CLASS(adam_display_parent_class)->dispose(object);
}

static void adam_display_class_init(AdamDisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    widget_class->snapshot = adam_display_snapshot;
    object_class->dispose = adam_display_dispose;
}

static void adam_display_init(AdamDisplay *self)
{
    self->fb = g_malloc0((gsize)ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT *
                         sizeof(uint16_t));
    self->rgb =
        g_malloc0((gsize)ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT * 3);
    self->aspect_mode = ADAM_ASPECT_SQUARE_PIXELS;
    self->smooth = FALSE;
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    self->tick_id =
        gtk_widget_add_tick_callback(GTK_WIDGET(self), tick_cb, NULL, NULL);
}

GtkWidget *adam_display_new(adamsession *session)
{
    AdamDisplay *self = g_object_new(ADAM_TYPE_DISPLAY, NULL);
    self->session = session;
    return GTK_WIDGET(self);
}

void adam_display_set_aspect_mode(AdamDisplay *self, AdamAspectMode mode)
{
    self->aspect_mode = mode;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void adam_display_set_smooth(AdamDisplay *self, gboolean smooth)
{
    self->smooth = smooth;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
