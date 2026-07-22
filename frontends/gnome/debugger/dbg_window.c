/*
 * Debugger window (GTK4/libadwaita): disassembly with breakpoint gutter,
 * editable registers, memory view, breakpoints, instruction trace, and the
 * VDP visualizers (nametable / pattern banks / sprites / palette), all over
 * the shared adamdebug engine. Stop events arrive on the emulator thread
 * and are marshaled here with g_idle_add.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "adamdebug.h"

#define DISASM_LINES 40
#define MEM_ROWS 16

typedef struct {
    GtkWindow *win;
    adamsession *session;
    adamdebug *dbg;

    GtkLabel *status;
    GtkButton *pause_btn;
    GtkTextView *disasm;
    uint16_t line_addr[DISASM_LINES];
    int line_count;
    uint16_t disasm_base;
    int follow_pc;

    GtkEntry *reg_entry[8]; /* AF BC DE HL IX IY SP PC */
    GtkLabel *flags_label;

    GtkEntry *mem_addr;
    GtkTextView *mem_view;
    uint16_t mem_base;

    GtkTextView *bp_view;
    GtkEntry *bp_entry;

    GtkTextView *trace_view;
    GtkSwitch *trace_switch;

    GtkPicture *pic_nt, *pic_pat, *pic_spr, *pic_pal;
    GtkDropDown *pat_bank;
    GtkTextView *sprite_info;

    guint tick_timer;
} DbgWin;

static const char *const reg_names[8] = {"AF", "BC", "DE", "HL",
                                         "IX", "IY", "SP", "PC"};

/* ---- helpers ------------------------------------------------------------- */

static void set_text(GtkTextView *view, const char *text)
{
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(view), text, -1);
}

static uint16_t reg_get(const adamcore_z80_regs *r, int i)
{
    switch (i) {
    case 0: return (uint16_t)((r->a << 8) | r->f);
    case 1: return (uint16_t)((r->b << 8) | r->c);
    case 2: return (uint16_t)((r->d << 8) | r->e);
    case 3: return (uint16_t)((r->h << 8) | r->l);
    case 4: return r->ix;
    case 5: return r->iy;
    case 6: return r->sp;
    default: return r->pc;
    }
}

static void reg_set(adamcore_z80_regs *r, int i, uint16_t v)
{
    switch (i) {
    case 0: r->a = (uint8_t)(v >> 8); r->f = (uint8_t)v; break;
    case 1: r->b = (uint8_t)(v >> 8); r->c = (uint8_t)v; break;
    case 2: r->d = (uint8_t)(v >> 8); r->e = (uint8_t)v; break;
    case 3: r->h = (uint8_t)(v >> 8); r->l = (uint8_t)v; break;
    case 4: r->ix = v; break;
    case 5: r->iy = v; break;
    case 6: r->sp = v; break;
    default: r->pc = v; break;
    }
}

/* Parses "FC5D", "$FC5D", "0xFC5D" or a symbol name. */
static int parse_addr(DbgWin *w, const char *text, uint16_t *out)
{
    const char *p = text;
    char *end;
    unsigned long v;
    while (*p == ' ') p++;
    if (*p == '$') p++;
    v = strtoul(p, &end, 16);
    if (end != p && *end == '\0' && v <= 0xFFFF) {
        *out = (uint16_t)v;
        return 1;
    }
    return adamdebug_symbol_find(w->dbg, text, out);
}

/* ---- refreshers ---------------------------------------------------------- */

static void refresh_disasm(DbgWin *w)
{
    adamdasm_line lines[DISASM_LINES];
    GString *str = g_string_new(NULL);
    adamcore_z80_regs r;
    int n, i;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(w->disasm);

    adamdebug_get_regs(w->dbg, &r);
    if (w->follow_pc)
        w->disasm_base = r.pc;

    n = adamdebug_disassemble(w->dbg, w->disasm_base, DISASM_LINES, lines);
    w->line_count = n;
    for (i = 0; i < n; i++) {
        char bytes[16] = "";
        int b;
        for (b = 0; b < lines[i].len; b++)
            g_snprintf(bytes + b * 3, 4, "%02X ", lines[i].bytes[b]);
        w->line_addr[i] = lines[i].addr;
        if (lines[i].symbol)
            g_string_append_printf(str, "%17s%s:\n", "", lines[i].symbol);
        g_string_append_printf(
            str, "%c%c%04X  %-12s %s\n",
            adamdebug_bp_is_set(w->dbg, lines[i].addr) ? '*' : ' ',
            lines[i].addr == r.pc ? '>' : ' ', lines[i].addr, bytes,
            lines[i].text);
    }
    gtk_text_buffer_set_text(buf, str->str, -1);
    g_string_free(str, TRUE);
}

static void refresh_regs(DbgWin *w)
{
    adamcore_z80_regs r;
    char buf[64];
    int i;
    adamdebug_get_regs(w->dbg, &r);
    for (i = 0; i < 8; i++) {
        g_snprintf(buf, sizeof(buf), "%04X", reg_get(&r, i));
        gtk_editable_set_text(GTK_EDITABLE(w->reg_entry[i]), buf);
    }
    g_snprintf(buf, sizeof(buf), "F: %c%c%c%c%c%c  IFF1:%d IM:%d  cyc:%llu",
               (r.f & 0x80) ? 'S' : '-', (r.f & 0x40) ? 'Z' : '-',
               (r.f & 0x10) ? 'H' : '-', (r.f & 0x04) ? 'P' : '-',
               (r.f & 0x02) ? 'N' : '-', (r.f & 0x01) ? 'C' : '-', r.iff1,
               r.im, (unsigned long long)r.cycles);
    gtk_label_set_text(w->flags_label, buf);
}

static void refresh_mem(DbgWin *w)
{
    uint8_t data[MEM_ROWS * 16];
    GString *str = g_string_new(NULL);
    int row, col;

    adamdebug_read_mem(w->dbg, w->mem_base, data, sizeof(data));
    for (row = 0; row < MEM_ROWS; row++) {
        g_string_append_printf(str, "%04X  ",
                               (uint16_t)(w->mem_base + row * 16));
        for (col = 0; col < 16; col++)
            g_string_append_printf(str, "%02X ", data[row * 16 + col]);
        g_string_append(str, " ");
        for (col = 0; col < 16; col++) {
            uint8_t c = data[row * 16 + col];
            g_string_append_c(str, (c >= 0x20 && c <= 0x7E) ? (char)c : '.');
        }
        g_string_append_c(str, '\n');
    }
    set_text(w->mem_view, str->str);
    g_string_free(str, TRUE);
}

static void refresh_bps(DbgWin *w)
{
    uint16_t bps[128];
    int n = adamdebug_bp_list(w->dbg, bps, 128), i;
    GString *str = g_string_new(n ? NULL : "(no breakpoints)\n");
    for (i = 0; i < n; i++) {
        uint16_t off = 0;
        const char *sym = adamdebug_symbol_at(w->dbg, bps[i], &off);
        if (sym && off == 0)
            g_string_append_printf(str, "%04X  %s\n", bps[i], sym);
        else if (sym)
            g_string_append_printf(str, "%04X  %s+%X\n", bps[i], sym, off);
        else
            g_string_append_printf(str, "%04X\n", bps[i]);
    }
    set_text(w->bp_view, str->str);
    g_string_free(str, TRUE);
}

static void refresh_trace(DbgWin *w)
{
    static adamtrace_entry entries[256];
    int n = adamdebug_trace_read(w->dbg, entries, 256), i;
    GString *str = g_string_new(
        n ? "  PC    AF   BC   DE   HL   SP\n" : "(trace empty; enable "
        "tracing and run)\n");
    for (i = 0; i < n; i++)
        g_string_append_printf(str, "%04X  %04X %04X %04X %04X %04X\n",
                               entries[i].pc, entries[i].af, entries[i].bc,
                               entries[i].de, entries[i].hl, entries[i].sp);
    set_text(w->trace_view, str->str);
    g_string_free(str, TRUE);
}

static GdkTexture *texture_from_rgba(const uint8_t *rgba, int tw, int th)
{
    GBytes *bytes = g_bytes_new(rgba, (gsize)tw * th * 4);
    GdkTexture *tex = gdk_memory_texture_new(
        tw, th, GDK_MEMORY_R8G8B8A8, bytes, (gsize)tw * 4);
    g_bytes_unref(bytes);
    return tex;
}

static void refresh_vdp(DbgWin *w)
{
    static adamvdp_snapshot snap;
    static uint8_t nt[256 * 192 * 4], pat[256 * 64 * 4], spr[128 * 64 * 4],
        pal[16 * 4];
    adamvdp_sprite info[32];
    GdkTexture *tex;
    GString *str;
    int i;

    adamdebug_vdp_snapshot(w->dbg, &snap);

    adamvdp_render_nametable(&snap, nt);
    tex = texture_from_rgba(nt, 256, 192);
    gtk_picture_set_paintable(w->pic_nt, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    adamvdp_render_patterns(&snap, (int)gtk_drop_down_get_selected(w->pat_bank),
                            pat);
    tex = texture_from_rgba(pat, 256, 64);
    gtk_picture_set_paintable(w->pic_pat, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    adamvdp_render_sprites(&snap, spr, info);
    tex = texture_from_rgba(spr, 128, 64);
    gtk_picture_set_paintable(w->pic_spr, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    adamvdp_render_palette(&snap, pal);
    tex = texture_from_rgba(pal, 16, 1);
    gtk_picture_set_paintable(w->pic_pal, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    str = g_string_new("##  Y    X  PAT CLR EC   R0-R7: ");
    for (i = 0; i < 8; i++)
        g_string_append_printf(str, "%02X ", snap.regs[i]);
    g_string_append_printf(str, " ST:%02X\n", snap.status);
    for (i = 0; i < 32; i++)
        g_string_append_printf(str, "%02d %3d  %3d  %02X  %2d  %d\n", i,
                               info[i].y, info[i].x, info[i].pattern,
                               info[i].color, info[i].early_clock);
    set_text(w->sprite_info, str->str);
    g_string_free(str, TRUE);
}

static void refresh_all(DbgWin *w)
{
    int paused = adamdebug_is_paused(w->dbg);
    refresh_disasm(w);
    refresh_regs(w);
    refresh_mem(w);
    refresh_bps(w);
    refresh_trace(w);
    refresh_vdp(w);
    gtk_button_set_label(w->pause_btn, paused ? "Continue (F5)"
                                              : "Pause (F5)");
}

/* ---- stop marshaling ------------------------------------------------------ */

typedef struct {
    DbgWin *w;
    adamdebug_stop_reason reason;
    uint16_t pc;
} StopEvent;

static gboolean stop_idle(gpointer user_data)
{
    StopEvent *ev = user_data;
    DbgWin *w = ev->w;
    static const char *const reason_names[] = {"paused", "breakpoint",
                                               "step", "run-to"};
    uint16_t off = 0;
    const char *sym = adamdebug_symbol_at(w->dbg, ev->pc, &off);
    char buf[128];

    if (sym)
        g_snprintf(buf, sizeof(buf), "Stopped (%s) at %04X  %s+%X",
                   reason_names[ev->reason], ev->pc, sym, off);
    else
        g_snprintf(buf, sizeof(buf), "Stopped (%s) at %04X",
                   reason_names[ev->reason], ev->pc);
    gtk_label_set_text(w->status, buf);
    w->follow_pc = 1;
    refresh_all(w);
    g_free(ev);
    return G_SOURCE_REMOVE;
}

static void on_stop(void *ud, adamdebug_stop_reason reason, uint16_t pc)
{
    StopEvent *ev = g_new0(StopEvent, 1);
    ev->w = ud;
    ev->reason = reason;
    ev->pc = pc;
    g_idle_add(stop_idle, ev);
}

/* ---- actions -------------------------------------------------------------- */

static void on_pause_continue(GtkButton *btn, gpointer ud)
{
    DbgWin *w = ud;
    (void)btn;
    if (adamdebug_is_paused(w->dbg)) {
        adamdebug_resume(w->dbg);
        gtk_label_set_text(w->status, "Running");
        refresh_all(w);
    } else {
        adamdebug_pause(w->dbg);
    }
}

static void on_step_into(GtkButton *b, gpointer ud)
{
    (void)b;
    adamdebug_step_into(((DbgWin *)ud)->dbg);
}

static void on_step_over(GtkButton *b, gpointer ud)
{
    (void)b;
    adamdebug_step_over(((DbgWin *)ud)->dbg);
}

static void on_step_out(GtkButton *b, gpointer ud)
{
    (void)b;
    adamdebug_step_out(((DbgWin *)ud)->dbg);
}

static void on_disasm_click(GtkGestureClick *gesture, int n_press, double x,
                            double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter iter;
    int bx, by, line = 0;
    char *text;
    GtkTextIter start, end;
    unsigned addr;
    (void)gesture;
    (void)n_press;

    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_TEXT,
                                          (int)x, (int)y, &bx, &by);
    if (!gtk_text_view_get_iter_at_location(w->disasm, &iter, bx, by))
        return;
    line = gtk_text_iter_get_line(&iter);
    /* Parse the address column from the clicked row (label rows lack one). */
    gtk_text_buffer_get_iter_at_line(gtk_text_view_get_buffer(w->disasm),
                                     &start, line);
    end = start;
    gtk_text_iter_forward_to_line_end(&end);
    text = gtk_text_iter_get_text(&start, &end);
    if (text && sscanf(text + 2, "%4X", &addr) == 1) {
        adamdebug_bp_toggle(w->dbg, (uint16_t)addr);
        refresh_disasm(w);
        refresh_bps(w);
    }
    g_free(text);
}

static void on_goto(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    uint16_t addr;
    if (parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &addr)) {
        w->follow_pc = 0;
        w->disasm_base = addr;
        refresh_disasm(w);
    }
}

static void on_mem_addr(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    uint16_t addr;
    if (parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &addr)) {
        w->mem_base = addr;
        refresh_mem(w);
    }
}

static void on_bp_add(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    uint16_t addr;
    if (parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &addr)) {
        adamdebug_bp_set(w->dbg, addr);
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        refresh_bps(w);
        refresh_disasm(w);
    }
}

static void on_bp_clear_all(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    adamdebug_bp_clear_all(w->dbg);
    refresh_bps(w);
    refresh_disasm(w);
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i;
    adamcore_z80_regs r;
    unsigned v;

    if (!adamdebug_is_paused(w->dbg))
        return;
    adamdebug_get_regs(w->dbg, &r);
    for (i = 0; i < 8; i++) {
        if (w->reg_entry[i] == entry &&
            sscanf(gtk_editable_get_text(GTK_EDITABLE(entry)), "%4X", &v) ==
                1) {
            reg_set(&r, i, (uint16_t)v);
            adamdebug_set_regs(w->dbg, &r);
            refresh_regs(w);
            refresh_disasm(w);
        }
    }
}

static void on_trace_toggle(GObject *sw, GParamSpec *pspec, gpointer ud)
{
    DbgWin *w = ud;
    (void)pspec;
    adamdebug_trace_enable(w->dbg,
                           gtk_switch_get_active(GTK_SWITCH(sw)) ? 1 : 0);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint keycode,
                       GdkModifierType state, gpointer ud)
{
    DbgWin *w = ud;
    (void)c;
    (void)keycode;
    switch (keyval) {
    case GDK_KEY_F5:
        on_pause_continue(NULL, w);
        return TRUE;
    case GDK_KEY_F7:
        adamdebug_step_into(w->dbg);
        return TRUE;
    case GDK_KEY_F8:
        if (state & GDK_SHIFT_MASK)
            adamdebug_step_out(w->dbg);
        else
            adamdebug_step_over(w->dbg);
        return TRUE;
    default:
        return FALSE;
    }
}

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    /* Live views while running; the stop path refreshes everything. */
    if (!adamdebug_is_paused(w->dbg)) {
        refresh_vdp(w);
        refresh_regs(w);
    }
    return G_SOURCE_CONTINUE;
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    adamdebug_set_stop_callback(w->dbg, NULL, NULL);
    if (w->tick_timer)
        g_source_remove(w->tick_timer);
    g_free(w);
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *mono_view(GtkTextView **out, gboolean wrap)
{
    GtkWidget *view = gtk_text_view_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),
                                wrap ? GTK_WRAP_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

void adam_debugger_show(GtkWindow *parent, adamsession *session)
{
    static GtkWindow *singleton;
    DbgWin *w;
    GtkWidget *tb, *toolbar, *root, *paned, *right, *notebook;
    GtkWidget *btn, *entry, *grid, *vdp_grid, *scroll;
    GtkEventController *keys;
    GtkGesture *click;
    int i;

    if (singleton) {
        gtk_window_present(singleton);
        return;
    }

    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = adamsession_debugger(session);
    w->follow_pc = 1;
    w->mem_base = 0xFC30;

    w->win = GTK_WINDOW(adw_window_new());
    singleton = w->win;
    g_object_add_weak_pointer(G_OBJECT(w->win), (gpointer *)&singleton);
    gtk_window_set_title(w->win, "ADAM Debugger");
    gtk_window_set_default_size(w->win, 1180, 820);
    gtk_window_set_transient_for(w->win, parent);

    /* Toolbar */
    toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(toolbar, 8);
    gtk_widget_set_margin_end(toolbar, 8);
    gtk_widget_set_margin_top(toolbar, 6);
    gtk_widget_set_margin_bottom(toolbar, 6);
    w->pause_btn = GTK_BUTTON(gtk_button_new_with_label("Pause (F5)"));
    g_signal_connect(w->pause_btn, "clicked",
                     G_CALLBACK(on_pause_continue), w);
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(w->pause_btn));
    btn = gtk_button_new_with_label("Step Into (F7)");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_step_into), w);
    gtk_box_append(GTK_BOX(toolbar), btn);
    btn = gtk_button_new_with_label("Step Over (F8)");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_step_over), w);
    gtk_box_append(GTK_BOX(toolbar), btn);
    btn = gtk_button_new_with_label("Step Out (Shift+F8)");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_step_out), w);
    gtk_box_append(GTK_BOX(toolbar), btn);

    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Go to addr / symbol");
    g_signal_connect(entry, "activate", G_CALLBACK(on_goto), w);
    gtk_box_append(GTK_BOX(toolbar), entry);

    gtk_box_append(GTK_BOX(toolbar), gtk_label_new("Trace:"));
    w->trace_switch = GTK_SWITCH(gtk_switch_new());
    gtk_widget_set_valign(GTK_WIDGET(w->trace_switch), GTK_ALIGN_CENTER);
    g_signal_connect(w->trace_switch, "notify::active",
                     G_CALLBACK(on_trace_toggle), w);
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(w->trace_switch));

    w->status = GTK_LABEL(gtk_label_new("Running"));
    gtk_label_set_xalign(w->status, 1);
    gtk_widget_set_hexpand(GTK_WIDGET(w->status), TRUE);
    gtk_box_append(GTK_BOX(toolbar), GTK_WIDGET(w->status));

    /* Left: disassembly (click gutter to toggle a breakpoint). */
    scroll = mono_view(&w->disasm, FALSE);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm),
                              GTK_EVENT_CONTROLLER(click));

    /* Right column: registers, breakpoints, memory. */
    right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(right, 8);
    gtk_widget_set_margin_end(right, 8);

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    for (i = 0; i < 8; i++) {
        GtkWidget *lab = gtk_label_new(reg_names[i]);
        GtkWidget *ent = gtk_entry_new();
        gtk_editable_set_max_width_chars(GTK_EDITABLE(ent), 6);
        gtk_editable_set_width_chars(GTK_EDITABLE(ent), 6);
        w->reg_entry[i] = GTK_ENTRY(ent);
        g_signal_connect(ent, "activate", G_CALLBACK(on_reg_activate), w);
        gtk_grid_attach(GTK_GRID(grid), lab, (i % 4) * 2, i / 4, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), ent, (i % 4) * 2 + 1, i / 4, 1, 1);
    }
    w->flags_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(w->flags_label, 0);
    gtk_box_append(GTK_BOX(right), labeled("Registers (Enter applies while "
                                           "paused)", grid));
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(w->flags_label));

    w->bp_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_entry, "Add: addr or symbol");
    g_signal_connect(w->bp_entry, "activate", G_CALLBACK(on_bp_add), w);
    btn = gtk_button_new_with_label("Clear all");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_bp_clear_all), w);
    {
        GtkWidget *bpbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_hexpand(GTK_WIDGET(w->bp_entry), TRUE);
        gtk_box_append(GTK_BOX(bpbox), GTK_WIDGET(w->bp_entry));
        gtk_box_append(GTK_BOX(bpbox), btn);
        gtk_box_append(GTK_BOX(right), labeled("Breakpoints", bpbox));
    }
    scroll = mono_view(&w->bp_view, FALSE);
    gtk_widget_set_size_request(scroll, -1, 90);
    gtk_box_append(GTK_BOX(right), scroll);

    w->mem_addr = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->mem_addr, "Memory addr / symbol");
    g_signal_connect(w->mem_addr, "activate", G_CALLBACK(on_mem_addr), w);
    gtk_box_append(GTK_BOX(right),
                   labeled("Memory", GTK_WIDGET(w->mem_addr)));
    scroll = mono_view(&w->mem_view, FALSE);
    gtk_box_append(GTK_BOX(right), scroll);

    paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    {
        GtkWidget *dis_scroll = gtk_widget_get_parent(GTK_WIDGET(w->disasm));
        gtk_paned_set_start_child(GTK_PANED(paned), dis_scroll);
    }
    gtk_paned_set_end_child(GTK_PANED(paned), right);
    gtk_paned_set_position(GTK_PANED(paned), 560);

    /* VDP page */
    vdp_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(vdp_grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(vdp_grid), 8);
    gtk_widget_set_margin_start(vdp_grid, 8);
    gtk_widget_set_margin_top(vdp_grid, 8);

    w->pic_nt = GTK_PICTURE(gtk_picture_new());
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_nt), 512, 384);
    w->pic_pat = GTK_PICTURE(gtk_picture_new());
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_pat), 512, 128);
    w->pic_spr = GTK_PICTURE(gtk_picture_new());
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_spr), 256, 128);
    w->pic_pal = GTK_PICTURE(gtk_picture_new());
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_pal), 256, 16);
    {
        const char *const banks[] = {"Bank 0", "Bank 1", "Bank 2", NULL};
        w->pat_bank = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(banks));
    }
    gtk_grid_attach(GTK_GRID(vdp_grid), labeled("Nametable",
                                                GTK_WIDGET(w->pic_nt)),
                    0, 0, 1, 2);
    {
        GtkWidget *patbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_append(GTK_BOX(patbox), GTK_WIDGET(w->pat_bank));
        gtk_box_append(GTK_BOX(patbox), GTK_WIDGET(w->pic_pat));
        gtk_grid_attach(GTK_GRID(vdp_grid), labeled("Patterns", patbox), 1,
                        0, 1, 1);
    }
    {
        GtkWidget *sprbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *si_scroll = mono_view(&w->sprite_info, FALSE);
        gtk_widget_set_size_request(si_scroll, 300, 200);
        gtk_box_append(GTK_BOX(sprbox), GTK_WIDGET(w->pic_spr));
        gtk_box_append(GTK_BOX(sprbox), si_scroll);
        gtk_grid_attach(GTK_GRID(vdp_grid), labeled("Sprites", sprbox), 1, 1,
                        1, 1);
    }
    gtk_grid_attach(GTK_GRID(vdp_grid), labeled("Palette",
                                                GTK_WIDGET(w->pic_pal)),
                    0, 2, 1, 1);

    /* Trace page */
    scroll = mono_view(&w->trace_view, FALSE);

    notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), paned,
                             gtk_label_new("CPU"));
    {
        GtkWidget *vdp_scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(vdp_scroll),
                                      vdp_grid);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vdp_scroll,
                                 gtk_label_new("VDP"));
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), scroll,
                             gtk_label_new("Trace"));

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    tb = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tb), adw_header_bar_new());
    gtk_box_append(GTK_BOX(root), toolbar);
    gtk_box_append(GTK_BOX(root), notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tb), root);
    adw_window_set_content(ADW_WINDOW(w->win), tb);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    adamdebug_set_stop_callback(w->dbg, on_stop, w);
    w->tick_timer = g_timeout_add(100, tick_refresh, w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    /* Dev affordance: ADAM_DEBUGGER_TAB=vdp|trace selects the start tab. */
    {
        const char *tab = g_getenv("ADAM_DEBUGGER_TAB");
        if (tab && strcmp(tab, "vdp") == 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
        else if (tab && strcmp(tab, "trace") == 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 2);
    }

    refresh_all(w);
    gtk_window_present(w->win);
}
