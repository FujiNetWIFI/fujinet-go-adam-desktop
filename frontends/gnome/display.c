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

/* ---- frame buffer pool ----------------------------------------------------
 * The converted frame is written straight into a pooled buffer and the
 * texture borrows it: no allocation in steady state. The buffer comes back to
 * the pool when GSK drops the texture's bytes, possibly after the widget is
 * gone, so the pool is reference counted and each outstanding buffer holds a
 * reference. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle guint32 buffers, ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT each */
} FramePool;

typedef struct {
    FramePool *pool;
    guint32 *px;
} FrameLoan;

#define POOL_KEEP 4

static FramePool *pool_new(void)
{
    FramePool *p = g_new0(FramePool, 1);
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(g_free);
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

static guint32 *pool_take(FramePool *p)
{
    guint32 *px = NULL;

    g_mutex_lock(&p->lock);
    if (p->spare->len > 0)
        px = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
    g_mutex_unlock(&p->lock);
    return px ? px : g_malloc((gsize)ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT * sizeof(guint32));
}

static void pool_give(FramePool *p, guint32 *px)
{
    g_mutex_lock(&p->lock);
    if (p->spare->len < POOL_KEEP) {
        g_ptr_array_add(p->spare, px);
        px = NULL;
    }
    g_mutex_unlock(&p->lock);
    g_free(px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->px);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _AdamDisplay {
    GtkWidget parent_instance;

    adamsession *session;
    GdkTexture *texture;
    /* Heap-allocated: a GObject instance must stay under 64K. GTK has no
     * 16-bit 565 memory format, so frames are expanded to XRGB8888 into a
     * pooled buffer the texture borrows. */
    uint16_t *fb;
    FramePool *pool;
    uint64_t serial;
    guint tick_id;
    gulong suspended_id;   /* notify::suspended on the toplevel, while mapped */
    GtkWindow *toplevel;
    AdamAspectMode aspect_mode;
    gboolean smooth;
};

G_DEFINE_FINAL_TYPE(AdamDisplay, adam_display, GTK_TYPE_WIDGET)

/* RGB565 -> XRGB8888, the layout GDK_MEMORY_B8G8R8X8 wants on a little-endian
 * host (the X byte is ignored, so no alpha). The 5- and 6-bit fields are
 * expanded by replicating their high bits, not by shifting left. */
static inline guint32 rgb565_to_xrgb(uint16_t p)
{
    guint r = (p >> 11) & 0x1F;
    guint g = (p >> 5) & 0x3F;
    guint b = p & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

/* Convert the session's RGB565 frame into px (one pass) and wrap px in the
 * texture. The previous texture goes in as the update source so GSK can reuse
 * its GPU texture. Takes ownership of px. */
static void publish_texture(AdamDisplay *self, guint32 *px)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;
    int i;

    for (i = 0; i < ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT; i++)
        px[i] = rgb565_to_xrgb(self->fb[i]);

    g_atomic_ref_count_inc(&self->pool->refs);
    loan->pool = self->pool;
    loan->px = px;
    bytes = g_bytes_new_with_free_func(
        px, (gsize)ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT * sizeof(guint32),
        loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)ADAMSESSION_FB_WIDTH * 4);
    gdk_memory_texture_builder_set_width(b, ADAMSESSION_FB_WIDTH);
    gdk_memory_texture_builder_set_height(b, ADAMSESSION_FB_HEIGHT);
    gdk_memory_texture_builder_set_format(b, GDK_MEMORY_B8G8R8X8);
    if (self->texture) {
        cairo_rectangle_int_t all = { 0, 0, ADAMSESSION_FB_WIDTH,
                                      ADAMSESSION_FB_HEIGHT };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, self->texture);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(&self->texture);
    self->texture = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
}

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
        publish_texture(self, pool_take(self->pool));
        gtk_widget_queue_draw(widget);
    }
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. A hidden or
 * suspended window gets no frame callbacks from the compositor anyway, but a
 * minimised one on some stacks keeps ticking, and every tick is a wakeup.
 * The emulator does not care: with no ticks the vsync stream goes stale and
 * the session paces on the wall clock. The first tick after the window comes
 * back copies whatever frame is newest. */
static void update_tick(AdamDisplay *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), tick_cb,
                                                     NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec;
    update_tick(ADAM_DISPLAY(user_data));
}

static void adam_display_map(GtkWidget *widget)
{
    AdamDisplay *self = ADAM_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(adam_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void adam_display_unmap(GtkWidget *widget)
{
    AdamDisplay *self = ADAM_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(adam_display_parent_class)->unmap(widget);
    update_tick(self);
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
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }
    G_OBJECT_CLASS(adam_display_parent_class)->dispose(object);
}

static void adam_display_class_init(AdamDisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    widget_class->snapshot = adam_display_snapshot;
    widget_class->map = adam_display_map;
    widget_class->unmap = adam_display_unmap;
    object_class->dispose = adam_display_dispose;
}

static void adam_display_init(AdamDisplay *self)
{
    self->fb = g_malloc0((gsize)ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT *
                         sizeof(uint16_t));
    self->pool = pool_new();
    self->aspect_mode = ADAM_ASPECT_SQUARE_PIXELS;
    self->smooth = FALSE;
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
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
