/*
 * Debugger window (GTK4/libadwaita) over the session's Z80 debugger engine,
 * via core/include/adamdebug.h (the engine mirrors the SMS and MSX
 * siblings' smsdebug.h / msxdebug.h, and this window follows theirs).
 *
 * Tabs (ADAM_DEBUGGER_TAB takes the number, or the start of the name):
 *   0 Console          the contract's command set, Tab completion, history
 *   1 CPU & Memory     the Z80 registers and flags, a memory dump with
 *                      go-to and poke
 *   2 Disassembly      breakpoint gutter, follow-PC, jump box, selectable row
 *   3 VDP              decoded registers, nametable / pattern / sprite /
 *                      palette pictures and a VRAM hex editor
 *   4 Breakpoints      execute, read/write watchpoints, port in/out,
 *                      conditions
 *   5 Memory Map & I/O the bank map, the ports and the VDP registers
 *   6 FujiNet          every AdamNet packet on the BoIP link, with hex
 * A toolbar carries the stepping controls, symbol loading and file saves;
 * the family's keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step
 * out, Shift+F7 frame, F9 breakpoint on the selected line, F12 close).
 * ADAM_OPEN_DEBUGGER=1 opens the window at startup (window.c).
 *
 * The engine is engaged only while this window is showing: showing it
 * attaches (which stops the machine, as on every sibling), hiding it
 * detaches and lets the machine run on at full speed. F12 in the main
 * window toggles it. The window itself is created once and hidden, not
 * destroyed.
 *
 * The window polls the engine's generation counter on a short timer and
 * refreshes only the tab showing, and only when something changed, so a
 * stopped machine costs nothing and a running one shows live values twice a
 * second. It does not use the engine's stop callback: that fires off the UI
 * thread, and the generation counter says the same thing.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <stdlib.h>
#include <string.h>

#include "adamdebug.h"
#include "../window.h"

#define DISASM_WINDOW 48
#define NREGS 17
#define MEM_ROWS 32
#define MEM_PAGE (MEM_ROWS * 16)
#define VRAM_ROWS 16
#define VRAM_PAGE (VRAM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512

typedef struct {
    GtkWindow *win;
    adamsession *session;
    adamdebug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    gboolean closing;         /* the window is being destroyed: touch nothing */
    int running_ticks;
    guint timer;

    AdwWindowTitle *title;    /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* console */
    GtkTextView *console_out;
    GtkEntry *console_in;
    GPtrArray *history;
    guint history_pos;

    /* cpu + memory */
    GtkEntry *reg[NREGS];
    GtkCheckButton *flag[6];  /* S Z H P/V N C */
    GtkLabel *beam;
    gboolean updating_flags;
    gboolean force_entries;   /* a refresh may overwrite a focused entry */
    GtkTextView *mem_view;
    GtkEntry *mem_goto, *mem_write;
    int mem_base;
    int mem_mark;             /* the address Go to found, marked; -1 none */

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first instruction */
    int line_addr[DISASM_WINDOW * 2];   /* per buffer line; -1 for a label line */
    int line_count;
    int selected;             /* the selected row's address, -1 none */

    /* vdp */
    GtkPicture *pic_nt, *pic_pat, *pic_spr, *pic_pal;
    GtkDropDown *pat_bank;
    GtkTextView *sprite_info;
    GtkTextView *vdp_state;
    GtkEntry *vram_entry;
    GtkLabel *vram_status;
    GtkTextView *vram_view;
    uint16_t vram_base;

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkLabel *bp_msg;
    GString *bp_sig;          /* what the list shows, to skip a needless rebuild */

    /* memory map & i/o */
    GtkTextView *io_text;

    /* fujinet */
    GtkLabel *fuji_note;
    GtkTextView *fuji_text, *fuji_state;
    uint64_t fuji_last;
    int fuji_count;
} DbgWin;

static DbgWin *g_win;

/* The register entries, each with how many hex digits it shows. */
static const struct { const char *name; int reg; int digits; } k_regs[NREGS] = {
    { "PC", ADAM_REG_PC, 4 }, { "SP", ADAM_REG_SP, 4 }, { "AF", ADAM_REG_AF, 4 },
    { "BC", ADAM_REG_BC, 4 }, { "DE", ADAM_REG_DE, 4 }, { "HL", ADAM_REG_HL, 4 },
    { "IX", ADAM_REG_IX, 4 }, { "IY", ADAM_REG_IY, 4 },
    { "AF'", ADAM_REG_AF2, 4 }, { "BC'", ADAM_REG_BC2, 4 }, { "DE'", ADAM_REG_DE2, 4 },
    { "HL'", ADAM_REG_HL2, 4 }, { "I", ADAM_REG_I, 2 }, { "R", ADAM_REG_R, 2 },
    { "IM", ADAM_REG_IM, 1 }, { "IFF1", ADAM_REG_IFF1, 1 }, { "IFF2", ADAM_REG_IFF2, 1 },
};
#define REGS_FIRST_ROW 8

/* The tabs, in order; ADAM_DEBUGGER_TAB takes a number or the start of a name. */
static const char *const k_tab_names[] = {
    "Console", "CPU & Memory", "Disassembly", "VDP", "Breakpoints", "Memory Map & I/O",
    "FujiNet",
};
enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_VDP, TAB_BREAKS, TAB_IO, TAB_FUJINET };

/* ---- helpers -------------------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *v = g_utf8_make_valid(text, -1), *s, *d;
    for (s = d = v; *s; s++)
        if ((unsigned char)*s >= 0x20 || *s == '\n' || *s == '\t') *d++ = *s;
    *d = '\0';
    return v;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

/* A log (the console, the FujiNet packets) follows its end while its
 * reader is there. New text grows the view only once it is laid out (or its
 * tab first shows), so the end is pinned whenever the view's height
 * changes; whether to follow is decided as text arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    /* tied to the view: the adjustment can outlive it while the window dies */
    g_signal_connect_object(vadj(view), "changed", G_CALLBACK(on_log_resized), view, 0);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 ||
        gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
            gtk_adjustment_get_upper(adj) - 2.0));
}

/* Replaces a view's text where the reader left it: a refresh (a step, or
 * the live values twice a second) must not throw a scrolled dump back to
 * its top. The top line is noted and scrolled back to once the new text is
 * laid out (scroll_to_mark waits for that). A log that is following goes
 * to its end instead. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextIter it;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &it, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&it);
    }
    gtk_text_buffer_set_text(b, clean, -1);
    g_free(clean);
    if (follow) {
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
        if (adj) on_log_resized(adj, view);
        return;
    }
    if (top > 0) {
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(b, &it, top);
        mark = gtk_text_buffer_create_mark(b, NULL, &it, TRUE);
        gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(b, mark);
    }
}

static void set_text(GtkTextView *view, const char *text)
{
    set_text_at(view, text, FALSE);
}

static void append_text(GtkTextView *view, const char *text, gboolean force_follow)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    char *clean = clean_text(text);
    log_note_reader(view);
    if (force_follow)
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, clean, -1);
    g_free(clean);
}

/* A number as typed: hex by default, $ or 0x or a trailing h for hex, #
 * for decimal. */
static int parse_num(const char *text, long *out)
{
    char buf[64], *end;
    size_t n;
    long v;
    while (*text == ' ') text++;
    g_strlcpy(buf, text, sizeof buf);
    n = strlen(buf);
    while (n && buf[n - 1] == ' ') buf[--n] = '\0';
    if (!n) return 0;
    if (buf[0] == '$') v = strtol(buf + 1, &end, 16);
    else if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) v = strtol(buf + 2, &end, 16);
    else if (buf[0] == '#') v = strtol(buf + 1, &end, 10);
    else if (buf[n - 1] == 'h' || buf[n - 1] == 'H') { buf[n - 1] = '\0'; v = strtol(buf, &end, 16); }
    else v = strtol(buf, &end, 16);
    if (end == buf || *end) return 0;
    *out = v;
    return 1;
}

/* An address box: a label (as the engine does, a name wins over a hex
 * literal), bare hex, or any expression the console takes ($C000+4, pc,
 * hl, [sp], label+2). 0 with the reason in err. */
static int parse_addr(DbgWin *w, const char *text, long *out, char *err, int errsz)
{
    char buf[128];
    long v;
    int a;
    g_strlcpy(buf, text, sizeof buf);
    g_strstrip(buf);
    if (!buf[0]) {
        g_strlcpy(err, "Enter an address, a label or an expression", errsz);
        return 0;
    }
    a = adamdebug_label_address(w->dbg, buf);
    if (a >= 0) { *out = a & 0xFFFF; return 1; }
    if (parse_num(buf, &v)) {
        if (v < 0 || v > 0xFFFF) {
            g_strlcpy(err, "Out of range (0 to $FFFF)", errsz);
            return 0;
        }
        *out = v;
        return 1;
    }
    if (adamdebug_eval(w->dbg, buf, &v, err, errsz) != 0) {
        if (!err[0]) g_strlcpy(err, "Not an address or a label", errsz);
        return 0;
    }
    *out = v & 0xFFFF;
    return 1;
}

/* A refresh does not overwrite a value being typed: a focused entry whose
 * text is not what was last put there keeps it, until Enter (force_entries)
 * says to take the engine's. */
static void set_entry(DbgWin *w, GtkEntry *e, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(e));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    const char *shown = g_object_get_data(G_OBJECT(e), "shown");
    if (focus && !w->force_entries && shown &&
        (focus == GTK_WIDGET(e) || gtk_widget_is_ancestor(focus, GTK_WIDGET(e))) &&
        strcmp(gtk_editable_get_text(GTK_EDITABLE(e)), shown) != 0)
        return;
    gtk_editable_set_text(GTK_EDITABLE(e), text);
    g_object_set_data_full(G_OBJECT(e), "shown", g_strdup(text), g_free);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean wrap)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

/* A heading over a block (the VDP and FujiNet panes). */
static GtkWidget *headed(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "heading");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *w)
{
    gtk_widget_set_margin_top(w, 8);
    gtk_widget_set_margin_bottom(w, 8);
    gtk_widget_set_margin_start(w, 8);
    gtk_widget_set_margin_end(w, 8);
    return w;
}

/* The accent is light: black text on it, in light and dark. */
static void accent_tag(GtkTextView *view, const char *name)
{
    char rgb[16];
    g_snprintf(rgb, sizeof rgb, "#%06X", ADAMSESSION_ACCENT_RGB);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name,
                               "paragraph-background", rgb, "foreground", "#000000", NULL);
}

/* Messages from Load Symbols and Save land in the console's transcript and,
 * since the console may not be the tab showing, in a toast. */
static void notice(DbgWin *w, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(w->console_out, line, FALSE);
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

static void toast(DbgWin *w, const char *msg)
{
    adw_toast_overlay_add_toast(w->toasts, adw_toast_new(msg));
}

/* The step controls wait a moment for the engine to come to rest, so a
 * step does not flash "Running" before the status catches up. */
static void settle(DbgWin *w, gboolean want_stopped)
{
    int i;
    for (i = 0; i < 20 && (adamdebug_is_stopped(w->dbg) != 0) != want_stopped; i++)
        g_usleep(2000);
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *w)
{
    adamdebug_cpu c;
    char buf[160];
    int i;
    adamdebug_cpu_get(w->dbg, &c);
    {
        const int vals[NREGS] = { c.pc, c.sp, c.af, c.bc, c.de, c.hl, c.ix, c.iy,
                                  c.af2, c.bc2, c.de2, c.hl2, c.i, c.r, c.im,
                                  c.iff1, c.iff2 };
        for (i = 0; i < NREGS; i++) {
            g_snprintf(buf, sizeof buf, "%0*X", k_regs[i].digits, (unsigned)vals[i]);
            set_entry(w, w->reg[i], buf);
        }
    }
    {
        const int flags[6] = { c.sf, c.zf, c.hf, c.pf, c.nf, c.cf };
        w->updating_flags = TRUE;
        for (i = 0; i < 6; i++)
            gtk_check_button_set_active(w->flag[i], flags[i] != 0);
        w->updating_flags = FALSE;
    }
    g_snprintf(buf, sizeof buf, "line %d   frame %u   cycle %llu%s", c.line, c.frame,
               (unsigned long long)c.cycles, c.halted ? "   HALT" : "");
    gtk_label_set_text(w->beam, buf);
}

static void refresh_mem(DbgWin *w)
{
    uint8_t data[MEM_PAGE];
    GString *str = g_string_new(NULL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->mem_view);
    int row, col;

    memset(data, 0, sizeof data);
    adamdebug_read(w->dbg, (uint16_t)w->mem_base, data, MEM_PAGE);
    g_string_append(str, "      0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");
    for (row = 0; row < MEM_ROWS; row++) {
        g_string_append_printf(str, "%04X  ", w->mem_base + row * 16);
        for (col = 0; col < 16; col++)
            g_string_append_printf(str, "%02X ", data[row * 16 + col]);
        g_string_append_c(str, ' ');
        for (col = 0; col < 16; col++) {
            uint8_t ch = data[row * 16 + col];
            g_string_append_c(str, (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '.');
        }
        g_string_append_c(str, '\n');
    }
    set_text(w->mem_view, str->str);
    g_string_free(str, TRUE);

    /* the row Go to found stays marked across refreshes */
    if (w->mem_mark >= w->mem_base && w->mem_mark < w->mem_base + MEM_PAGE) {
        GtkTextIter s, e;
        int line = (w->mem_mark - w->mem_base) / 16 + 1;
        gtk_text_buffer_get_iter_at_line(b, &s, line);
        gtk_text_buffer_get_iter_at_line(b, &e, line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "mark", &s, &e);
    }
}

static void refresh_disasm(DbgWin *w)
{
    static adamdebug_line lines[DISASM_WINDOW];
    int dim_from[DISASM_WINDOW * 2];    /* per buffer line: grey from this column, -1 none */
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GString *str = g_string_new(NULL);
    GtkTextIter s, e;
    int n, pc_line, i, line = 0, sel_line = -1, pc_buf = -1;
    gsize head;

    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(w->follow_pc)) {
        adamdebug_cpu c;
        adamdebug_cpu_get(w->dbg, &c);
        w->disasm_top = adamdebug_row_address(w->dbg, (uint16_t)c.pc, -(DISASM_WINDOW / 3));
        if (w->disasm_top > c.pc) w->disasm_top = 0;   /* near $0000 it wraps: start at the bottom */
    }

    n = adamdebug_disassemble(w->dbg, (uint16_t)w->disasm_top, lines, DISASM_WINDOW, &pc_line);
    for (i = 0; i < n; i++) {
        /* a label gets a line of its own above its instruction */
        if (lines[i].label[0]) {
            g_string_append_printf(str, "      %s:\n", lines[i].label);
            dim_from[line] = 0;
            w->line_addr[line++] = -1;
        }
        head = str->len;
        g_string_append_printf(str, "%s%c %04X  %-11s %-16s",
                               lines[i].has_breakpoint ? "\xe2\x97\x8f" : " ",
                               lines[i].is_pc ? '>' : ' ',
                               lines[i].address, lines[i].bytes, lines[i].disasm);
        dim_from[line] = -1;
        if (lines[i].comment[0]) {
            dim_from[line] = (int)g_utf8_strlen(str->str + head, -1);
            g_string_append_printf(str, "; %s", lines[i].comment);
        }
        g_string_append_c(str, '\n');
        if (lines[i].address == w->selected) sel_line = line;
        if (i == pc_line) pc_buf = line;
        w->line_addr[line++] = lines[i].address;
    }
    w->line_count = line;
    if (n == 0) g_string_append(str, "(no disassembly)\n");
    gtk_text_buffer_set_text(b, str->str, -1);
    g_string_free(str, TRUE);

    /* labels and comments in grey */
    for (i = 0; i < line; i++) {
        if (dim_from[i] < 0) continue;
        gtk_text_buffer_get_iter_at_line(b, &s, i);
        if (dim_from[i] > 0) gtk_text_iter_set_line_offset(&s, dim_from[i]);
        e = s;
        gtk_text_iter_forward_to_line_end(&e);
        gtk_text_buffer_apply_tag_by_name(b, "dim", &s, &e);
    }
    if (sel_line >= 0) {
        gtk_text_buffer_get_iter_at_line(b, &s, sel_line);
        gtk_text_buffer_get_iter_at_line(b, &e, sel_line + 1);
        gtk_text_buffer_apply_tag_by_name(b, "sel", &s, &e);
    }
    /* the PC line in the accent colour (over the selection) */
    if (pc_buf >= 0) {
        gtk_text_buffer_get_iter_at_line(b, &s, pc_buf);
        gtk_text_buffer_get_iter_at_line(b, &e, pc_buf + 1);
        gtk_text_buffer_apply_tag_by_name(b, "pc", &s, &e);
    }
    /* the gutter marker: red while the breakpoint is enabled, grey off (it
     * keeps its colour on the PC line) */
    for (i = 0, line = 0; i < n; i++) {
        if (lines[i].label[0]) line++;
        if (lines[i].has_breakpoint) {
            gtk_text_buffer_get_iter_at_line(b, &s, line);
            e = s;
            gtk_text_iter_forward_char(&e);
            gtk_text_buffer_apply_tag_by_name(b, lines[i].breakpoint_enabled ? "bp_on" : "bp_off",
                                              &s, &e);
        }
        line++;
    }
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
        pal[ADAMVDP_PAL_W * ADAMVDP_PAL_H * 4];
    static char text[4096];
    adamvdp_sprite info[32];
    GdkTexture *tex;
    GString *str;
    int i;

    adamdebug_vdp_snapshot(w->dbg, &snap);

    adamvdp_render_nametable(&snap, nt);
    tex = texture_from_rgba(nt, 256, 192);
    gtk_picture_set_paintable(w->pic_nt, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    adamvdp_render_patterns(&snap, (int)gtk_drop_down_get_selected(w->pat_bank), pat);
    tex = texture_from_rgba(pat, 256, 64);
    gtk_picture_set_paintable(w->pic_pat, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    adamvdp_render_sprites(&snap, spr, info);
    tex = texture_from_rgba(spr, 128, 64);
    gtk_picture_set_paintable(w->pic_spr, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    adamvdp_render_palette(&snap, pal);
    tex = texture_from_rgba(pal, ADAMVDP_PAL_W, ADAMVDP_PAL_H);
    gtk_picture_set_paintable(w->pic_pal, GDK_PAINTABLE(tex));
    g_object_unref(tex);

    str = g_string_new("##  Y    X  PAT CLR EC\n");
    for (i = 0; i < 32; i++)
        g_string_append_printf(str, "%02d %3d  %3d  %02X  %2d  %d\n", i,
                               info[i].y, info[i].x, info[i].pattern,
                               info[i].color, info[i].early_clock);
    set_text(w->sprite_info, str->str);
    g_string_free(str, TRUE);

    adamvdp_format_state(&snap, text, (int)sizeof text);
    set_text(w->vdp_state, text);

    adamvdp_format_hex(&snap, w->vram_base, VRAM_ROWS, text, (int)sizeof text);
    set_text(w->vram_view, text);
}

static void refresh_io(DbgWin *w)
{
    static char text[8192];
    adamdebug_io_state(w->dbg, text, sizeof text);
    set_text(w->io_text, text);
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat folded into the newest. */
static void refresh_fujinet(DbgWin *w, gboolean force)
{
    static adam_fuji_packet p[TRACE_MAX];
    static char state[4096];
    char note[160];
    GString *t;
    int n = adamdebug_fujinet_trace(w->dbg, p, TRACE_MAX), i;

    if (!adamsession_fujinet_running(w->session))
        g_strlcpy(note, "FujiNet is not running (FujiNet \xe2\x96\xb8 FujiNet Console Log).",
                  sizeof note);
    else
        g_snprintf(note, sizeof note, "FujiNet is on BoIP port %d, %s.", ADAMSESSION_BOIP_PORT,
                   adamsession_fujinet_link(w->session) ? "connected" : "not connected");
    gtk_label_set_text(w->fuji_note, note);
    adamdebug_fujinet_state(w->dbg, state, (int)sizeof state);
    set_text(w->fuji_state, state);

    if (!force && n == w->fuji_count && (n == 0 || p[n - 1].serial == w->fuji_last))
        return;
    w->fuji_count = n;
    w->fuji_last = n > 0 ? p[n - 1].serial : 0;
    log_note_reader(w->fuji_text);
    t = g_string_new(NULL);
    for (i = 0; i < n; i++) {
        int k;
        g_string_append_printf(t, "%s%s", i ? "\n" : "", p[i].text);
        if (p[i].repeat > 1) g_string_append_printf(t, " \xc3\x97%u", p[i].repeat);
        g_string_append(t, "\n     ");
        for (k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++)
            g_string_append_printf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) g_string_append(t, "\xe2\x80\xa6");
    }
    if (n == 0)
        g_string_append(t, "(no AdamNet packets since the trace was cleared)");
    set_text_at(w->fuji_text, t->str, force || following(w->fuji_text));
    g_string_free(t, TRUE);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud);
static void bp_remove_clicked(GtkButton *b, gpointer ud);

static const char *bp_type_name(int type)
{
    if (type & ADAMDEBUG_BP_EXEC) return "Execute";
    if ((type & (ADAMDEBUG_BP_READ | ADAMDEBUG_BP_WRITE)) == (ADAMDEBUG_BP_READ | ADAMDEBUG_BP_WRITE))
        return "Read/Write";
    if (type & ADAMDEBUG_BP_READ) return "Read";
    if (type & ADAMDEBUG_BP_WRITE) return "Write";
    if ((type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) == (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT))
        return "In/Out";
    if (type & ADAMDEBUG_BP_IN) return "Port In";
    return "Port Out";
}

static void refresh_bps(DbgWin *w)
{
    static adamdebug_breakpoint bps[128];
    GtkWidget *child;
    GString *sig = g_string_new(NULL);
    int n = adamdebug_breakpoint_list(w->dbg, bps, 128), i;

    /* the list is rebuilt only when what it shows changed (hit counts move
     * while the machine runs), so a click on a row is not pulled out from
     * under the pointer */
    for (i = 0; i < n; i++)
        g_string_append_printf(sig, "%d/%d/%u-%u/%d/%u/%s\n", bps[i].id, bps[i].type,
                               bps[i].start, bps[i].end, bps[i].enabled, bps[i].hits,
                               bps[i].condition);
    if (w->bp_sig && g_string_equal(sig, w->bp_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (w->bp_sig) g_string_free(w->bp_sig, TRUE);
    w->bp_sig = sig;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->bp_list))) != NULL)
        gtk_list_box_remove(w->bp_list, child);
    if (n == 0) {
        GtkWidget *l = gtk_label_new("No breakpoints. Select a disassembly line and press F9, "
                                     "or add one below.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_list_box_append(w->bp_list, l);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *en = gtk_check_button_new();
        GtkWidget *rm = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *l;
        char text[320], range[32];
        const gboolean port = (bps[i].type & (ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT)) != 0;
        if (bps[i].end != bps[i].start)
            g_snprintf(range, sizeof range, port ? "$%02X-$%02X" : "$%04X-$%04X",
                       bps[i].start, bps[i].end);
        else
            g_snprintf(range, sizeof range, port ? "$%02X" : "$%04X", bps[i].start);
        g_snprintf(text, sizeof text, "#%-3d %-10s %-12s hits %-6u %s%s", bps[i].id,
                   bp_type_name(bps[i].type), range, bps[i].hits,
                   bps[i].condition[0] ? "if " : "", bps[i].condition);
        l = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_widget_add_css_class(l, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(en), bps[i].enabled != 0);
        gtk_widget_set_tooltip_text(en, "Enabled");
        g_object_set_data(G_OBJECT(en), "bp", GINT_TO_POINTER(bps[i].id));
        g_object_set_data(G_OBJECT(rm), "bp", GINT_TO_POINTER(bps[i].id));
        g_signal_connect(en, "toggled", G_CALLBACK(bp_enable_toggled), w);
        g_signal_connect(rm, "clicked", G_CALLBACK(bp_remove_clicked), w);
        gtk_widget_add_css_class(rm, "flat");
        gtk_widget_set_tooltip_text(rm, "Remove");
        gtk_box_append(GTK_BOX(row), en);
        gtk_box_append(GTK_BOX(row), l);
        gtk_box_append(GTK_BOX(row), rm);
        gtk_list_box_append(w->bp_list, row);
    }
}

static void refresh_status(DbgWin *w)
{
    char reason[160], text[256];
    int addr;
    gboolean stopped = adamdebug_is_stopped(w->dbg) != 0;
    adamdebug_stop_reason(w->dbg, reason, sizeof reason, &addr);
    if (stopped) {
        const gboolean plain = !reason[0] || strcmp(reason, "stopped") == 0;
        g_snprintf(text, sizeof text, "Stopped%s%s", plain ? "" : ": ", plain ? "" : reason);
        if (addr >= 0 && !strchr(reason, '$'))
            g_snprintf(text + strlen(text), sizeof text - strlen(text), " at $%04X", addr);
    } else
        g_snprintf(text, sizeof text, "Running");
    adw_window_title_set_subtitle(w->title, text);
    gtk_button_set_label(w->run_btn, stopped ? "Run (F5)" : "Stop (F5)");
    if (stopped) gtk_widget_add_css_class(GTK_WIDGET(w->run_btn), "adam-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(w->run_btn), "adam-accent");
}

/* Only the tab showing is brought up to date; the others catch up when
 * their tab is switched to. */
static void refresh_tab(DbgWin *w, int tab)
{
    if (w->closing) return;
    switch (tab) {
    case TAB_CPU: refresh_cpu(w); refresh_mem(w); break;
    case TAB_DISASM: refresh_disasm(w); break;
    case TAB_VDP: refresh_vdp(w); break;
    case TAB_BREAKS: refresh_bps(w); break;
    case TAB_IO: refresh_io(w); break;
    case TAB_FUJINET: refresh_fujinet(w, FALSE); break;
    default: break;
    }
}

/* The status and the tab showing, and the generation they now reflect. */
static void refresh_now(DbgWin *w)
{
    if (w->closing) return;
    w->seen_generation = adamdebug_generation(w->dbg);
    w->was_stopped = adamdebug_is_stopped(w->dbg) != 0;
    refresh_status(w);
    refresh_tab(w, gtk_notebook_get_current_page(w->notebook));
}

static void attach_and_refresh(DbgWin *w);

static gboolean tick_refresh(gpointer ud)
{
    DbgWin *w = ud;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(w->win))) return G_SOURCE_CONTINUE;
    if (!adamdebug_is_attached(w->dbg)) {
        attach_and_refresh(w);
        return G_SOURCE_CONTINUE;
    }
    gen = adamdebug_generation(w->dbg);
    stopped = adamdebug_is_stopped(w->dbg) != 0;
    if (gen != w->seen_generation || stopped != w->was_stopped) {
        w->running_ticks = 0;
        refresh_now(w);
    } else if (!stopped && ++w->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        w->running_ticks = 0;
        refresh_now(w);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (adamdebug_is_stopped(w->dbg)) {
        adamdebug_resume(w->dbg);
    } else {
        adamdebug_stop(w->dbg);
        settle(w, TRUE);
    }
    refresh_now(w);
}

/* A step: the machine is stopped, runs a moment, stops again. */
#define STEP_HANDLER(name, call) \
    static void name(GtkButton *b, gpointer ud) \
    { \
        DbgWin *w = ud; \
        (void)b; \
        call; \
        settle(w, TRUE); \
        refresh_now(w); \
    }
STEP_HANDLER(on_step, adamdebug_step(w->dbg))
STEP_HANDLER(on_step_over, adamdebug_step_over(w->dbg))
STEP_HANDLER(on_step_out, adamdebug_step_out(w->dbg))
STEP_HANDLER(on_frame, adamdebug_frame(w->dbg, 1))
#undef STEP_HANDLER

static void on_run_to_cursor(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    if (w->selected < 0) {
        toast(w, "Select a disassembly line to run to");
        return;
    }
    adamdebug_run_to(w->dbg, (uint16_t)w->selected);
    settle(w, TRUE);
    refresh_now(w);
}

/* F9 and the gutter: an execute breakpoint on the selected line. */
static void toggle_selected(DbgWin *w)
{
    if (w->selected < 0) {
        toast(w, "Select a disassembly line first");
        return;
    }
    adamdebug_breakpoint_toggle(w->dbg, (uint16_t)w->selected);
    refresh_now(w);
}

static void on_console(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    gboolean was_stopped = adamdebug_is_stopped(w->dbg) != 0;
    if (!cmd || !*cmd) return;
    /* the history keeps each command once in a row */
    if (w->history->len == 0 ||
        strcmp(g_ptr_array_index(w->history, w->history->len - 1), cmd) != 0)
        g_ptr_array_add(w->history, g_strdup(cmd));
    w->history_pos = w->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(w->console_out, line, TRUE);
    out[0] = '\0';
    adamdebug_command(w->dbg, cmd, out, sizeof out);
    append_text(w->console_out, out, TRUE);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(w->console_out, "\n", TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    if (was_stopped) settle(w, TRUE);
    refresh_now(w);
}

static void history_show(DbgWin *w)
{
    const char *text = w->history_pos < w->history->len
                           ? g_ptr_array_index(w->history, w->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(w->console_in), text);
    gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
}

/* Tab completes the current word against the commands, the registers and
 * the labels; several matches are listed instead. Up and Down walk the
 * command history. */
static gboolean on_console_key(GtkEventControllerKey *c, guint keyval, guint code,
                               GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    char comps[4096];
    const char *text;
    const char *word;
    int n;
    (void)c; (void)code; (void)st;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (w->history_pos > 0) {
            w->history_pos--;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (w->history_pos < w->history->len) {
            w->history_pos++;
            history_show(w);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(w->console_in));
    comps[0] = '\0';
    /* the engine completes the end of the whole line (a command, then its
     * arguments), so the whole line goes and the first word is replaced */
    n = adamdebug_completions(w->dbg, text, comps, sizeof comps);
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    if (n == 1) {
        char merged[600];
        char *nl = strchr(comps, '\n');
        if (nl) *nl = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(w->console_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
    } else if (n > 1) {
        /* the common start is filled in; the candidates are listed */
        char **cand = g_strsplit(comps, "\n", -1);
        size_t common = strlen(cand[0]), len = strlen(word);
        int i;
        for (i = 1; cand[i] && *cand[i]; i++) {
            size_t k = 0;
            while (k < common && cand[i][k] == cand[0][k]) k++;
            common = k;
        }
        if (common > len) {
            char merged[600];
            g_snprintf(merged, sizeof merged, "%.*s%.*s", (int)(word - text), text,
                       (int)common, cand[0]);
            gtk_editable_set_text(GTK_EDITABLE(w->console_in), merged);
            gtk_editable_set_position(GTK_EDITABLE(w->console_in), -1);
        }
        g_strfreev(cand);
        append_text(w->console_out, comps, FALSE);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(w->console_out, "\n", FALSE);
    }
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"));
    long v;
    if (!adamdebug_is_stopped(w->dbg))
        toast(w, "Stop the machine to edit the registers");
    else if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &v))
        adamdebug_cpu_set(w->dbg, k_regs[i].reg, (int)v);
    /* Enter ends the edit: show the engine's value, or put the old one back */
    w->force_entries = TRUE;
    refresh_now(w);
    w->force_entries = FALSE;
}

static void on_flag_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "flag"));
    static const int regs[6] = { ADAM_FLAG_S, ADAM_FLAG_Z, ADAM_FLAG_H, ADAM_FLAG_PV,
                                 ADAM_FLAG_N, ADAM_FLAG_C };
    if (w->updating_flags) return;
    if (!adamdebug_is_stopped(w->dbg)) {
        toast(w, "Stop the machine to edit the registers");
        refresh_cpu(w);           /* put the box back */
        return;
    }
    adamdebug_cpu_set(w->dbg, regs[i], gtk_check_button_get_active(b));
    refresh_now(w);
}

/* "address value...": bytes written through the live bank map. */
static void on_mem_write(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    char **parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(entry)), " \t:=,", -1);
    char err[128] = "";
    uint8_t bytes[POKE_MAX];
    long a, v;
    int n = 0, i;
    gboolean ok;
    char *addr = NULL;
    for (i = 0; parts[i]; i++) {
        if (!*parts[i]) continue;
        if (!addr) addr = parts[i];
        else if (n < POKE_MAX && parse_num(parts[i], &v) && v >= 0 && v <= 255)
            bytes[n++] = (uint8_t)v;
        else { n = -1; break; }
    }
    ok = addr && n > 0 && parse_addr(w, addr, &a, err, sizeof err);
    if (ok) {
        for (i = 0; i < n; i++)
            adamdebug_write(w->dbg, (uint16_t)(a + i), bytes[i]);
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        refresh_now(w);
    } else {
        toast(w, err[0] ? err : "Poke takes an address and hex bytes, e.g. C000 41 42");
    }
    g_strfreev(parts);
}

static void mem_set_base(DbgWin *w, int base)
{
    if (base < 0) base = 0;
    if (base > 0x10000 - MEM_PAGE) base = 0x10000 - MEM_PAGE;
    w->mem_base = base & ~0xF;
    refresh_mem(w);
}

/* Go to: the page holding an address (or a label), with its row marked. */
static void on_mem_goto(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    char err[128] = "";
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a, err, sizeof err)) {
        toast(w, err);
        return;
    }
    w->mem_mark = (int)a;
    mem_set_base(w, ((int)a & ~0xF) - 4 * 16);   /* a little context above it */
}

static void on_mem_page(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    mem_set_base(w, w->mem_base + GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "delta")) * MEM_PAGE);
}

/* Click selects a row; a click on the gutter or a double-click toggles its
 * breakpoint (a label line has no address, so it selects nothing). The
 * address comes from the row, never from the text. */
static void on_disasm_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    DbgWin *w = ud;
    GtkTextIter it, third;
    GdkRectangle r;
    int bx, by, line, top, height;
    gboolean gutter;
    (void)g;
    gtk_text_view_window_to_buffer_coords(w->disasm, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    /* by line, not by hit-test: the margin left of the gutter is the gutter */
    gtk_text_view_get_line_at_y(w->disasm, &it, by, NULL);
    gtk_text_view_get_line_yrange(w->disasm, &it, &top, &height);
    if (by < top || by >= top + height) return;
    line = gtk_text_iter_get_line(&it);
    if (line < 0 || line >= w->line_count || w->line_addr[line] < 0) return;
    third = it;
    gtk_text_iter_set_line_offset(&third, 2);
    gtk_text_view_get_iter_location(w->disasm, &third, &r);
    gutter = bx < r.x;
    w->selected = w->line_addr[line];
    if ((n == 1 && gutter) || (n == 2 && !gutter))
        adamdebug_breakpoint_toggle(w->dbg, (uint16_t)w->selected);
    refresh_disasm(w);
}

static void on_follow_toggled(GtkCheckButton *b, gpointer ud)
{
    (void)b;
    refresh_disasm(ud);
}

static void on_jump(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    char err[128] = "";
    long a;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(entry)), &a, err, sizeof err)) {
        toast(w, err);
        return;
    }
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = (int)a;
    refresh_disasm(w);
}

/* The wheel scrolls by instruction. */
static gboolean on_disasm_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer ud)
{
    DbgWin *w = ud;
    int rows = (int)(dy * 3);
    (void)c; (void)dx;
    if (!rows) rows = dy > 0 ? 1 : -1;
    gtk_check_button_set_active(w->follow_pc, FALSE);
    w->disasm_top = adamdebug_row_address(w->dbg, (uint16_t)w->disasm_top, rows);
    refresh_disasm(w);
    return TRUE;
}

static void on_pat_bank(GObject *o, GParamSpec *ps, gpointer ud)
{
    DbgWin *w = ud;
    (void)o; (void)ps;
    if (!w->closing && gtk_widget_get_mapped(GTK_WIDGET(w->win))) refresh_vdp(w);
}

static void on_vram_page(GtkButton *btn, gpointer ud)
{
    DbgWin *w = ud;
    int delta = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "delta"));
    w->vram_base = (uint16_t)((w->vram_base + delta * VRAM_PAGE) & 0x3FFF);
    refresh_vdp(w);
}

/* One entry does both jobs: an address alone scrolls the dump there, an
 * address followed by hex bytes writes them first ("1800: 41 42 43"). */
static void on_vram_entry(GtkEntry *entry, gpointer ud)
{
    DbgWin *w = ud;
    uint8_t bytes[POKE_MAX];
    uint16_t addr;
    char msg[96];
    int n = adamvdp_parse_poke(gtk_editable_get_text(GTK_EDITABLE(entry)), &addr, bytes, POKE_MAX);

    if (n < 0) {
        gtk_label_set_text(w->vram_status, "expected: addr [bytes...], e.g. 1800: 41 42");
        return;
    }
    if (n > 0) {
        adamdebug_vdp_write(w->dbg, addr, bytes, n);
        g_snprintf(msg, sizeof msg, "wrote %d byte%s at $%04X", n, n == 1 ? "" : "s", addr);
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
    } else {
        g_snprintf(msg, sizeof msg, "$%04X", addr);
    }
    gtk_label_set_text(w->vram_status, msg);
    w->vram_base = (uint16_t)(addr & 0x3FF0);
    refresh_vdp(w);
}

static void bp_enable_toggled(GtkCheckButton *b, gpointer ud)
{
    DbgWin *w = ud;
    adamdebug_breakpoint_enable(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")),
                                gtk_check_button_get_active(b));
    refresh_now(w);
}

static void bp_remove_clicked(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    adamdebug_breakpoint_remove(w->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bp")));
    refresh_now(w);
}

static void on_bp_add(GtkWidget *wgt, gpointer ud)
{
    DbgWin *w = ud;
    static const int types[7] = {
        ADAMDEBUG_BP_EXEC, ADAMDEBUG_BP_READ, ADAMDEBUG_BP_WRITE,
        ADAMDEBUG_BP_READ | ADAMDEBUG_BP_WRITE, ADAMDEBUG_BP_IN, ADAMDEBUG_BP_OUT,
        ADAMDEBUG_BP_IN | ADAMDEBUG_BP_OUT };
    guint sel = gtk_drop_down_get_selected(w->bp_type);
    long a, b;
    int id;
    char err[256] = "";
    const char *end = gtk_editable_get_text(GTK_EDITABLE(w->bp_end));
    (void)wgt;
    if (sel >= G_N_ELEMENTS(types)) sel = 0;
    if (!parse_addr(w, gtk_editable_get_text(GTK_EDITABLE(w->bp_start)), &a, err, sizeof err)) {
        gtk_label_set_text(w->bp_msg, err);
        return;
    }
    b = a;
    if (end && *end && !parse_addr(w, end, &b, err, sizeof err)) {
        gtk_label_set_text(w->bp_msg, err);
        return;
    }
    id = adamdebug_breakpoint_add(w->dbg, types[sel], (uint16_t)a, (uint16_t)b,
                                  gtk_editable_get_text(GTK_EDITABLE(w->bp_cond)),
                                  err, sizeof err);
    if (id < 0) {
        char *clean = clean_text(err[0] ? err : "The breakpoint was refused");
        gtk_label_set_text(w->bp_msg, clean);
        g_free(clean);
        return;
    }
    gtk_label_set_text(w->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(w->bp_cond), "");
    refresh_now(w);
}

static void on_bp_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    adamdebug_breakpoint_clear(w->dbg);
    refresh_now(w);
}

static void popdown_of(GtkWidget *b)
{
    GtkWidget *pop = gtk_widget_get_ancestor(b, GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void load_symbols(DbgWin *w, const char *path)
{
    char msg[512] = "";
    adamdebug_load_symbols(w->dbg, path, msg, sizeof msg);
    notice(w, msg);
    refresh_now(w);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file || w->closing) return;
    path = g_file_get_path(file);
    if (path) load_symbols(w, path);
}

static void on_symbols(GtkWidget *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *syms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    const char *cart = adamsession_get_str(w->session, "cart_path", "");
    (void)b;

    gtk_file_filter_set_name(syms, "Symbol files (*.sym, *.map, *.noi, *.txt)");
    gtk_file_filter_add_suffix(syms, "sym");
    gtk_file_filter_add_suffix(syms, "map");
    gtk_file_filter_add_suffix(syms, "noi");
    gtk_file_filter_add_suffix(syms, "txt");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, syms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (HHHH NAME, WLA-DX .sym, z88dk .map, SDCC .noi)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    /* a game's symbols usually sit beside it */
    if (cart && *cart) {
        g_autofree char *dir = g_path_get_dirname(cart);
        g_autoptr(GFile) folder = g_file_new_for_path(dir);
        gtk_file_dialog_set_initial_folder(dlg, folder);
    }
    gtk_file_dialog_open(dlg, w->win, NULL, on_symbols_chosen, w);
    g_object_unref(syms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

/* <cartridge>.sym/.map/.noi next to the opened cartridge, no picker needed. */
static void on_symbols_beside(GtkButton *b, gpointer ud)
{
    popdown_of(GTK_WIDGET(b));
    load_symbols(ud, NULL);
}

static void on_save_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    DbgWin *w = ud;
    g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *kind = g_object_get_data(src, "kind");
    char msg[512] = "";
    if (!file || w->closing) return;
    path = g_file_get_path(file);
    if (!path) return;
    adamdebug_save(w->dbg, kind, path, msg, sizeof msg);
    notice(w, msg);
}

static void on_save(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    const char *kind = g_object_get_data(G_OBJECT(b), "kind");
    char title[64];
    popdown_of(GTK_WIDGET(b));
    g_snprintf(title, sizeof title, "Save %s", (const char *)g_object_get_data(G_OBJECT(b), "title"));
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_initial_name(dlg, g_object_get_data(G_OBJECT(b), "file"));
    g_object_set_data(G_OBJECT(dlg), "kind", (gpointer)kind);
    gtk_file_dialog_save(dlg, w->win, NULL, on_save_chosen, w);
    g_object_unref(dlg);
}

static void on_fuji_clear(GtkButton *b, gpointer ud)
{
    DbgWin *w = ud;
    (void)b;
    adamdebug_fujinet_trace_clear(w->dbg);
    refresh_fujinet(w, TRUE);
}

/* The notebook also switches pages as it is taken apart with the window
 * (main window closing), before the window's "destroy": only a mapped window
 * has a tab to refresh, a hidden one refreshes when it is shown. */
static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, gpointer ud)
{
    DbgWin *w = ud;
    (void)nb; (void)page;
    if (!w->closing && gtk_widget_get_mapped(GTK_WIDGET(w->win)))
        refresh_tab(w, (int)num);
}

/* Closing hides: the machine runs on, and the window is back on F12. */
static void hide_window(DbgWin *w)
{
    adamdebug_detach(w->dbg);
    gtk_widget_set_visible(GTK_WIDGET(w->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code,
                       GdkModifierType st, gpointer ud)
{
    DbgWin *w = ud;
    (void)c; (void)code;
    switch (keyval) {
    case GDK_KEY_F5: on_run(NULL, w); return TRUE;
    case GDK_KEY_F7:
        if (st & GDK_SHIFT_MASK) on_frame(NULL, w); else on_step(NULL, w);
        return TRUE;
    case GDK_KEY_F8:
        if (st & GDK_SHIFT_MASK) on_step_out(NULL, w); else on_step_over(NULL, w);
        return TRUE;
    case GDK_KEY_F9: toggle_selected(w); return TRUE;
    case GDK_KEY_F12: hide_window(w); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer ud)
{
    (void)win;
    hide_window(ud);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

static GtkWidget *build_toolbar(DbgWin *w)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *b, *sym, *sympop, *beside, *save, *pop, *popbox;
    static const struct { const char *kind, *title, *file; } saves[] = {
        { "dis", "Disassembly ($0000-$FFFF)", "disassembly.asm" },
        { "mem", "Memory (64K)", "memory.bin" },
        { "vram", "VRAM (16K)", "vram.bin" },
        { "regs", "Registers (text)", "registers.txt" } };
    unsigned i;

    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, cb) do { b = gtk_button_new_with_label(label); \
    g_signal_connect(b, "clicked", G_CALLBACK(cb), w); gtk_box_append(GTK_BOX(bar), b); } while (0)
    w->run_btn = GTK_BUTTON(gtk_button_new_with_label("Stop (F5)"));
    g_signal_connect(w->run_btn, "clicked", G_CALLBACK(on_run), w);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->run_btn));
    TB("Step (F7)", on_step);
    TB("Step Over (F8)", on_step_over);
    TB("Step Out (\xe2\x87\xa7""F8)", on_step_out);
    TB("Frame (\xe2\x87\xa7""F7)", on_frame);
    TB("Run to Cursor", on_run_to_cursor);
#undef TB

    /* Load Symbols: a file, or (the arrow) the files beside the cartridge */
    sym = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(sym), "Load Symbols\xe2\x80\xa6");
    adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(sym), "More ways to load symbols");
    g_signal_connect(sym, "clicked", G_CALLBACK(on_symbols), w);
    sympop = gtk_popover_new();
    beside = gtk_button_new_with_label("Beside the Cartridge (.sym, .map, .noi)");
    gtk_widget_add_css_class(beside, "flat");
    g_signal_connect(beside, "clicked", G_CALLBACK(on_symbols_beside), w);
    gtk_popover_set_child(GTK_POPOVER(sympop), beside);
    adw_split_button_set_popover(ADW_SPLIT_BUTTON(sym), GTK_POPOVER(sympop));
    gtk_box_append(GTK_BOX(bar), sym);

    save = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(save), "Save\xe2\x80\xa6");
    pop = gtk_popover_new();
    popbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (i = 0; i < G_N_ELEMENTS(saves); i++) {
        GtkWidget *sb = gtk_button_new_with_label(saves[i].title);
        gtk_widget_add_css_class(sb, "flat");
        g_object_set_data(G_OBJECT(sb), "kind", (gpointer)saves[i].kind);
        g_object_set_data(G_OBJECT(sb), "title", (gpointer)saves[i].title);
        g_object_set_data(G_OBJECT(sb), "file", (gpointer)saves[i].file);
        g_signal_connect(sb, "clicked", G_CALLBACK(on_save), w);
        gtk_box_append(GTK_BOX(popbox), sb);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), popbox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(save), pop);
    gtk_box_append(GTK_BOX(bar), save);
    return bar;
}

static GtkWidget *build_console(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&w->console_out, TRUE);
    GtkEventController *keys;

    make_log(w->console_out);
    w->console_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->console_in,
        "Command (help, step, break, bpw, bpin, mem, poke, disasm, regs, ...) "
        "\xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(w->console_in), TRUE);
    g_signal_connect(w->console_in, "activate", G_CALLBACK(on_console), w);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->console_in), keys);
    w->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->console_in));
    set_text_at(w->console_out, "ADAM debugger console (Z80). Type 'help' for every command. "
                                "Numbers are decimal; $ or 0x for hex.\n", TRUE);
    return padded(box);
}

static GtkWidget *build_cpu(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *regs1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *regs2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mem = mono_view(&w->mem_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *up, *down;
    static const char *const fnames[6] = { "S", "Z", "H", "P/V", "N", "C" };
    int i;
    for (i = 0; i < NREGS; i++) {
        w->reg[i] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_max_length(w->reg[i], 6);
        gtk_editable_set_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(w->reg[i]), k_regs[i].digits + 1);
        g_object_set_data(G_OBJECT(w->reg[i]), "reg", GINT_TO_POINTER(i));
        g_signal_connect(w->reg[i], "activate", G_CALLBACK(on_reg_activate), w);
        /* PC, SP and the main set on the first row; shadows and control on the second */
        gtk_box_append(GTK_BOX(i < REGS_FIRST_ROW ? regs1 : regs2),
                       labeled(k_regs[i].name, GTK_WIDGET(w->reg[i])));
    }
    for (i = 0; i < 6; i++) {
        w->flag[i] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(fnames[i]));
        g_object_set_data(G_OBJECT(w->flag[i]), "flag", GINT_TO_POINTER(i));
        g_signal_connect(w->flag[i], "toggled", G_CALLBACK(on_flag_toggled), w);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->flag[i]));
    }
    w->beam = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->beam), "dim-label");
    gtk_label_set_xalign(w->beam, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(w->beam), TRUE);
    gtk_box_append(GTK_BOX(flags), GTK_WIDGET(w->beam));

    w->mem_goto = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_goto), 12);
    gtk_entry_set_placeholder_text(w->mem_goto, "$C000 / label");
    g_signal_connect(w->mem_goto, "activate", G_CALLBACK(on_mem_goto), w);
    w->mem_write = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(w->mem_write), 14);
    gtk_entry_set_placeholder_text(w->mem_write, "C000 41 42");
    g_signal_connect(w->mem_write, "activate", G_CALLBACK(on_mem_write), w);
    up = gtk_button_new_with_label("Page Up");
    down = gtk_button_new_with_label("Page Down");
    g_object_set_data(G_OBJECT(up), "delta", GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(down), "delta", GINT_TO_POINTER(1));
    g_signal_connect(up, "clicked", G_CALLBACK(on_mem_page), w);
    g_signal_connect(down, "clicked", G_CALLBACK(on_mem_page), w);
    gtk_box_append(GTK_BOX(edit), labeled("Go to", GTK_WIDGET(w->mem_goto)));
    gtk_box_append(GTK_BOX(edit), up);
    gtk_box_append(GTK_BOX(edit), down);
    gtk_box_append(GTK_BOX(edit), labeled("Poke (address bytes)", GTK_WIDGET(w->mem_write)));

    gtk_box_append(GTK_BOX(box), regs1);
    gtk_box_append(GTK_BOX(box), regs2);
    gtk_box_append(GTK_BOX(box), flags);
    gtk_box_append(GTK_BOX(box), edit);
    gtk_box_append(GTK_BOX(box), mem);
    accent_tag(w->mem_view, "mark");
    w->mem_mark = -1;
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&w->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *b = gtk_text_view_get_buffer(w->disasm);
    GtkWidget *hint;

    /* later tags win: the selection under the PC line, the gutter marker
     * over both */
    gtk_text_buffer_create_tag(b, "dim", "foreground", "#8a8a8a", NULL);
    gtk_text_buffer_create_tag(b, "sel", "paragraph-background", "rgba(128,128,128,0.35)", NULL);
    accent_tag(w->disasm, "pc");
    gtk_text_buffer_create_tag(b, "bp_on", "foreground", "#e01b24", NULL);
    gtk_text_buffer_create_tag(b, "bp_off", "foreground", "#8a8a8a", NULL);

    w->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(w->follow_pc, TRUE);
    g_signal_connect(w->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), w);
    w->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->jump, "$0038 / label / expression");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->jump), 22);
    g_signal_connect(w->jump, "activate", G_CALLBACK(on_jump), w);
    hint = gtk_label_new("Click selects a line; double-click, F9 or the gutter toggles its "
                         "breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(w->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), w);
    gtk_widget_add_controller(GTK_WIDGET(w->disasm), scrollc);
    /* the wheel is ours; EXTERNAL (unlike NEVER) does not ask for a window
     * tall enough to show every one of the DISASM_WINDOW lines */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    w->disasm_top = 0;
    w->selected = -1;
    return padded(box);
}

static GtkWidget *build_vdp(DbgWin *w)
{
    GtkWidget *vdp_grid = gtk_grid_new();
    GtkWidget *scroll, *outer;

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
    /* 1:1 with the rendered swatch grid so the index labels stay crisp. */
    gtk_widget_set_size_request(GTK_WIDGET(w->pic_pal), ADAMVDP_PAL_W, ADAMVDP_PAL_H);
    gtk_widget_set_halign(GTK_WIDGET(w->pic_pal), GTK_ALIGN_START);
    {
        const char *const banks[] = {"Bank 0", "Bank 1", "Bank 2", NULL};
        w->pat_bank = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(banks));
        g_signal_connect(w->pat_bank, "notify::selected", G_CALLBACK(on_pat_bank), w);
    }
    gtk_grid_attach(GTK_GRID(vdp_grid), headed("Nametable", GTK_WIDGET(w->pic_nt)), 0, 0, 1, 2);
    {
        GtkWidget *patbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_append(GTK_BOX(patbox), GTK_WIDGET(w->pat_bank));
        gtk_box_append(GTK_BOX(patbox), GTK_WIDGET(w->pic_pat));
        gtk_grid_attach(GTK_GRID(vdp_grid), headed("Patterns", patbox), 1, 0, 1, 1);
    }
    {
        GtkWidget *sprbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *si_scroll = mono_view(&w->sprite_info, FALSE);
        gtk_widget_set_size_request(si_scroll, 300, 200);
        gtk_box_append(GTK_BOX(sprbox), GTK_WIDGET(w->pic_spr));
        gtk_box_append(GTK_BOX(sprbox), si_scroll);
        gtk_grid_attach(GTK_GRID(vdp_grid), headed("Sprites", sprbox), 1, 1, 1, 1);
    }
    gtk_grid_attach(GTK_GRID(vdp_grid), headed("Palette", GTK_WIDGET(w->pic_pal)), 0, 2, 1, 1);

    /* Decoded registers + the table addresses the beam is fetching from.
     * Wrapped, so a narrow window folds the one long line (R1) instead of
     * hiding its tail behind a horizontal scrollbar. */
    scroll = mono_view(&w->vdp_state, TRUE);
    gtk_widget_set_size_request(scroll, 560, 520);
    gtk_grid_attach(GTK_GRID(vdp_grid), headed("Registers & tables", scroll), 1, 2, 1, 1);

    /* VRAM hex editor. */
    {
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *prev = gtk_button_new_with_label("\xe2\x97\x80");
        GtkWidget *next = gtk_button_new_with_label("\xe2\x96\xb6");

        w->vram_entry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(w->vram_entry, "addr [bytes...] e.g. 1800: 41 42 43");
        gtk_widget_set_hexpand(GTK_WIDGET(w->vram_entry), TRUE);
        g_signal_connect(w->vram_entry, "activate", G_CALLBACK(on_vram_entry), w);
        g_object_set_data(G_OBJECT(prev), "delta", GINT_TO_POINTER(-1));
        g_object_set_data(G_OBJECT(next), "delta", GINT_TO_POINTER(1));
        g_signal_connect(prev, "clicked", G_CALLBACK(on_vram_page), w);
        g_signal_connect(next, "clicked", G_CALLBACK(on_vram_page), w);
        w->vram_status = GTK_LABEL(gtk_label_new(""));
        gtk_label_set_xalign(w->vram_status, 0);

        gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->vram_entry));
        gtk_box_append(GTK_BOX(bar), prev);
        gtk_box_append(GTK_BOX(bar), next);
        gtk_box_append(GTK_BOX(bar), GTK_WIDGET(w->vram_status));
        gtk_box_append(GTK_BOX(vbox), bar);
        scroll = mono_view(&w->vram_view, FALSE);
        gtk_widget_set_size_request(scroll, -1, 280);
        gtk_box_append(GTK_BOX(vbox), scroll);
        gtk_grid_attach(GTK_GRID(vdp_grid),
                        headed("VRAM (Enter writes; addresses wrap at 16K)", vbox), 0, 3, 2, 1);
    }

    outer = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outer), vdp_grid);
    return outer;
}

static GtkWidget *build_breaks(DbgWin *w)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear, *hint;
    static const char *const types[] = { "Execute", "Read", "Write", "Read/Write", "Port In",
                                         "Port Out", "Port In/Out", NULL };

    w->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(w->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(w->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    w->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    w->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_start, "$C000 / port / label");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_start), 14);
    w->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(w->bp_end), 8);
    w->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(w->bp_cond, "condition, e.g. a == 5 && hl > $8000 "
                                               "\xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(w->bp_cond), TRUE);
    g_signal_connect(w->bp_start, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_end, "activate", G_CALLBACK(on_bp_add), w);
    g_signal_connect(w->bp_cond, "activate", G_CALLBACK(on_bp_add), w);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), w);
    clear = gtk_button_new_with_label("Clear All");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->bp_cond));
    gtk_box_append(GTK_BOX(row), add);
    gtk_box_append(GTK_BOX(row), clear);
    w->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->bp_msg), "adam-accent-text");
    gtk_label_set_xalign(w->bp_msg, 0);
    gtk_label_set_wrap(w->bp_msg, TRUE);
    hint = gtk_label_new("A port breakpoint takes a port number (0-255). A condition is an "
                         "expression like the console's; `value` and `addr` are the access.");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(w->bp_msg));
    gtk_box_append(GTK_BOX(box), hint);
    return padded(box);
}

static GtkWidget *build_io(DbgWin *w)
{
    return padded(mono_view(&w->io_text, FALSE));
}

/* The packets left (a note and Clear over them), the BoIP link and its
 * counts right. */
static GtkWidget *build_fujinet(DbgWin *w)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *start, *end;

    w->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(w->fuji_note), "dim-label");
    gtk_label_set_xalign(w->fuji_note, 0);
    gtk_label_set_ellipsize(w->fuji_note, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(w->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), w);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(w->fuji_note));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&w->fuji_text, FALSE));
    make_log(w->fuji_text);

    start = headed("AdamNet packets over BoIP", left);
    end = headed("BoIP link", mono_view(&w->fuji_state, FALSE));
    gtk_paned_set_start_child(GTK_PANED(paned), start);
    gtk_paned_set_end_child(GTK_PANED(paned), end);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(start, 600, -1);
    gtk_widget_set_size_request(end, 300, -1);
    w->fuji_count = -1;
    return padded(paned);
}

/* The state outlives the window: the widgets go before the window's
 * "destroy" and can still call back (the notebook's page switches), so it is
 * freed only once the window object itself is gone. */
static void on_win_finalized(gpointer data, GObject *where)
{
    DbgWin *w = data;
    (void)where;
    if (w->history) g_ptr_array_unref(w->history);
    if (w->bp_sig) g_string_free(w->bp_sig, TRUE);
    g_free(w);
}

static void on_destroy(GtkWidget *widget, gpointer ud)
{
    DbgWin *w = ud;
    (void)widget;
    w->closing = TRUE;
    if (w->timer) g_source_remove(w->timer);
    w->timer = 0;
    adamdebug_detach(w->dbg);
    if (g_win == w) g_win = NULL;
}

static void attach_and_refresh(DbgWin *w)
{
    adamdebug_attach(w->dbg);    /* stops the machine, within a frame */
    settle(w, TRUE);
    w->running_ticks = 0;
    if (w->bp_sig) { g_string_free(w->bp_sig, TRUE); w->bp_sig = NULL; }
    refresh_now(w);
}

/* ADAM_DEBUGGER_TAB: a page number (0-6), or a tab's name (or the start of one). */
static int tab_from_env(void)
{
    const char *tab = g_getenv("ADAM_DEBUGGER_TAB");
    char *end;
    long n;
    unsigned i;
    if (!tab || !*tab) return -1;
    n = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (n >= 0 && n < (long)G_N_ELEMENTS(k_tab_names)) ? (int)n : -1;
    for (i = 0; i < G_N_ELEMENTS(k_tab_names); i++)
        if (g_ascii_strncasecmp(tab, k_tab_names[i], strlen(tab)) == 0)
            return (int)i;
    return -1;
}

void adam_debugger_show(GtkWindow *parent, adamsession *session)
{
    DbgWin *w;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    int tab;

    if (g_win) {
        gboolean shown = gtk_widget_get_visible(GTK_WIDGET(g_win->win));
        gtk_window_present(g_win->win);
        if (!shown) attach_and_refresh(g_win);
        return;
    }
    w = g_new0(DbgWin, 1);
    w->session = session;
    w->dbg = adamsession_debugger(session);
    w->mem_mark = -1;
    g_win = w;

    w->win = GTK_WINDOW(adw_window_new());
    g_object_weak_ref(G_OBJECT(w->win), on_win_finalized, w);
    gtk_window_set_title(w->win, "Debugger");
    gtk_window_set_icon_name(w->win, adam_icon_name());
    gtk_window_set_default_size(w->win, 1150, 800);
    gtk_window_set_transient_for(w->win, parent);
    gtk_window_set_destroy_with_parent(w->win, TRUE);
    g_signal_connect(w->win, "close-request", G_CALLBACK(on_close), w);
    g_signal_connect(w->win, "destroy", G_CALLBACK(on_destroy), w);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(w));
    notebook = gtk_notebook_new();
    w->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_notebook_append_page(w->notebook, build_console(w), gtk_label_new(k_tab_names[TAB_CONSOLE]));
    gtk_notebook_append_page(w->notebook, build_cpu(w), gtk_label_new(k_tab_names[TAB_CPU]));
    gtk_notebook_append_page(w->notebook, build_disasm(w), gtk_label_new(k_tab_names[TAB_DISASM]));
    gtk_notebook_append_page(w->notebook, build_vdp(w), gtk_label_new(k_tab_names[TAB_VDP]));
    gtk_notebook_append_page(w->notebook, build_breaks(w), gtk_label_new(k_tab_names[TAB_BREAKS]));
    gtk_notebook_append_page(w->notebook, build_io(w), gtk_label_new(k_tab_names[TAB_IO]));
    gtk_notebook_append_page(w->notebook, build_fujinet(w), gtk_label_new(k_tab_names[TAB_FUJINET]));
    gtk_box_append(GTK_BOX(root), notebook);
    tab = tab_from_env();
    if (tab >= 0) gtk_notebook_set_current_page(w->notebook, tab);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), w);

    overlay = adw_toast_overlay_new();
    w->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(w->toasts, root);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    w->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(w->title));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
    adw_window_set_content(ADW_WINDOW(w->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), w);
    gtk_widget_add_controller(GTK_WIDGET(w->win), keys);

    attach_and_refresh(w);
    w->timer = g_timeout_add(100, tick_refresh, w);
    gtk_window_present(w->win);
    if (gtk_notebook_get_current_page(w->notebook) == TAB_CONSOLE)
        gtk_widget_grab_focus(GTK_WIDGET(w->console_in));
}

/* F12 and the menu item: show it, or hide it if it is showing. */
void adam_debugger_toggle(GtkWindow *parent, adamsession *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        adam_debugger_show(parent, session);
}
