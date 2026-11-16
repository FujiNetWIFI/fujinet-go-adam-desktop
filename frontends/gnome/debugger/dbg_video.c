/*
 * The debugger's Video tab (GTK4/cairo).
 *
 * Everything that decides what to show lives in the core (adamvidtab.h): this file is
 * widgets; the old VDP tab's panes (name table, patterns, sprites, palette, VRAM hex) are its
 * Decoded view's sources and its VRAM section. The picture is drawn from the controller's cached buffer (0xFFRRGGBB, drawn as
 * CAIRO_FORMAT_RGB24 with a nearest filter), so a mouse move redraws only the marks. Only the
 * section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <string.h>

#include "adamvidcap.h"
#include "adamvidtab.h"

#define NSWATCH 16          /* the VDP's 16 colours */
#define VRAM_STEP 256       /* the VRAM section shows 16 rows of 16 bytes */
#define MAX_OVERLAYS 16

typedef struct {
    adamvid_tab *tab;
    adamdebug *dbg;
    GtkDropDown *bank;
    GtkEntry *vram_entry;
    GtkLabel *vram_note;
    GtkTextView *vram_text;
    char *vram_sig;
    GtkDropDown *view, *source, *zoom;
    GtkWidget *brk_top, *brk_pix;
    GtkCheckButton *ovl[MAX_OVERLAYS];
    int nov;
    GtkLabel *status;
    GtkDrawingArea *pic;
    GtkWidget *pic_scroll;
    int vp_w, vp_h;
    GtkNotebook *sections;
    GtkLabel *pixel_text, *pixel_note;
    GtkTextView *regs_text, *lines_text;
    char *regs_sig, *lines_sig;
    GtkDrawingArea *swatch_box[NSWATCH];
    GtkLabel *swatch_lbl[NSWATCH];
    GtkLabel *timing_text;
    GtkProgressBar *beam;
    gboolean updating, primed, was_mapped;
    guint tick_id;
    gboolean down;
} Vid;

static GtkWidget *mono_label(void)
{
    GtkWidget *lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
    gtk_widget_add_css_class(lbl, "monospace");
    return lbl;
}

static GtkWidget *padded_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    return box;
}

static void set_rgb(cairo_t *ctx, uint32_t rgb)
{
    cairo_set_source_rgb(ctx, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
}

/* ---- the picture ---------------------------------------------------------- */

static void draw_message(cairo_t *ctx, int width, int height, const char *msg)
{
    cairo_text_extents_t ext;
    cairo_set_source_rgb(ctx, 0.65, 0.65, 0.68);
    cairo_select_font_face(ctx, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(ctx, 14);
    cairo_text_extents(ctx, msg, &ext);
    cairo_move_to(ctx, (width - ext.width) / 2 - ext.x_bearing, (height - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(ctx, msg);
}

/* A pixel can be one device pixel wide: outline it from outside, dark under light. */
static void draw_mark(cairo_t *ctx, const int box[4], double red, double green, double blue)
{
    cairo_set_line_width(ctx, 3);
    cairo_set_source_rgba(ctx, 0, 0, 0, 0.85);
    cairo_rectangle(ctx, box[0] - 1.5, box[1] - 1.5, box[2] + 3, box[3] + 3);
    cairo_stroke(ctx);
    cairo_set_line_width(ctx, 1);
    cairo_set_source_rgb(ctx, red, green, blue);
    cairo_rectangle(ctx, box[0] - 1.5, box[1] - 1.5, box[2] + 3, box[3] + 3);
    cairo_stroke(ctx);
}

static void draw_pic(GtkDrawingArea *area, cairo_t *ctx, int width, int height, gpointer data)
{
    Vid *vid = data;
    int picw, pich, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *pixels;
    (void)area;
    cairo_set_source_rgb(ctx, 0.07, 0.07, 0.09);
    cairo_paint(ctx);
    pixels = adamvid_tab_picture(vid->tab, &picw, &pich);
    if (!pixels) {
        const char *why = adamvid_tab_why(vid->tab);
        draw_message(ctx, width, height, why && *why ? why : "Nothing to show");
        return;
    }
    {
        cairo_surface_t *str = cairo_image_surface_create_for_data((unsigned char *)pixels, CAIRO_FORMAT_RGB24,
                                                                 picw, pich, picw * 4);
        cairo_set_source_surface(ctx, str, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(ctx), CAIRO_FILTER_NEAREST);
        cairo_paint(ctx);
        cairo_surface_destroy(str);
    }
    marks = adamvid_tab_marks(vid->tab, hover, pin);
    if (marks & 1) draw_mark(ctx, hover, 1, 1, 1);
    if (marks & 2) draw_mark(ctx, pin, 1.0, 0.85, 0.1);
}

static void update_pixel_section(Vid *vid)
{
    const adamvid_pixel *pix = adamvid_tab_pixel(vid->tab);
    const gboolean can = adamvid_tab_can_break(vid->tab) != 0;
    gtk_widget_set_sensitive(vid->brk_top, can);
    gtk_widget_set_sensitive(vid->brk_pix, can);
    gtk_label_set_text(vid->pixel_text, pix ? pix->text
        : "Move over the picture to inspect a pixel.\nClick to pin it (click again to unpin);\n"
          "double-click to break on writes to it.");
    {   /* the swatch the inspected pixel uses is outlined */
        int index;
        for (index = 0; index < NSWATCH; index++)
            gtk_widget_queue_draw(GTK_WIDGET(vid->swatch_box[index]));
    }
}

static void do_break_pixel(Vid *vid)
{
    const adamvid_pixel *pix = adamvid_tab_pixel(vid->tab);
    int bpid = adamvid_tab_break_on_pixel(vid->tab);
    char buf[160];
    if (bpid >= 0 && pix && pix->nbreak > 0) {
        if (pix->break_kind[0] == ADAMVID_BK_VRAM)
            g_snprintf(buf, sizeof buf, "Break on OUT to the data port ($BE) when the VRAM address is $%04X-$%04X",
                       pix->break_lo[0], pix->break_hi[0]);
        else
            g_snprintf(buf, sizeof buf, "Break on writes to VDP register %u (control port $BF)", pix->break_lo[0]);
    } else {
        g_snprintf(buf, sizeof buf, "Nothing to break on (inspect a tile, a pattern or a sprite, not the border, a swatch or the palette)");
    }
    /* the message lives in the Pixel section: first line under the picture would take room from
     * Fit and change the zoom */
    gtk_label_set_text(vid->pixel_note, buf);
    gtk_notebook_set_current_page(vid->sections, ADAMVID_S_PIXEL);
}

static void on_motion(GtkEventControllerMotion *ctrl, double posx, double posy, gpointer data)
{
    Vid *vid = data;
    (void)ctrl;
    if (adamvid_tab_hover(vid->tab, (int)posx, (int)posy)) {
        gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
        update_pixel_section(vid);
    }
}

static void on_leave(GtkEventControllerMotion *ctrl, gpointer data)
{
    Vid *vid = data;
    (void)ctrl;
    if (adamvid_tab_hover(vid->tab, -1, -1)) {
        gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
        update_pixel_section(vid);
    }
}

static void on_click(GtkGestureClick *gest, int clicks, double posx, double posy, gpointer data)
{
    Vid *vid = data;
    (void)gest;
    if (clicks == 1) {
        adamvid_tab_pin(vid->tab, (int)posx, (int)posy);
    } else if (clicks == 2) {
        /* the first click of the pair toggled the pin; the second restores it */
        adamvid_tab_hover(vid->tab, (int)posx, (int)posy);
        adamvid_tab_pin(vid->tab, (int)posx, (int)posy);
        do_break_pixel(vid);
    }
    gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
    update_pixel_section(vid);
}

/* ---- text tables ------------------------------------------------------------- */

/* Replace first text view'str contents when they (or the highlighted rows) changed;
 * the scroll position stays put. lit[idx] != 0 highlights line idx. */
static void put_table(GtkTextView *view, char **sigp, GString *text, const GString *lit)
{
    GString *sig = g_string_new(NULL);
    g_string_append_len(sig, lit->str, (gssize)lit->len);
    g_string_append_c(sig, '|');
    g_string_append_len(sig, text->str, (gssize)text->len);
    if (!*sigp || strcmp(*sigp, sig->str)) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(view);
        GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
        double pos = adj ? gtk_adjustment_get_value(adj) : 0;
        char *clean = g_utf8_make_valid(text->str, -1);
        gsize idx;
        gtk_text_buffer_set_text(buf, clean, -1);
        g_free(clean);
        for (idx = 0; idx < lit->len; idx++) {
            if (lit->str[idx] == '1') {
                GtkTextIter first, last;
                gtk_text_buffer_get_iter_at_line(buf, &first, (int)idx);
                last = first;
                gtk_text_iter_forward_to_line_end(&last);
                gtk_text_buffer_apply_tag_by_name(buf, "changed", &first, &last);
            }
        }
        if (adj) gtk_adjustment_set_value(adj, pos);
        g_free(*sigp);
        *sigp = g_strdup(sig->str);
    }
    g_string_free(sig, TRUE);
}

static void fill_regs(Vid *vid)
{
    const adamvid_reg *rec;
    int num = adamvid_tab_regs(vid->tab, &rec), idx;
    GString *str = g_string_new(NULL), *lit = g_string_new(NULL);
    g_string_append(str, "Register     Value    Writes  Meaning\n");
    g_string_append_c(lit, '0');
    for (idx = 0; idx < num; idx++) {
        char writes[24];
        if (rec[idx].writes > 0) g_snprintf(writes, sizeof writes, "%dx@%d", rec[idx].writes, rec[idx].first_line);
        else g_strlcpy(writes, "-", sizeof writes);
        g_string_append_printf(str, "%-12s %-8s %-7s %s\n", rec[idx].name, rec[idx].value, writes, rec[idx].meaning);
        g_string_append_c(lit, (rec[idx].age < ADAMVID_REG_FADE || rec[idx].writes > 0) ? '1' : '0');
    }
    if (!num) g_string_append(str, "The machine is not running.\n");
    put_table(vid->regs_text, &vid->regs_sig, str, lit);
    g_string_free(str, TRUE);
    g_string_free(lit, TRUE);
}

static void fill_lines(Vid *vid)
{
    const adamvid_band *rec;
    int num = adamvid_tab_lines(vid->tab, &rec), idx;
    GString *str = g_string_new(NULL), *lit = g_string_new(NULL);
    g_string_append(str, "Bands of display lines drawn from the same registers\n");
    g_string_append_c(lit, '0');
    for (idx = 0; idx < num; idx++) {
        g_string_append_printf(str, "%s\n", rec[idx].text);
        g_string_append_c(lit, rec[idx].writes > 0 ? '1' : '0');
    }
    if (!num) g_string_append(str, "The machine is not running.\n");
    put_table(vid->lines_text, &vid->lines_sig, str, lit);
    g_string_free(str, TRUE);
    g_string_free(lit, TRUE);
}

/* ---- colours ------------------------------------------------------------------ */

static void draw_swatch(GtkDrawingArea *area, cairo_t *ctx, int width, int height, gpointer data)
{
    Vid *vid = data;
    const adamvid_swatch *swatch;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "idx"));
    (void)width; (void)height;
    if (adamvid_tab_swatches(vid->tab, &swatch) <= index) return;
    set_rgb(ctx, swatch[index].rgb);
    cairo_rectangle(ctx, 0, 0, 22, 22);
    cairo_fill(ctx);
    cairo_set_source_rgba(ctx, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(ctx, 1);
    cairo_rectangle(ctx, 0.5, 0.5, 21, 21);
    cairo_stroke(ctx);
    if (swatch[index].inspected) {      /* the inspected pixel uses this colour */
        cairo_set_source_rgb(ctx, 1, 0.85, 0.1);
        cairo_set_line_width(ctx, 2);
        cairo_rectangle(ctx, 1, 1, 20, 20);
        cairo_stroke(ctx);
    }
}

static void fill_colours(Vid *vid)
{
    const adamvid_swatch *swatch;
    int num = adamvid_tab_swatches(vid->tab, &swatch), index;
    for (index = 0; index < NSWATCH; index++) {
        char buf[160];
        if (index < num)
            g_snprintf(buf, sizeof buf, "%-14s #%06X %7ld px  %s", swatch[index].name,
                       swatch[index].rgb & 0xffffff, swatch[index].uses, swatch[index].role);
        else
            g_snprintf(buf, sizeof buf, "-");
        gtk_label_set_text(vid->swatch_lbl[index], buf);
        gtk_widget_queue_draw(GTK_WIDGET(vid->swatch_box[index]));
    }
}

static void fill_timing(Vid *vid)
{
    const char *const *lines;
    int num = adamvid_tab_timing(vid->tab, &lines), idx;
    GString *str = g_string_new(NULL);
    for (idx = 0; idx < num; idx++) {
        const char *tab = strchr(lines[idx], '\t');
        if (tab) g_string_append_printf(str, "%-14.*s %s\n", (int)(tab - lines[idx]), lines[idx], tab + 1);
        else g_string_append_printf(str, "%s\n", lines[idx]);
    }
    gtk_label_set_text(vid->timing_text, str->len ? str->str : "The machine is not running.");
    g_string_free(str, TRUE);
    gtk_progress_bar_set_fraction(vid->beam, adamvid_tab_beam_permille(vid->tab) / 1000.0);
}

/* The physical VRAM, 16 rows from the tab's base address. */
static void fill_vram(Vid *vid)
{
    char text[2400], note[96];
    GString *str, *lit = g_string_new("");
    adamvid_tab_text(vid->tab, ADAMVID_S_VRAM, text, (int)sizeof text);
    str = g_string_new(text);
    if (!str->len) g_string_append(str, "The machine is not running.\n");
    put_table(vid->vram_text, &vid->vram_sig, str, lit);
    g_string_free(str, TRUE);
    g_string_free(lit, TRUE);
    g_snprintf(note, sizeof note, "$%04X", (unsigned)adamvid_tab_vram_base(vid->tab));
    if (!gtk_label_get_text(vid->vram_note)[0] || gtk_label_get_text(vid->vram_note)[0] == '$')
        gtk_label_set_text(vid->vram_note, note);
}

/* ---- refresh ------------------------------------------------------------------- */

static void fill_section(Vid *vid, int sec)
{
    switch (sec) {
    case ADAMVID_S_REGISTERS: fill_regs(vid); break;
    case ADAMVID_S_COLOURS: fill_colours(vid); break;
    case ADAMVID_S_LINES: fill_lines(vid); break;
    case ADAMVID_S_TIMING: fill_timing(vid); break;
    case ADAMVID_S_VRAM: fill_vram(vid); break;
    default: update_pixel_section(vid); break;
    }
}

static void refresh_view(Vid *vid, unsigned what)
{
    if (what & (ADAMVID_TAB_PICTURE | ADAMVID_TAB_OFF)) {
        int picw = 0, pich = 0;
        if (adamvid_tab_picture(vid->tab, &picw, &pich)) {
            gtk_drawing_area_set_content_width(vid->pic, picw);
            gtk_drawing_area_set_content_height(vid->pic, pich);
        } else {
            gtk_drawing_area_set_content_width(vid->pic, vid->vp_w > 4 ? vid->vp_w : 480);
            gtk_drawing_area_set_content_height(vid->pic, vid->vp_h > 4 ? vid->vp_h : 300);
        }
        gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
    }
    if (what & ADAMVID_TAB_TABLES) {
        gtk_label_set_text(vid->status, adamvid_tab_status(vid->tab));
        fill_section(vid, gtk_notebook_get_current_page(vid->sections));
        if (gtk_notebook_get_current_page(vid->sections) != ADAMVID_S_PIXEL)
            update_pixel_section(vid);    /* the Break on writes buttons follow the pixel */
    }
}

static void sync_viewport(Vid *vid)
{
    int wide = gtk_widget_get_width(vid->pic_scroll), high = gtk_widget_get_height(vid->pic_scroll);
    if (wide > 4 && high > 4 && (wide != vid->vp_w || high != vid->vp_h)) {
        vid->vp_w = wide;
        vid->vp_h = high;
        adamvid_tab_set_viewport(vid->tab, wide - 2, high - 2);
    }
}

void dbg_video_refresh(GtkWidget *page, gboolean force)
{
    Vid *vid = g_object_get_data(G_OBJECT(page), "vid");
    gboolean mapped;
    unsigned what;
    if (!vid || vid->down) return;
    mapped = gtk_widget_get_mapped(page);
    if (mapped && !vid->was_mapped) force = TRUE;      /* just shown */
    vid->was_mapped = mapped;
    if (!vid->primed) force = TRUE;
    vid->primed = TRUE;
    sync_viewport(vid);
    what = adamvid_tab_refresh(vid->tab, force);
    if (force) what |= ADAMVID_TAB_PICTURE | ADAMVID_TAB_TABLES;
    refresh_view(vid, what);
}

/* The picture's room changed (window resized, split dragged): Fit follows it. */
static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    Vid *vid = data;
    int picw = gtk_widget_get_width(widget), pich = gtk_widget_get_height(widget);
    (void)clock;
    if (picw > 4 && pich > 4 && (picw != vid->vp_w || pich != vid->vp_h)) {
        sync_viewport(vid);
        if (vid->primed && adamvid_tab_zoom(vid->tab) == 0)
            refresh_view(vid, adamvid_tab_refresh(vid->tab, TRUE) | ADAMVID_TAB_PICTURE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- controls ------------------------------------------------------------------- */

static void refresh_all(Vid *vid)
{
    refresh_view(vid, adamvid_tab_refresh(vid->tab, TRUE) | ADAMVID_TAB_PICTURE | ADAMVID_TAB_TABLES);
}

static void on_view(GObject *obj, GParamSpec *spec, gpointer data)
{
    Vid *vid = data;
    (void)obj; (void)spec;
    if (vid->updating) return;
    adamvid_tab_set_view(vid->tab, (int)gtk_drop_down_get_selected(vid->view));
    refresh_all(vid);
}

static void on_source(GObject *obj, GParamSpec *spec, gpointer data)
{
    Vid *vid = data;
    (void)obj; (void)spec;
    if (vid->updating) return;
    adamvid_tab_set_source(vid->tab, (int)gtk_drop_down_get_selected(vid->source));
    refresh_all(vid);
}

static void on_zoom(GObject *obj, GParamSpec *spec, gpointer data)
{
    Vid *vid = data;
    int num;
    const int *zoomval = adamvid_zooms(&num);
    guint idx = gtk_drop_down_get_selected(vid->zoom);
    (void)obj; (void)spec;
    if (vid->updating || (int)idx >= num) return;
    sync_viewport(vid);
    adamvid_tab_set_zoom(vid->tab, zoomval[idx]);
    refresh_all(vid);
}

static void on_bank(GObject *obj, GParamSpec *spec, gpointer data)
{
    Vid *vid = data;
    (void)obj; (void)spec;
    if (vid->updating) return;
    adamvid_tab_set_bank(vid->tab, (int)gtk_drop_down_get_selected(vid->bank));
    refresh_all(vid);
}

/* One entry does both jobs: an address alone scrolls the dump there, an
 * address followed by hex bytes writes them first ("1800: 41 42 43"). */
static void on_vram_entry(GtkEntry *entry, gpointer data)
{
    Vid *vid = data;
    uint8_t bytes[64];
    uint16_t addr = 0;
    char msg[200], err[160] = "";
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    int count = adamvdp_parse_poke(text, &addr, bytes, (int)sizeof bytes);
    if (count < 0) {
        gtk_label_set_text(vid->vram_note, "expected: addr [bytes...], e.g. 1800: 41 42");
        return;
    }
    /* the controller writes the bytes (the address counter and read-ahead are
     * not touched) and moves the dump to the address */
    count = adamvid_tab_poke(vid->tab, text, err, (int)sizeof err);
    if (count < 0) {
        gtk_label_set_text(vid->vram_note, err[0] ? err : "The bytes could not be written");
        return;
    }
    if (count > 0) {
        g_snprintf(msg, sizeof msg, "wrote %d byte%s at $%04X", count, count == 1 ? "" : "s", addr);
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
    } else {
        g_snprintf(msg, sizeof msg, "$%04X", addr);
    }
    gtk_label_set_text(vid->vram_note, msg);
    refresh_all(vid);
}

static void on_vram_page(GtkButton *btn, gpointer data)
{
    Vid *vid = data;
    int delta = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "delta"));
    /* addresses wrap at 16K, as the VDP's do */
    const uint32_t next = (uint32_t)((gint64)adamvid_tab_vram_base(vid->tab) + (gint64)delta * VRAM_STEP) & 0x3FFFu;
    adamvid_tab_set_vram_base(vid->tab, next);
    gtk_label_set_text(vid->vram_note, "");
    refresh_all(vid);
}

static void on_overlay(GtkCheckButton *btn, gpointer data)
{
    Vid *vid = data;
    unsigned mask = 0;
    int num, idx;
    const adamvid_overlay_info *info = adamvid_overlays(&num);
    (void)btn;
    if (vid->updating) return;
    for (idx = 0; idx < num && idx < vid->nov; idx++)
        if (gtk_check_button_get_active(vid->ovl[idx])) mask |= info[idx].bit;
    adamvid_tab_set_overlays(vid->tab, mask);
    refresh_all(vid);
}

static void on_unpin(GtkButton *btn, gpointer data)
{
    Vid *vid = data;
    (void)btn;
    adamvid_tab_unpin(vid->tab);
    gtk_widget_queue_draw(GTK_WIDGET(vid->pic));
    update_pixel_section(vid);
}

static void on_break(GtkButton *btn, gpointer data)
{
    (void)btn;
    do_break_pixel(data);
}

static void on_section(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer data)
{
    Vid *vid = data;
    (void)page;
    /* the sections' notebook re-selects pages as it is taken apart with its window: an unmapped
     * page has nothing to fill (it fills when shown) */
    if (vid->down || !gtk_widget_get_mapped(GTK_WIDGET(notebook))) return;
    fill_section(vid, (int)page_num);
}

/* Taking the page apart re-selects the sections' pages and resets the
 * choosers: the window calls this first, while every widget is still alive,
 * so nothing calls back into the page from then on. */
void dbg_video_shutdown(GtkWidget *page)
{
    Vid *vid = g_object_get_data(G_OBJECT(page), "vid");
    int idx;
    if (!vid || vid->down) return;
    vid->down = TRUE;
    vid->updating = TRUE;
    if (vid->tick_id) gtk_widget_remove_tick_callback(vid->pic_scroll, vid->tick_id);
    vid->tick_id = 0;
    g_signal_handlers_disconnect_by_data(vid->sections, vid);
    g_signal_handlers_disconnect_by_data(vid->view, vid);
    g_signal_handlers_disconnect_by_data(vid->source, vid);
    g_signal_handlers_disconnect_by_data(vid->zoom, vid);
    g_signal_handlers_disconnect_by_data(vid->bank, vid);
    for (idx = 0; idx < vid->nov; idx++)
        g_signal_handlers_disconnect_by_data(vid->ovl[idx], vid);
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    Vid *vid = data;
    (void)widget;
    adamvid_tab_free(vid->tab);
    g_free(vid->vram_sig);
    g_free(vid->regs_sig);
    g_free(vid->lines_sig);
    g_free(vid);
}

GtkWidget *dbg_video_new(adamdebug *dbg)
{
    Vid *vid = g_new0(Vid, 1);
    GtkWidget *root, *left, *bar, *right, *box, *btns, *brkbtn, *unpbtn, *scroll, *ovbtn, *pop, *ovbox, *grid, *brk;
    GtkEventController *motion;
    GtkGesture *click;
    GtkTextBuffer *buf;
    const adamvid_overlay_info *ovinfo;
    const int *zoomv;
    const char *views[ADAMVID_V_COUNT + 1], *zlabels[24];
    char zbuf[24][8];
    int num, nzoom, idx;

    vid->dbg = dbg;
    vid->tab = adamvid_tab_new(dbg);

    root = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set_data(G_OBJECT(root), "vid", vid);
    g_signal_connect(root, "destroy", G_CALLBACK(on_destroy), vid);

    /* ---- left: toolbar, status, picture */
    left = padded_box();
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (idx = 0; idx < ADAMVID_V_COUNT; idx++) views[idx] = adamvid_view_name(idx);
    views[ADAMVID_V_COUNT] = NULL;
    vid->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    gtk_widget_set_tooltip_text(GTK_WIDGET(vid->view),
        "Display: the last frame the machine published, border included.\n"
        "Decoded: one source rebuilt from VRAM and the VDP registers (see Source).\n"
        "Beam: the display with the lines the beam has not reached dimmed (stopped).\n"
        "Values: each name table entry as a flat false colour of its value.");
    {
        const char *sources[ADAMVID_SRC_COUNT + 1];
        int sidx;
        for (sidx = 0; sidx < ADAMVID_SRC_COUNT; sidx++) sources[sidx] = adamvid_source_name(sidx);
        sources[ADAMVID_SRC_COUNT] = NULL;
        vid->source = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(sources));
        gtk_widget_set_tooltip_text(GTK_WIDGET(vid->source),
            "What the Decoded view redraws: the screen (the name table in its mode), "
            "the pattern sheet, the 32 sprites, or the 16 colours.");
    }
    zoomv = adamvid_zooms(&nzoom);
    if (nzoom > 23) nzoom = 23;
    for (idx = 0; idx < nzoom; idx++) {
        if (zoomv[idx] == 0) g_strlcpy(zbuf[idx], "Fit", sizeof zbuf[idx]);
        else g_snprintf(zbuf[idx], sizeof zbuf[idx], "%d×", zoomv[idx]);
        zlabels[idx] = zbuf[idx];
    }
    zlabels[nzoom] = NULL;
    vid->zoom = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(zlabels));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("View"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(vid->view));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Source"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(vid->source));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Zoom"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(vid->zoom));
    {
        static const char *const banks[] = { "Bank 0", "Bank 1", "Bank 2", NULL };
        vid->bank = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(banks));
        gtk_widget_set_tooltip_text(GTK_WIDGET(vid->bank),
            "Patterns in Graphics II come in three banks of 256 tiles");
        gtk_box_append(GTK_BOX(bar), GTK_WIDGET(vid->bank));
    }

    ovinfo = adamvid_overlays(&num);
    if (num > MAX_OVERLAYS) num = MAX_OVERLAYS;
    vid->nov = num;
    ovbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    for (idx = 0; idx < num; idx++) {
        vid->ovl[idx] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(ovinfo[idx].label));
        g_signal_connect(vid->ovl[idx], "toggled", G_CALLBACK(on_overlay), vid);
        gtk_box_append(GTK_BOX(ovbox), GTK_WIDGET(vid->ovl[idx]));
    }
    pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), ovbox);
    ovbtn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(ovbtn), "Overlays");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(ovbtn), pop);
    gtk_box_append(GTK_BOX(bar), ovbtn);

    brk = gtk_button_new_with_label("Break on writes");
    gtk_widget_set_tooltip_text(brk, "Break when the CPU writes what the inspected pixel is made from: "
                                     "its name table entry, pattern or colour byte or sprite byte "
                                     "(an OUT to the VDP data port at that VRAM address)");
    vid->brk_top = brk;
    g_signal_connect(brk, "clicked", G_CALLBACK(on_break), vid);
    gtk_box_append(GTK_BOX(bar), brk);
    gtk_box_append(GTK_BOX(left), bar);

    vid->status = GTK_LABEL(mono_label());
    gtk_label_set_selectable(vid->status, FALSE);
    gtk_label_set_ellipsize(vid->status, PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(vid->status));

    vid->pic = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_draw_func(vid->pic, draw_pic, vid, NULL);
    gtk_drawing_area_set_content_width(vid->pic, 480);
    gtk_drawing_area_set_content_height(vid->pic, 300);
    gtk_widget_set_halign(GTK_WIDGET(vid->pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(vid->pic), GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), vid);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), vid);
    gtk_widget_add_controller(GTK_WIDGET(vid->pic), motion);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), vid);
    gtk_widget_add_controller(GTK_WIDGET(vid->pic), GTK_EVENT_CONTROLLER(click));
    scroll = gtk_scrolled_window_new();
    vid->pic_scroll = scroll;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(vid->pic));
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    vid->tick_id = gtk_widget_add_tick_callback(scroll, on_tick, vid, NULL);
    gtk_box_append(GTK_BOX(left), scroll);


    /* ---- right: the six sections, in adamvid_section_name order */
    right = gtk_notebook_new();
    vid->sections = GTK_NOTEBOOK(right);
    gtk_widget_set_size_request(right, 480, -1);

    /* Pixel */
    box = padded_box();
    vid->pixel_text = GTK_LABEL(mono_label());
    gtk_label_set_wrap(vid->pixel_text, TRUE);
    gtk_label_set_wrap_mode(vid->pixel_text, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(vid->pixel_text));
    vid->pixel_note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(vid->pixel_note, 0);
    gtk_label_set_wrap(vid->pixel_note, TRUE);
    gtk_label_set_wrap_mode(vid->pixel_note, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(vid->pixel_note));
    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    brkbtn = gtk_button_new_with_label("Break on writes");
    vid->brk_pix = brkbtn;
    unpbtn = gtk_button_new_with_label("Unpin");
    g_signal_connect(brkbtn, "clicked", G_CALLBACK(on_break), vid);
    g_signal_connect(unpbtn, "clicked", G_CALLBACK(on_unpin), vid);
    gtk_box_append(GTK_BOX(btns), brkbtn);
    gtk_box_append(GTK_BOX(btns), unpbtn);
    gtk_box_append(GTK_BOX(box), btns);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(vid->sections, scroll, gtk_label_new(adamvid_section_name(ADAMVID_S_PIXEL)));

    /* Registers */
    vid->regs_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(vid->regs_text, FALSE);
    gtk_text_view_set_cursor_visible(vid->regs_text, FALSE);
    gtk_text_view_set_monospace(vid->regs_text, TRUE);
    gtk_text_view_set_wrap_mode(vid->regs_text, GTK_WRAP_NONE);   /* the columns stay lined up; it scrolls */
    buf = gtk_text_view_get_buffer(vid->regs_text);
    gtk_text_buffer_create_tag(buf, "changed", "background", "#C8B58C", "foreground", "#000000",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(vid->regs_text));
    gtk_notebook_append_page(vid->sections, scroll, gtk_label_new(adamvid_section_name(ADAMVID_S_REGISTERS)));

    /* Colours: the TMS9918A's 16 */
    box = padded_box();
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    for (idx = 0; idx < NSWATCH; idx++) {
        vid->swatch_box[idx] = GTK_DRAWING_AREA(gtk_drawing_area_new());
        gtk_drawing_area_set_content_width(vid->swatch_box[idx], 22);
        gtk_drawing_area_set_content_height(vid->swatch_box[idx], 22);
        g_object_set_data(G_OBJECT(vid->swatch_box[idx]), "idx", GINT_TO_POINTER(idx));
        gtk_drawing_area_set_draw_func(vid->swatch_box[idx], draw_swatch, vid, NULL);
        vid->swatch_lbl[idx] = GTK_LABEL(mono_label());
        gtk_label_set_selectable(vid->swatch_lbl[idx], FALSE);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(vid->swatch_box[idx]), 0, idx, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(vid->swatch_lbl[idx]), 1, idx, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(vid->sections, scroll, gtk_label_new(adamvid_section_name(ADAMVID_S_COLOURS)));

    /* Lines */
    vid->lines_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(vid->lines_text, FALSE);
    gtk_text_view_set_cursor_visible(vid->lines_text, FALSE);
    gtk_text_view_set_monospace(vid->lines_text, TRUE);
    buf = gtk_text_view_get_buffer(vid->lines_text);
    gtk_text_buffer_create_tag(buf, "changed", "background", "#C8B58C", "foreground", "#000000",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(vid->lines_text));
    gtk_notebook_append_page(vid->sections, scroll, gtk_label_new(adamvid_section_name(ADAMVID_S_LINES)));

    /* Timing */
    box = padded_box();
    vid->beam = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_text(vid->beam, "beam");
    gtk_progress_bar_set_show_text(vid->beam, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(vid->beam));
    vid->timing_text = GTK_LABEL(mono_label());
    gtk_label_set_selectable(vid->timing_text, FALSE);
    gtk_label_set_wrap(vid->timing_text, TRUE);
    gtk_label_set_wrap_mode(vid->timing_text, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(vid->timing_text));
    gtk_notebook_append_page(vid->sections, box, gtk_label_new(adamvid_section_name(ADAMVID_S_TIMING)));

    /* VRAM: the hex dump with the poke entry (a pin on a pixel moves it to the address it is made from) */
    box = padded_box();
    {
        GtkWidget *vbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *prev = gtk_button_new_with_label("\xe2\x97\x80");
        GtkWidget *next = gtk_button_new_with_label("\xe2\x96\xb6");
        vid->vram_entry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(vid->vram_entry, "addr [bytes...]  1800: 41 42");
        gtk_widget_set_tooltip_text(GTK_WIDGET(vid->vram_entry),
            "An address scrolls the dump there; an address and bytes writes them into VRAM first");
        gtk_widget_set_hexpand(GTK_WIDGET(vid->vram_entry), TRUE);
        g_signal_connect(vid->vram_entry, "activate", G_CALLBACK(on_vram_entry), vid);
        g_object_set_data(G_OBJECT(prev), "delta", GINT_TO_POINTER(-1));
        g_object_set_data(G_OBJECT(next), "delta", GINT_TO_POINTER(1));
        g_signal_connect(prev, "clicked", G_CALLBACK(on_vram_page), vid);
        g_signal_connect(next, "clicked", G_CALLBACK(on_vram_page), vid);
        gtk_box_append(GTK_BOX(vbar), GTK_WIDGET(vid->vram_entry));
        gtk_box_append(GTK_BOX(vbar), prev);
        gtk_box_append(GTK_BOX(vbar), next);
        gtk_box_append(GTK_BOX(box), vbar);
        vid->vram_note = GTK_LABEL(gtk_label_new(""));
        gtk_label_set_xalign(vid->vram_note, 0);
        gtk_label_set_wrap(vid->vram_note, TRUE);
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(vid->vram_note));
        vid->vram_text = GTK_TEXT_VIEW(gtk_text_view_new());
        gtk_text_view_set_editable(vid->vram_text, FALSE);
        gtk_text_view_set_cursor_visible(vid->vram_text, FALSE);
        gtk_text_view_set_monospace(vid->vram_text, TRUE);
        gtk_text_view_set_wrap_mode(vid->vram_text, GTK_WRAP_NONE);
        scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(vid->vram_text));
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_box_append(GTK_BOX(box), scroll);
    }
    gtk_notebook_append_page(vid->sections, box, gtk_label_new(adamvid_section_name(ADAMVID_S_VRAM)));

    gtk_paned_set_start_child(GTK_PANED(root), left);
    gtk_paned_set_end_child(GTK_PANED(root), right);
    gtk_paned_set_resize_start_child(GTK_PANED(root), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(root), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(root), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(root), FALSE);

    /* start from the controller's state (the ADAM_DEBUGGER_VIDEO_* variables) */
    vid->updating = TRUE;
    gtk_drop_down_set_selected(vid->view, (guint)adamvid_tab_view(vid->tab));
    gtk_drop_down_set_selected(vid->source, (guint)adamvid_tab_source(vid->tab));
    gtk_drop_down_set_selected(vid->bank, (guint)adamvid_tab_bank(vid->tab));
    for (idx = 0; idx < nzoom; idx++)
        if (zoomv[idx] == adamvid_tab_zoom(vid->tab)) gtk_drop_down_set_selected(vid->zoom, (guint)idx);
    for (idx = 0; idx < vid->nov; idx++)
        gtk_check_button_set_active(vid->ovl[idx], (adamvid_tab_overlays(vid->tab) & ovinfo[idx].bit) != 0);
    gtk_notebook_set_current_page(vid->sections, adamvid_tab_env_section(vid->tab));
    vid->updating = FALSE;
    update_pixel_section(vid);
    g_signal_connect(vid->view, "notify::selected", G_CALLBACK(on_view), vid);
    g_signal_connect(vid->source, "notify::selected", G_CALLBACK(on_source), vid);
    g_signal_connect(vid->bank, "notify::selected", G_CALLBACK(on_bank), vid);
    g_signal_connect(vid->zoom, "notify::selected", G_CALLBACK(on_zoom), vid);
    g_signal_connect(vid->sections, "switch-page", G_CALLBACK(on_section), vid);
    return root;
}
