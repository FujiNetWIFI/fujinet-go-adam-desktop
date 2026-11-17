/*
 * The Controllers window -- both ADAM hand controllers drawn as a player sees
 * them: a cream case with the stick's disc ringed by eight direction wedges,
 * a fire button down either side, and the twelve-key keypad on its silver
 * overlay-card frame. Everything is drawn from the session's layout
 * (adamsession_controls_layout), so every frontend shows the same controllers.
 * Under them, chosen by the Keyboard check, is the ADAM's own 75-key keyboard
 * (adamsession_keyboard_layout): a click on a cap types that key, SHIFT and
 * CONTROL latch until the next key and LOCK toggles.
 *
 * Mouse: a press on a key or a fire button holds it, a press on the stick's
 * disc or one of its wedges tilts the stick toward the pointer and a drag
 * follows (adamsession_stick_from_vector -> adamsession_pointer_set).
 * Every press is the raw press and release (a GtkGestureDrag, not a click: a
 * click gives up when the pointer wobbles), because the console samples its
 * controllers once per frame -- a press present only for the instant of a
 * click falls between frames. One press holds one control: chords come from
 * the keyboard or a gamepad. Losing the focus, hiding and the pointer leaving
 * a held key let go of whatever the mouse holds here.
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (adamsession_buttons_held) -- is polled every frame and painted in the
 * accent, so the window doubles as a gamepad tester.
 *
 * Keys typed with the window active go through the main window
 * (adam_window_key), so the machine, F9 and the shortcuts behave the same
 * whichever window has the focus.
 *
 * FIXED SIZE, FLOATING: the console is a drawing area of one size, the labels
 * ellipsize rather than grow, and the window is not resizable, so neither Map
 * mode nor a long gamepad name changes its size and tiling compositors float
 * it. It is hidden at startup (F9 or the menu shows it).
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 * Its timer runs only while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a control to pick
 * the target, then press a keyboard key OR a gamepad button. While armed the
 * same gestures pick the target instead of pressing, every control shows the
 * key bound to it, and typed keys are not typed into the console; once a
 * target is picked the other controls are greyed. Rebinding steals the key
 * from whatever held it. Escape cancels. Clear takes the picked target's key
 * and button away, Defaults puts every binding back.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../help/help_window.h"
#include "../window.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define UNIT 40.0               /* pixels per layout unit (a keypad key's width) */
#define CASE_PAD 18             /* the case around everything */
#define BRAND_H 22              /* the brand strip along the top */
#define TOP_GAP 12
#define STRIP_BW 214
#define STRIP_GAP 12
#define STRIP_H 34
#define MAX_PARTS 64
#define STRIP_ITEMS 2
#define KEY_UNIT 27.0           /* pixels per keyboard key unit */
#define KEY_PAD 14              /* the cream case around the keyboard */
#define LOGO_H 16               /* the dark ADAM logo bar over the SmartKeys */
#define DIR_BITS ((1u << ADAM_ACT_UP) | (1u << ADAM_ACT_DOWN) | \
                  (1u << ADAM_ACT_LEFT) | (1u << ADAM_ACT_RIGHT))

typedef struct {
    double xpos, ypos, width, height;
    int target;
    const char *label;
} StripItem;

typedef struct {
    GtkWidget *area;
    StripItem items[STRIP_ITEMS];
    int count, pressed, hover;
} Strip;

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the AdamWindow: typed keys go through it */
static adamsession *g_session;
static GtkWidget *g_console;    /* the controllers' drawing area */
static Strip g_strip;
static GtkWidget *g_keys_area;          /* the ADAM keyboard's drawing area */
static GtkWidget *g_keyboard_check;
static GtkWidget *g_root;

static const adam_control *g_layout;  /* adamsession_controls_layout: static */
static int g_part_count;
static float g_layout_wid, g_layout_hgt;
static gboolean g_lit[MAX_PARTS];       /* what the console sees held */
static const adam_keycap *g_caps;       /* adamsession_keyboard_layout: static */
static int g_cap_count;
static float g_cap_wide, g_cap_high;
static unsigned char g_cap_bits[ADAM_KBD_CAPS_MAX];  /* bit 0 flash, bit 1 latched */
static int g_press_cap = -1;            /* the cap the mouse holds */
static int g_hover = -1;                /* the part under the pointer */
static int g_pressed = -1;              /* a key or fire button the mouse holds */

/* The stick the mouse is dragging. */
static struct {
    gboolean active;
    int port;                   /* 0 controller 1, 1 controller 2 */
    double cenx, ceny, rad;     /* the stick disc, pixels */
} g_drag;

/* each stick cap as it is painted: eased toward what is held */
static struct {
    double tiltx, tilty;        /* -1..1 (+y toward the player) */
} g_anim[ADAM_PORTS];

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button;
static GtkWidget *g_map_hint;
static GtkWidget *g_show_bindings;
#define MAX_PADS    8           /* pads listed (SDL opens few more) */
#define PAD_ROWS    2           /* pad rows the window reserves; more scroll */
#define PAD_ROW_H   36          /* one row's height, px */
#define PAD_GAP     4
static GtkWidget *g_pad_box;
static GtkWidget *g_pad_eff[MAX_PADS];  /* each pad's "drives controller N" */
static int g_pad_row_count;
static unsigned g_pad_generation = (unsigned)-1;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_info_timer;

static void refresh_all(void);
static void cap_release(void);

/* "controllers_show_bindings"; the older spellings are read when it is absent. */
static gboolean show_bindings_saved(void)
{
    static const char *const older[] = { "controllers_bindings", "controllers_key_names",
                                         "show_bindings" };
    int val = adamsession_get_int(g_session, "controllers_show_bindings", -1);
    size_t idx;
    for (idx = 0; val < 0 && idx < G_N_ELEMENTS(older); idx++)
        val = adamsession_get_int(g_session, older[idx], -1);
    return val > 0;
}

/* ---- small helpers ---------------------------------------------------------- */

static void rgb(cairo_t *cctx, guint32 col)
{
    cairo_set_source_rgb(cctx, ((col >> 16) & 0xFF) / 255.0, ((col >> 8) & 0xFF) / 255.0,
                         (col & 0xFF) / 255.0);
}

static guint32 mix(guint32 from, guint32 dest, double frac)
{
    guint32 out = 0;
    int shift;
    for (shift = 16; shift >= 0; shift -= 8) {
        double chan_from = (from >> shift) & 0xFF, chan_dest = (dest >> shift) & 0xFF;
        out |= (guint32)(chan_from + (chan_dest - chan_from) * frac + 0.5) << shift;
    }
    return out;
}

static void rrect(cairo_t *cctx, double xpos, double ypos, double wid, double hgt, double rad)
{
    if (rad > wid / 2) rad = wid / 2;
    if (rad > hgt / 2) rad = hgt / 2;
    cairo_new_sub_path(cctx);
    cairo_arc(cctx, xpos + wid - rad, ypos + rad, rad, -G_PI / 2, 0);
    cairo_arc(cctx, xpos + wid - rad, ypos + hgt - rad, rad, 0, G_PI / 2);
    cairo_arc(cctx, xpos + rad, ypos + hgt - rad, rad, G_PI / 2, G_PI);
    cairo_arc(cctx, xpos + rad, ypos + rad, rad, G_PI, 3 * G_PI / 2);
    cairo_close_path(cctx);
}

static void stop_rgb(cairo_pattern_t *pat, double offset, guint32 col)
{
    cairo_pattern_add_color_stop_rgb(pat, offset, ((col >> 16) & 0xFF) / 255.0,
                                     ((col >> 8) & 0xFF) / 255.0, (col & 0xFF) / 255.0);
}

/* Text centred in a box, in an absolute pixel size, ellipsized to the box. */
static void legend(cairo_t *cctx, GtkWidget *widget, const char *text, double pxsize,
                   gboolean bold, guint32 colour, double xpos, double ypos, double boxw,
                   double boxh)
{
    PangoLayout *lay = gtk_widget_create_pango_layout(widget, text);
    PangoFontDescription *fdesc = pango_font_description_new();
    int textw, texth;

    pango_font_description_set_family(fdesc, "Sans");
    pango_font_description_set_weight(fdesc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(fdesc, pxsize * PANGO_SCALE);
    pango_layout_set_font_description(lay, fdesc);
    pango_layout_set_width(lay, (int)(boxw * PANGO_SCALE));
    pango_layout_set_ellipsize(lay, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(lay, PANGO_ALIGN_CENTER);
    pango_layout_get_pixel_size(lay, &textw, &texth);
    rgb(cctx, colour);
    cairo_move_to(cctx, xpos, ypos + (boxh - texth) / 2.0);
    pango_cairo_show_layout(cctx, lay);
    /* the move_to above is still the current point, and the next arc would
     * join it with a line */
    cairo_new_path(cctx);
    pango_font_description_free(fdesc);
    g_object_unref(lay);
}

/* A host key's name as a small legend: the long ones shortened to fit. */
static void legend_name(guint32 keysym, char *dst, int dstsz)
{
    char name[32];
    adamsession_keysym_name(keysym, name, sizeof name);
    if (!strcmp(name, "Keypad Enter"))
        g_snprintf(dst, dstsz, "KP Ent");     /* fits a fire button */
    else if (g_str_has_prefix(name, "Keypad "))
        g_snprintf(dst, dstsz, "KP %s", name + 7);
    else if (g_str_has_prefix(name, "Left "))
        g_snprintf(dst, dstsz, "L %s", name + 5);
    else if (g_str_has_prefix(name, "Right "))
        g_snprintf(dst, dstsz, "R %s", name + 6);
    else if (!strcmp(name, "Backspace"))
        g_snprintf(dst, dstsz, "BkSp");
    else if (!strcmp(name, "Escape"))
        g_snprintf(dst, dstsz, "Esc");
    else
        g_snprintf(dst, dstsz, "%s", name);
}

static gboolean is_sysact(int target)
{
    return target >= ADAM_TARGET_SYSACT(0);
}

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings));
}

/* The binding legend for a target, or "" when none or not shown. */
static void bound_text(int target, char *dst, int dstsz)
{
    adam_binding bind;
    dst[0] = '\0';
    if (target < 0 || !showing_bindings()) return;
    bind = adamsession_binding_get(g_session, target);
    if (bind.keysym)
        legend_name(bind.keysym, dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

/* ---- geometry ---------------------------------------------------------------- */

#define ORG_X (CASE_PAD)
#define ORG_Y (CASE_PAD + BRAND_H + TOP_GAP)

static double pix_x(double unitx) { return ORG_X + unitx * UNIT; }
static double pix_y(double unity) { return ORG_Y + unity * UNIT; }

static int console_width(void)
{
    return (int)(2 * CASE_PAD + g_layout_wid * UNIT + 0.5);
}

static int console_height(void)
{
    return (int)(ORG_Y + g_layout_hgt * UNIT + CASE_PAD + 0.5);
}

/* The stick disc (centre, radius in pixels) of a controller, or FALSE. */
static gboolean stick_of(int port, double *cenx, double *ceny, double *rad)
{
    int idx;
    for (idx = 0; idx < g_part_count; idx++)
        if (g_layout[idx].kind == ADAM_CTL_STICK && g_layout[idx].port == port) {
            *cenx = pix_x(g_layout[idx].xpos + g_layout[idx].width / 2);
            *ceny = pix_y(g_layout[idx].ypos + g_layout[idx].height / 2);
            *rad = g_layout[idx].width * UNIT / 2;
            return TRUE;
        }
    return FALSE;
}

/* ---- what is held --------------------------------------------------------------- */

static gboolean part_held(const adam_control *ctl, const unsigned held[ADAM_PORTS])
{
    switch (ctl->kind) {
    case ADAM_CTL_KEY:
    case ADAM_CTL_FIRE:
        return (held[ctl->port] & ctl->act_mask) != 0;
    case ADAM_CTL_DIRECTION:
        /* the wedge that is exactly the direction held: a diagonal lights its
         * own wedge, not the two beside it */
        return ctl->act_mask && (held[ctl->port] & DIR_BITS) == ctl->act_mask;
    default:
        return FALSE;
    }
}

/* The stick cap's eased tilt toward what is held. Returns whether it moved. */
static gboolean ease_cap(int port, unsigned held)
{
    double tgtx = (int)((held >> ADAM_ACT_RIGHT) & 1) - (int)((held >> ADAM_ACT_LEFT) & 1);
    double tgty = (int)((held >> ADAM_ACT_DOWN) & 1) - (int)((held >> ADAM_ACT_UP) & 1);
    double *cur[2] = { &g_anim[port].tiltx, &g_anim[port].tilty };
    double tgt[2];
    gboolean moved = FALSE;
    int idx;

    if (tgtx != 0 && tgty != 0) { tgtx *= 0.7071; tgty *= 0.7071; }
    tgt[0] = tgtx; tgt[1] = tgty;
    for (idx = 0; idx < 2; idx++) {
        const double dist = tgt[idx] - *cur[idx];
        if (fabs(dist) < 0.004) {
            if (dist != 0) { *cur[idx] = tgt[idx]; moved = TRUE; }
            continue;
        }
        *cur[idx] += dist * 0.4;
        moved = TRUE;
    }
    return moved;
}

/* ---- drawing ------------------------------------------------------------------- */

/* ADAM cream (DESIGN-adam.md): the drawing is the family's, only the plastic
 * is the ADAM's. The outer case is the keyboard tray's darker cream, the
 * controllers' bodies the cream itself, the keys warm grey. */
#define COL_CASE_TOP 0xCFC6AE
#define COL_CASE_BOT 0xBDB398
#define COL_CASE_EDGE 0x8A7B5C
#define COL_CASE_LIGHT 0xE8DFC6
#define COL_BRAND_BAR 0x1E1B1A
#define COL_BODY_TOP 0xE8DFC6
#define COL_BODY_BOT 0xD9CFB2
#define COL_BODY_EDGE 0x8A7B5C
#define COL_BODY_LIGHT 0xF6F0DE
#define COL_PART 0xCFC6AE
#define COL_PART_HOT 0xDDD5BE
#define COL_KEY 0xB3AA92
#define COL_KEY_HOT 0xC4BBA3
#define COL_KEY_SIDE 0x8A8068
#define COL_KEY_INK 0x4A4438
#define COL_FIRE 0xD9461E
#define COL_FIRE_SIDE 0x7C240F
#define COL_PLATE_OUT 0x8A7B5C
#define COL_PLATE_IN 0xB9AF94
#define COL_OVERLAY_TOP 0xB9BBBE
#define COL_OVERLAY_BOT 0x8E9093
#define COL_SLOT 0x0B0B0C
#define COL_SLOT_LIP 0x4A4A4E
#define COL_STRIP_CAP 0xCFC6AE
#define COL_STRIP_HOT 0xDDD5BE
#define COL_STRIP_SIDE 0x8A7B5C
#define COL_INK 0x4A4438
#define COL_DIM_INK 0x8A7B5C
#define COL_CAP_HI 0xDDD5BE
#define COL_CAP_LO 0xB7AD92
#define COL_CAP_EDGE 0x8A7B5C
#define COL_ACCENT ADAMSESSION_ACCENT_RGB
#define COL_HELD_INK 0xFFFFFF

typedef struct {
    gboolean held, selected, dim, hot;
} PartState;

static PartState part_state(int idx)
{
    const adam_control *ctl = &g_layout[idx];
    PartState pst;
    const gboolean armed = g_map_state != -2;
    pst.held = g_lit[idx];
    pst.selected = g_map_state >= 0 && ctl->target >= 0 && g_map_state == ctl->target;
    pst.dim = armed && (ctl->target < 0 || (g_map_state >= 0 && !pst.selected));
    pst.hot = g_hover == idx && !pst.held;
    return pst;
}

/* Everything a part draws between push and pop is dimmed to 40% in Map mode
 * when it is not the picked target. */
static void dim_begin(cairo_t *cctx, const PartState *pst)
{
    if (pst->dim) cairo_push_group(cctx);
}

static void dim_end(cairo_t *cctx, const PartState *pst)
{
    if (pst->dim) {
        cairo_pop_group_to_source(cctx);
        cairo_paint_with_alpha(cctx, 0.4);
    }
}

/* One controller's cream case, from its caption down, with the caption on it. */
static void draw_body(cairo_t *cctx, GtkWidget *widget, const adam_control *cap)
{
    const double xpos = pix_x(cap->xpos) - 6, ypos = pix_y(cap->ypos) - 6;
    const double wid = cap->width * UNIT + 12, hgt = pix_y(g_layout_hgt) - ypos;
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, ypos, 0, ypos + hgt);

    stop_rgb(grad, 0, COL_BODY_TOP);
    stop_rgb(grad, 1, COL_BODY_BOT);
    rrect(cctx, xpos, ypos, wid, hgt, 14);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_BODY_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    cairo_move_to(cctx, xpos + 14, ypos + 1.5);
    cairo_line_to(cctx, xpos + wid - 14, ypos + 1.5);
    rgb(cctx, COL_BODY_LIGHT);
    cairo_stroke(cctx);
    legend(cctx, widget, cap->label, 12, TRUE, COL_INK, pix_x(cap->xpos), pix_y(cap->ypos),
           cap->width * UNIT, cap->height * UNIT);
}

/* The overlay-card frame behind the keypad: a silver faceplate with the card
 * slot across its top. Inert. */
static void draw_overlay(cairo_t *cctx, const adam_control *ctl)
{
    const double xpos = pix_x(ctl->xpos), ypos = pix_y(ctl->ypos);
    const double wid = ctl->width * UNIT, hgt = ctl->height * UNIT;
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, ypos, 0, ypos + hgt);

    stop_rgb(grad, 0, COL_OVERLAY_TOP);
    stop_rgb(grad, 1, COL_OVERLAY_BOT);
    rrect(cctx, xpos, ypos, wid, hgt, 10);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, mix(COL_OVERLAY_BOT, 0x000000, 0.5));
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    rrect(cctx, xpos + 14, ypos + 6, wid - 28, 3, 1.5);
    rgb(cctx, COL_SLOT);
    cairo_fill(cctx);
    rrect(cctx, xpos + 14, ypos + 9, wid - 28, 1.5, 0.75);
    rgb(cctx, COL_SLOT_LIP);
    cairo_fill(cctx);
}

/* A keypad key: a rubber cap with its legend, the bound key under it
 * when bindings are shown. */
static void draw_key(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const adam_control *ctl = &g_layout[idx];
    const PartState pst = part_state(idx);
    const double xpos = pix_x(ctl->xpos), ypos = pix_y(ctl->ypos);
    const double wid = ctl->width * UNIT, hgt = ctl->height * UNIT - 3;
    const gboolean lit = pst.held || pst.selected;
    guint32 face = pst.hot ? COL_KEY_HOT : COL_KEY;
    guint32 ink = lit ? COL_HELD_INK : COL_KEY_INK;
    char bound[40];
    double lhgt = hgt;

    if (lit) face = COL_ACCENT;
    dim_begin(cctx, &pst);
    rrect(cctx, xpos, ypos + 3, wid, hgt, 4);
    rgb(cctx, lit ? mix(COL_ACCENT, 0x000000, 0.45) : COL_KEY_SIDE);
    cairo_fill(cctx);
    rrect(cctx, xpos, ypos, wid, hgt, 4);
    rgb(cctx, face);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD_INK : mix(face, 0x000000, 0.5));
    cairo_set_line_width(cctx, lit ? 2 : 1);
    cairo_stroke(cctx);

    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0]) {
        lhgt = hgt * 0.62;
        legend(cctx, widget, bound, 9, FALSE, mix(ink, face, 0.3), xpos, ypos + lhgt - 2, wid,
               hgt - lhgt);
    }
    legend(cctx, widget, ctl->label, 15, TRUE, ink, xpos, ypos, wid, lhgt);
    dim_end(cctx, &pst);
}

/* A fire button down the side of the head: a tall orange-red cap with its
 * lower edge, blank like the real ones; the bound key in white when shown. */
static void draw_fire(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const adam_control *ctl = &g_layout[idx];
    const PartState pst = part_state(idx);
    const double xpos = pix_x(ctl->xpos), ypos = pix_y(ctl->ypos);
    const double wid = ctl->width * UNIT, hgt = ctl->height * UNIT - 3;
    const gboolean lit = pst.held || pst.selected;
    guint32 face = lit ? COL_ACCENT : pst.hot ? mix(COL_FIRE, 0xFFFFFF, 0.16) : COL_FIRE;
    char bound[40];

    dim_begin(cctx, &pst);
    rrect(cctx, xpos, ypos + 3, wid, hgt, 10);
    rgb(cctx, lit ? mix(COL_ACCENT, 0x000000, 0.45) : COL_FIRE_SIDE);
    cairo_fill(cctx);
    rrect(cctx, xpos, ypos, wid, hgt, 10);
    rgb(cctx, face);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD_INK : mix(face, 0x000000, 0.5));
    cairo_set_line_width(cctx, lit ? 2 : 1);
    cairo_stroke(cctx);

    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0])
        legend(cctx, widget, bound, 9, TRUE, COL_HELD_INK, xpos - 6, ypos, wid + 12, hgt);
    dim_end(cctx, &pst);
}

/* The screen angle of a wedge from its direction bits (y down). */
static double wedge_angle(unsigned acts)
{
    const double delx = (int)((acts >> ADAM_ACT_RIGHT) & 1) - (int)((acts >> ADAM_ACT_LEFT) & 1);
    const double dely = (int)((acts >> ADAM_ACT_DOWN) & 1) - (int)((acts >> ADAM_ACT_UP) & 1);
    return atan2(dely, delx);
}

/* One of the eight wedges: the sector of the stick disc between half its
 * radius and all of it. */
static void draw_wedge(cairo_t *cctx, GtkWidget *widget, int idx, double cenx, double ceny,
                       double rad)
{
    const adam_control *ctl = &g_layout[idx];
    const PartState pst = part_state(idx);
    const double ang = wedge_angle(ctl->act_mask), span = 21.0 * G_PI / 180.0;
    const double rad0 = rad * 0.5 + 2, rad1 = rad - 1;
    const double mid = (rad0 + rad1) / 2;
    const double glyx = cenx + cos(ang) * mid, glyy = ceny + sin(ang) * mid;
    const gboolean lit = pst.held || pst.selected;
    guint32 face = lit ? COL_ACCENT : pst.hot ? COL_PART_HOT : COL_PART;
    guint32 ink = lit ? COL_HELD_INK : COL_DIM_INK;
    char bound[40];

    dim_begin(cctx, &pst);
    cairo_arc(cctx, cenx, ceny, rad1, ang - span, ang + span);
    cairo_arc_negative(cctx, cenx, ceny, rad0, ang + span, ang - span);
    cairo_close_path(cctx);
    rgb(cctx, face);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD_INK : mix(face, 0x000000, 0.5));
    cairo_set_line_width(cctx, lit ? 2 : 1);
    cairo_stroke(cctx);

    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0]) {
        legend(cctx, widget, bound, 10, TRUE, lit ? COL_HELD_INK : COL_INK, glyx - 22,
               glyy - 9, 44, 18);
    } else {
        /* an arrow pointing the way the stick tilts */
        const double unitx = cos(ang), unity = sin(ang);
        cairo_move_to(cctx, glyx + unitx * 8, glyy + unity * 8);
        cairo_line_to(cctx, glyx - unitx * 5 - unity * 7, glyy - unity * 5 + unitx * 7);
        cairo_line_to(cctx, glyx - unitx * 5 + unity * 7, glyy - unity * 5 - unitx * 7);
        cairo_close_path(cctx);
        rgb(cctx, ink);
        cairo_fill(cctx);
    }
    dim_end(cctx, &pst);
}

/* The disc's recessed plate, under the wedges. */
static void draw_plate(cairo_t *cctx, double cenx, double ceny, double rad)
{
    cairo_arc(cctx, cenx, ceny, rad + 5, 0, 2 * G_PI);
    rgb(cctx, COL_PLATE_OUT);
    cairo_fill(cctx);
    cairo_arc(cctx, cenx, ceny, rad + 1, 0, 2 * G_PI);
    rgb(cctx, COL_PLATE_IN);
    cairo_fill(cctx);
}

/* The stick's cap, over the wedges: a dark disc with a light crescent, shifted
 * toward the tilt with its shadow lengthening. */
static void draw_cap(cairo_t *cctx, int port, double cenx, double ceny, double rad)
{
    const double capr = rad * 0.5 - 5;
    const double shift = rad * 0.14;
    const double capx = cenx + g_anim[port].tiltx * shift;
    const double capy = ceny + g_anim[port].tilty * shift;
    const double lean = fabs(g_anim[port].tiltx) + fabs(g_anim[port].tilty);
    cairo_pattern_t *grad;

    cairo_arc(cctx, capx + 2 + 2 * g_anim[port].tiltx * lean,
              capy + 4 + 2 * g_anim[port].tilty * lean, capr, 0, 2 * G_PI);
    cairo_set_source_rgba(cctx, 0, 0, 0, 0.55);
    cairo_fill(cctx);

    grad = cairo_pattern_create_radial(capx - capr * 0.3, capy - capr * 0.3, capr * 0.1,
                                       capx, capy, capr);
    stop_rgb(grad, 0, COL_CAP_HI);
    stop_rgb(grad, 1, COL_CAP_LO);
    cairo_arc(cctx, capx, capy, capr, 0, 2 * G_PI);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_CAP_EDGE);
    cairo_set_line_width(cctx, 2);
    cairo_stroke(cctx);

    cairo_arc(cctx, capx, capy, capr - 4, G_PI * 1.1, G_PI * 1.6);
    cairo_set_source_rgba(cctx, 1, 1, 1, 0.55);
    cairo_set_line_width(cctx, 2);
    cairo_set_line_cap(cctx, CAIRO_LINE_CAP_ROUND);
    cairo_stroke(cctx);
}

static void draw_console(GtkDrawingArea *area, cairo_t *cctx, int wid, int hgt, gpointer udata)
{
    GtkWidget *widget = GTK_WIDGET(area);
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, hgt);
    int idx;
    (void)udata;

    /* the case */
    stop_rgb(grad, 0, COL_CASE_TOP);
    stop_rgb(grad, 1, COL_CASE_BOT);
    rrect(cctx, 0.5, 0.5, wid - 1, hgt - 1, 16);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    rgb(cctx, COL_CASE_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    cairo_pattern_destroy(grad);
    cairo_move_to(cctx, 18, 2.5);
    cairo_line_to(cctx, wid - 18, 2.5);
    rgb(cctx, COL_CASE_LIGHT);
    cairo_set_line_width(cctx, 1.5);
    cairo_stroke(cctx);

    /* the brand strip: a dark bar, the accent under it, the name in a tint */
    rrect(cctx, CASE_PAD, CASE_PAD, wid - 2 * CASE_PAD, BRAND_H, 4);
    rgb(cctx, COL_BRAND_BAR);
    cairo_fill(cctx);
    cairo_rectangle(cctx, CASE_PAD + 3, CASE_PAD + BRAND_H - 2, wid - 2 * CASE_PAD - 6, 2);
    rgb(cctx, COL_ACCENT);
    cairo_fill(cctx);
    legend(cctx, widget, "ADAM", 12, TRUE, mix(COL_ACCENT, 0xFFFFFF, 0.35),
           wid / 2.0 - 80, CASE_PAD, 160, BRAND_H - 2);

    if (!g_layout)
        return;

    /* the parts in the layout's paint order (cases, plates, wedges, buttons) */
    for (idx = 0; idx < g_part_count; idx++) {
        const adam_control *ctl = &g_layout[idx];
        double cenx, ceny, rad;
        switch (ctl->kind) {
        case ADAM_CTL_CAPTION:
            draw_body(cctx, widget, ctl);
            break;
        case ADAM_CTL_OVERLAY:
            draw_overlay(cctx, ctl);
            break;
        case ADAM_CTL_STICK:
            if (stick_of(ctl->port, &cenx, &ceny, &rad))
                draw_plate(cctx, cenx, ceny, rad);
            break;
        case ADAM_CTL_DIRECTION:
            if (stick_of(ctl->port, &cenx, &ceny, &rad))
                draw_wedge(cctx, widget, idx, cenx, ceny, rad);
            break;
        case ADAM_CTL_FIRE:
            draw_fire(cctx, widget, idx);
            break;
        case ADAM_CTL_KEY:
            draw_key(cctx, widget, idx);
            break;
        default:
            break;
        }
    }
    /* the caps last, over the wedges */
    for (idx = 0; idx < g_part_count; idx++) {
        double cenx, ceny, rad;
        if (g_layout[idx].kind == ADAM_CTL_STICK &&
            stick_of(g_layout[idx].port, &cenx, &ceny, &rad)) {
            const gboolean armed = g_map_state != -2;
            if (armed) cairo_push_group(cctx);
            draw_cap(cctx, g_layout[idx].port, cenx, ceny, rad);
            if (armed) {
                cairo_pop_group_to_source(cctx);
                cairo_paint_with_alpha(cctx, 0.4);
            }
        }
    }
}

/* The two buttons under the case. */
static void draw_strip(GtkDrawingArea *area, cairo_t *cctx, int wid, int hgt, gpointer udata)
{
    Strip *strip = udata;
    int idx;
    (void)wid; (void)hgt;

    for (idx = 0; idx < strip->count; idx++) {
        const StripItem *item = &strip->items[idx];
        const gboolean held = strip->pressed == idx;
        const gboolean sel = g_map_state >= 0 && g_map_state == item->target;
        const gboolean dim = g_map_state >= 0 && !sel;
        guint32 face = held || sel ? COL_ACCENT : strip->hover == idx ? COL_STRIP_HOT : COL_STRIP_CAP;
        char bound[40], text[120];

        if (dim) cairo_push_group(cctx);
        rrect(cctx, item->xpos, item->ypos + 3, item->width, item->height - 3, 7);
        rgb(cctx, held || sel ? mix(COL_ACCENT, 0x000000, 0.45) : COL_STRIP_SIDE);
        cairo_fill(cctx);
        rrect(cctx, item->xpos, item->ypos, item->width, item->height - 3, 7);
        rgb(cctx, face);
        cairo_fill(cctx);
        bound_text(item->target, bound, sizeof bound);
        if (bound[0])
            g_snprintf(text, sizeof text, "%s  \xc2\xb7  %s", item->label, bound);
        else
            g_snprintf(text, sizeof text, "%s", item->label);
        legend(cctx, GTK_WIDGET(area), text, 12, FALSE, held || sel ? COL_HELD_INK : COL_INK,
               item->xpos + 4, item->ypos, item->width - 8, item->height - 3);
        if (dim) {
            cairo_pop_group_to_source(cctx);
            cairo_paint_with_alpha(cctx, 0.4);
        }
    }
}

/* ---- pointer ---------------------------------------------------------------------- */

static int console_hit(double xpos, double ypos)
{
    return adamsession_controls_hit((float)((xpos - ORG_X) / UNIT),
                                      (float)((ypos - ORG_Y) / UNIT));
}

static void run_sysaction(int sysact)
{
    if (g_parent)
        adam_window_run_sysaction(ADAM_WINDOW(g_parent), sysact);
    else
        adamsession_sysaction(g_session, sysact);
}

/* Lets go of everything the mouse holds on the controllers. */
static void mouse_release(void)
{
    if (g_drag.active) {
        adamsession_pointer_set(g_session, g_drag.port, 0);
        g_drag.active = FALSE;
    }
    if (g_pressed >= 0) {
        const int port = g_layout[g_pressed].port;
        g_pressed = -1;
        adamsession_pointer_set(g_session, port, 0);
    }
    if (g_strip.pressed >= 0) {
        g_strip.pressed = -1;
        if (g_strip.area) gtk_widget_queue_draw(g_strip.area);
    }
    cap_release();
    if (g_console) gtk_widget_queue_draw(g_console);
    if (g_keys_area) gtk_widget_queue_draw(g_keys_area);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    adamsession_gamepad_capture_cancel(g_session);
}

static void say_bound(const char *what, const char *stolen)
{
    char msg[220];
    if (stolen[0])
        g_snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else
        g_snprintf(msg, sizeof msg, "Bound %s", what);
    gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
}

static void set_map_state(int state);

static gboolean capture_tick(gpointer udata)
{
    int button;
    (void)udata;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (adamsession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], name[40];
        adamsession_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        /* the name is in a static buffer for raw buttons and hats: copy it */
        g_snprintf(name, sizeof name, "%s", adam_pad_button_name(button));
        g_capture_timer = 0;
        set_map_state(-1);     /* stay armed: remapping several in a row is normal */
        say_bound(name, stolen);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    gtk_widget_set_sensitive(g_clear_button, state >= 0);
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "adam-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Hold a control with the mouse. Map rebinds one.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "adam-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a control to remap it");
    } else {
        char msg[160];
        mouse_release();
        g_snprintf(msg, sizeof msg, "Press a key or a gamepad button for %s (Esc cancels)",
                   adam_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        adamsession_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_all();
}

/* The stick under a drag: the pointer's vector from the disc's centre. */
static void drag_follow(double xpos, double ypos)
{
    const unsigned stick = adamsession_stick_from_vector((float)(xpos - g_drag.cenx),
                                                           (float)(ypos - g_drag.ceny),
                                                           (float)g_drag.rad);
    adamsession_pointer_set(g_session, g_drag.port, stick);
    gtk_widget_queue_draw(g_console);
}

static void on_console_begin(GtkGestureDrag *gest, double xpos, double ypos, gpointer udata)
{
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gest));
    const int idx = console_hit(xpos, ypos);
    const adam_control *ctl;
    (void)udata;

    if (idx < 0 || button != GDK_BUTTON_PRIMARY)
        return;
    ctl = &g_layout[idx];
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target; the stick's disc and the
         * diagonal wedges are no single control */
        if (ctl->target >= 0)
            set_map_state(ctl->target);
        return;
    }
    mouse_release();
    switch (ctl->kind) {
    case ADAM_CTL_STICK:
    case ADAM_CTL_DIRECTION:
        if (!stick_of(ctl->port, &g_drag.cenx, &g_drag.ceny, &g_drag.rad))
            return;
        g_drag.active = TRUE;
        g_drag.port = ctl->port;
        drag_follow(xpos, ypos);
        break;
    case ADAM_CTL_KEY:
    case ADAM_CTL_FIRE:
        g_pressed = idx;
        adamsession_pointer_set(g_session, ctl->port, ctl->act_mask);
        break;
    default:
        return;
    }
    gtk_widget_queue_draw(g_console);
}

static void on_console_update(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    double startx, starty;
    (void)udata;
    if (!g_drag.active) return;
    gtk_gesture_drag_get_start_point(gest, &startx, &starty);
    drag_follow(startx + offx, starty + offy);
}

static void on_console_end(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    (void)gest; (void)offx; (void)offy; (void)udata;
    mouse_release();
}

static void on_console_cancel(GtkGesture *gest, GdkEventSequence *seq, gpointer udata)
{
    (void)gest; (void)seq; (void)udata;
    mouse_release();
}

/* Dragging off a held key or fire button lets it go. */
static void on_console_motion(GtkEventControllerMotion *ctrl, double xpos, double ypos,
                              gpointer udata)
{
    const int idx = console_hit(xpos, ypos);
    (void)ctrl; (void)udata;
    if (g_pressed >= 0 && idx != g_pressed)
        mouse_release();
    if (idx != g_hover) {
        g_hover = idx;
        gtk_widget_queue_draw(g_console);
    }
}

static void on_console_leave(GtkEventControllerMotion *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    if (g_pressed >= 0)
        mouse_release();
    if (g_hover >= 0) {
        g_hover = -1;
        gtk_widget_queue_draw(g_console);
    }
}

static gboolean on_console_tooltip(GtkWidget *widget, int xpos, int ypos, gboolean kbmode,
                                   GtkTooltip *tip, gpointer udata)
{
    const int idx = kbmode ? -1 : console_hit(xpos, ypos);
    const adam_control *ctl;
    char text[300];
    (void)widget; (void)udata;
    if (idx < 0) return FALSE;
    ctl = &g_layout[idx];
    if (ctl->target >= 0) {
        const adam_binding bind = adamsession_binding_get(g_session, ctl->target);
        char key[32], pad[40];
        adamsession_keysym_name(bind.keysym, key, sizeof key);
        g_snprintf(pad, sizeof pad, "%s",
                   bind.button != ADAM_PAD_BTN_NONE ? adam_pad_button_name(bind.button) : "none");
        g_snprintf(text, sizeof text, "%s\nKey: %s    Gamepad: %s", adam_target_name(ctl->target),
                   key[0] ? key : "none", pad);
    } else if (ctl->kind == ADAM_CTL_STICK) {
        g_snprintf(text, sizeof text, "Press and drag to tilt the stick");
    } else if (ctl->kind == ADAM_CTL_DIRECTION) {
        g_snprintf(text, sizeof text, "%s (two directions): press and hold", ctl->label);
    } else {
        return FALSE;
    }
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

/* ---- the strip ------------------------------------------------------------------------ */

static int strip_hit(const Strip *strip, double xpos, double ypos)
{
    int idx;
    for (idx = strip->count - 1; idx >= 0; idx--)
        if (xpos >= strip->items[idx].xpos && xpos < strip->items[idx].xpos + strip->items[idx].width &&
            ypos >= strip->items[idx].ypos && ypos < strip->items[idx].ypos + strip->items[idx].height)
            return idx;
    return -1;
}

static void strip_release(gboolean activate)
{
    const int idx = g_strip.pressed;
    if (idx < 0) return;
    g_strip.pressed = -1;
    /* a system action fires on release, like a real button: pressing and
     * dragging off should not reset the console */
    if (activate && is_sysact(g_strip.items[idx].target))
        run_sysaction(g_strip.items[idx].target - ADAM_TARGET_SYSACT(0));
    gtk_widget_queue_draw(g_strip.area);
}

static void on_strip_begin(GtkGestureDrag *gest, double xpos, double ypos, gpointer udata)
{
    const int idx = strip_hit(&g_strip, xpos, ypos);
    (void)gest; (void)udata;
    if (idx < 0) return;
    if (g_map_state != -2) {
        set_map_state(g_strip.items[idx].target);
        return;
    }
    g_strip.pressed = idx;
    gtk_widget_queue_draw(g_strip.area);
}

static void on_strip_end(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    (void)gest; (void)offx; (void)offy; (void)udata;
    strip_release(g_strip.hover == g_strip.pressed);
}

static void on_strip_cancel(GtkGesture *gest, GdkEventSequence *seq, gpointer udata)
{
    (void)gest; (void)seq; (void)udata;
    strip_release(FALSE);
}

static void on_strip_motion(GtkEventControllerMotion *ctrl, double xpos, double ypos,
                            gpointer udata)
{
    const int idx = strip_hit(&g_strip, xpos, ypos);
    (void)ctrl; (void)udata;
    if (idx != g_strip.hover) {
        g_strip.hover = idx;
        gtk_widget_queue_draw(g_strip.area);
    }
}

static void on_strip_leave(GtkEventControllerMotion *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    g_strip.hover = -1;
    gtk_widget_queue_draw(g_strip.area);
}

static gboolean on_strip_tooltip(GtkWidget *widget, int xpos, int ypos, gboolean kbmode,
                                 GtkTooltip *tip, gpointer udata)
{
    const int idx = kbmode ? -1 : strip_hit(&g_strip, xpos, ypos);
    adam_binding bind;
    char key[32], pad[40], text[260];
    (void)widget; (void)udata;
    if (idx < 0) return FALSE;
    bind = adamsession_binding_get(g_session, g_strip.items[idx].target);
    adamsession_keysym_name(bind.keysym, key, sizeof key);
    g_snprintf(pad, sizeof pad, "%s",
               bind.button != ADAM_PAD_BTN_NONE ? adam_pad_button_name(bind.button) : "none");
    g_snprintf(text, sizeof text, "%s\nKey: %s    Gamepad: %s",
               adam_target_name(g_strip.items[idx].target), key[0] ? key : "none", pad);
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

static void build_strip(void)
{
    Strip *strip = &g_strip;
    const int btnw = STRIP_BW;

    strip->count = STRIP_ITEMS;
    strip->pressed = strip->hover = -1;
    strip->items[0] = (StripItem){ 0, 0, btnw, STRIP_H,
                                   ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_COMPUTER),
                                   "Reset Computer" };
    strip->items[1] = (StripItem){ btnw + STRIP_GAP, 0, btnw, STRIP_H,
                                   ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_GAME),
                                   "Reset Game" };
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(strip->area), 2 * btnw + STRIP_GAP);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(strip->area), STRIP_H);
}

/* ---- the keyboard ------------------------------------------------------------- */

#define COL_KB_CASE_TOP 0xE6DCC4
#define COL_KB_CASE_BOT 0xD3C9AF
#define COL_KB_CASE_EDGE 0x9A8E7E
#define COL_KB_CASE_LIGHT 0xF4EDD9
#define COL_KB_ALPHA 0xE8E2D2
#define COL_KB_SPECIAL 0x9A8E7E
#define COL_KB_RETURN 0x7F7468
#define COL_KB_SMART 0x2A2624
#define COL_KB_INK 0x2E2A26
#define COL_KB_SMART_INK 0xF0EDE4
#define COL_KB_SHIFT_INK 0x6E675A
#define COL_KB_LIT_INK 0x14102E
#define KEY_TOP (KEY_PAD + LOGO_H + 6)

static double cap_left(const adam_keycap *cap) { return KEY_PAD + cap->left * KEY_UNIT; }
static double cap_top(const adam_keycap *cap) { return KEY_TOP + cap->top * KEY_UNIT; }

static int keys_width(void) { return (int)(g_cap_wide * KEY_UNIT + 2 * KEY_PAD + 0.5); }
static int keys_height(void) { return (int)(KEY_TOP + g_cap_high * KEY_UNIT + KEY_PAD + 0.5); }

/* The cursor keys carry UTF-8 arrows; they are drawn as triangles so no font
 * has to have them. */
static void draw_cap_arrow(cairo_t *cctx, const char *label, double cenx, double ceny,
                           guint32 colour)
{
    double dirx = 0, diry = 0;
    if (!strcmp(label, "\xe2\x86\x91")) diry = -1;
    else if (!strcmp(label, "\xe2\x86\x93")) diry = 1;
    else if (!strcmp(label, "\xe2\x86\x90")) dirx = -1;
    else dirx = 1;
    cairo_move_to(cctx, cenx + dirx * 7, ceny + diry * 7);
    cairo_line_to(cctx, cenx - dirx * 5 - diry * 6, ceny - diry * 5 + dirx * 6);
    cairo_line_to(cctx, cenx - dirx * 5 + diry * 6, ceny - diry * 5 - dirx * 6);
    cairo_close_path(cctx);
    rgb(cctx, colour);
    cairo_fill(cctx);
}

/* A long legend on two lines: "WILD CARD", "ESCAPE/WP" -> "ESCAPE/" "WP".
 * Returns the longest line's length. */
static int cap_text(const char *label, char *dst, int dstsz)
{
    int longest = 0, run = 0, out = 0;
    const gboolean split = strlen(label) > 5;
    const char *c;

    for (c = label; *c && out < dstsz - 2; c++) {
        if (split && *c == ' ') {
            dst[out++] = '\n';
            run = 0;
            continue;
        }
        dst[out++] = *c;
        run++;
        if (split && *c == '/' && c[1]) {
            dst[out++] = '\n';
            run = 0;
        }
        if (run > longest) longest = run;
    }
    dst[out] = '\0';
    return longest;
}

static void draw_keycap(cairo_t *cctx, GtkWidget *widget, int idx)
{
    const adam_keycap *cap = &g_caps[idx];
    const int bits = idx < ADAM_KBD_CAPS_MAX ? g_cap_bits[idx] : 0;
    const gboolean latched = (bits & 2) != 0;
    const gboolean lit = (bits & 3) != 0 || g_press_cap == idx;
    const double left = cap_left(cap), top = cap_top(cap);
    const double wid = cap->wide * KEY_UNIT, hgt = cap->high * KEY_UNIT;
    guint32 face = COL_KB_ALPHA, ink = COL_KB_INK, side;
    char text[48];
    double labtop = top, labhgt = hgt - 3;

    switch (cap->kind) {
    case ADAM_KEYCAP_SMARTKEY:
        face = COL_KB_SMART; ink = COL_KB_SMART_INK;
        break;
    case ADAM_KEYCAP_NORMAL:
    case ADAM_KEYCAP_SPACE:
        break;
    default:
        face = !strcmp(cap->label, "RETURN") ? COL_KB_RETURN : COL_KB_SPECIAL;
        break;
    }
    side = mix(face, 0x000000, 0.4);
    if (lit) {
        face = COL_ACCENT;
        side = mix(COL_ACCENT, 0x000000, 0.45);
        ink = COL_KB_LIT_INK;
    }
    rrect(cctx, left, top + 3, wid, hgt - 3, 5);
    rgb(cctx, side);
    cairo_fill(cctx);
    rrect(cctx, left, top, wid, hgt - 3, 5);
    rgb(cctx, face);
    cairo_fill_preserve(cctx);
    rgb(cctx, lit ? COL_HELD_INK : mix(face, 0x000000, 0.45));
    cairo_set_line_width(cctx, lit ? 2 : 1);
    cairo_stroke(cctx);

    if (cap->kind == ADAM_KEYCAP_ARROW) {
        draw_cap_arrow(cctx, cap->label, left + wid / 2, top + (hgt - 3) / 2, ink);
    } else if (cap->kind != ADAM_KEYCAP_SPACE) {
        const int longest = cap_text(cap->label, text, sizeof text);
        const gboolean two = strchr(text, '\n') != NULL;
        double pixels;
        if (cap->shifted) {
            legend(cctx, widget, cap->shifted, 8, FALSE, lit ? ink : COL_KB_SHIFT_INK, left + 2,
                   top + 1, wid - 4, 10);
            labtop = top + 8;
            labhgt = hgt - 11;
        }
        if (longest <= 1)
            pixels = 13.0;
        else
            pixels = MAX(6.5, MIN(two ? 9.0 : 11.0, (wid - 6) / (longest * 0.66)));
        legend(cctx, widget, text, pixels, TRUE, ink, left, labtop, wid, labhgt);
    }
    if (latched) {
        cairo_arc(cctx, left + 6, top + 6, 3, 0, 2 * G_PI);
        rgb(cctx, 0xFFFFFF);
        cairo_fill_preserve(cctx);
        rgb(cctx, mix(COL_ACCENT, 0x000000, 0.45));
        cairo_set_line_width(cctx, 1);
        cairo_stroke(cctx);
    }
}

static void draw_keys(GtkDrawingArea *area, cairo_t *cctx, int wid, int hgt, gpointer udata)
{
    GtkWidget *widget = GTK_WIDGET(area);
    static const guint32 rainbow[5] = { 0xD33A2F, 0xE8832A, 0xE6C93A, 0x4FA83D, 0x3B6FD1 };
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, hgt);
    double stripx;
    int idx;
    (void)udata;

    /* the cream case */
    stop_rgb(grad, 0, COL_KB_CASE_TOP);
    stop_rgb(grad, 1, COL_KB_CASE_BOT);
    rrect(cctx, 0.5, 0.5, wid - 1, hgt - 1, 14);
    cairo_set_source(cctx, grad);
    cairo_fill_preserve(cctx);
    cairo_pattern_destroy(grad);
    rgb(cctx, COL_KB_CASE_EDGE);
    cairo_set_line_width(cctx, 1);
    cairo_stroke(cctx);
    cairo_move_to(cctx, 16, 2.5);
    cairo_line_to(cctx, wid - 16, 2.5);
    rgb(cctx, COL_KB_CASE_LIGHT);
    cairo_set_line_width(cctx, 1.5);
    cairo_stroke(cctx);

    /* the logo bar over the SmartKeys: the name, then the rainbow stripe */
    rrect(cctx, KEY_PAD, KEY_PAD, wid - 2 * KEY_PAD, LOGO_H, 3);
    rgb(cctx, COL_BRAND_BAR);
    cairo_fill(cctx);
    legend(cctx, widget, "ADAM", 10, TRUE, 0xF0EDE4, KEY_PAD + 6, KEY_PAD, 44, LOGO_H);
    stripx = KEY_PAD + 58;
    for (idx = 0; idx < 5; idx++) {
        const double seg = (wid - KEY_PAD - 8 - stripx) / 5;
        cairo_rectangle(cctx, stripx + idx * seg, KEY_PAD + LOGO_H / 2.0 - 2, seg + 0.5, 4);
        rgb(cctx, rainbow[idx]);
        cairo_fill(cctx);
    }

    if (!g_caps)
        return;
    for (idx = 0; idx < g_cap_count; idx++)
        draw_keycap(cctx, widget, idx);
}

static int cap_at(double xpos, double ypos)
{
    int idx;
    for (idx = g_cap_count - 1; idx >= 0; idx--) {
        const adam_keycap *cap = &g_caps[idx];
        const double left = cap_left(cap), top = cap_top(cap);
        if (xpos >= left && xpos < left + cap->wide * KEY_UNIT && ypos >= top &&
            ypos < top + cap->high * KEY_UNIT)
            return idx;
    }
    return -1;
}

static void cap_release(void)
{
    if (g_press_cap < 0)
        return;
    adamsession_keyboard_press(g_session, g_press_cap, 0);
    g_press_cap = -1;
}

/* Samples the caps' flash and latch bits; TRUE if one changed. */
static gboolean sample_caps(void)
{
    gboolean changed = FALSE;
    int idx;
    for (idx = 0; idx < g_cap_count && idx < ADAM_KBD_CAPS_MAX; idx++) {
        const unsigned char bits = (unsigned char)adamsession_keyboard_held(g_session, idx);
        if (bits != g_cap_bits[idx]) {
            g_cap_bits[idx] = bits;
            changed = TRUE;
        }
    }
    return changed;
}

static void on_keys_begin(GtkGestureDrag *gest, double xpos, double ypos, gpointer udata)
{
    const int hit = cap_at(xpos, ypos);
    (void)gest; (void)udata;

    if (hit < 0 || g_map_state != -2)
        return;                 /* the keyboard is inert in Map mode */
    cap_release();
    g_press_cap = hit;
    adamsession_keyboard_press(g_session, hit, 1);
    sample_caps();
    gtk_widget_queue_draw(g_keys_area);
}

static void on_keys_update(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    double startx, starty;
    (void)udata;
    gtk_gesture_drag_get_start_point(gest, &startx, &starty);
    if (g_press_cap >= 0 && cap_at(startx + offx, starty + offy) != g_press_cap) {
        /* dragging off the cap lets it go */
        cap_release();
        sample_caps();
        gtk_widget_queue_draw(g_keys_area);
    }
}

static void on_keys_end(GtkGestureDrag *gest, double offx, double offy, gpointer udata)
{
    (void)gest; (void)offx; (void)offy; (void)udata;
    cap_release();
    sample_caps();
    gtk_widget_queue_draw(g_keys_area);
}

static void on_keys_cancel(GtkGesture *gest, GdkEventSequence *seq, gpointer udata)
{
    (void)gest; (void)seq; (void)udata;
    cap_release();
    sample_caps();
}

static gboolean on_keys_tooltip(GtkWidget *widget, int xpos, int ypos, gboolean kbmode,
                                GtkTooltip *tip, gpointer udata)
{
    const int hit = kbmode ? -1 : cap_at(xpos, ypos);
    (void)widget; (void)udata;
    if (hit < 0 || g_caps[hit].kind == ADAM_KEYCAP_ARROW || g_caps[hit].kind == ADAM_KEYCAP_SPACE)
        return FALSE;
    if (g_caps[hit].kind == ADAM_KEYCAP_MODIFIER)
        gtk_tooltip_set_text(tip, "Sticky: tap it, then the key");
    else if (g_caps[hit].kind == ADAM_KEYCAP_LOCK)
        gtk_tooltip_set_text(tip, "Tap to lock capitals on, tap again to release");
    else
        gtk_tooltip_set_text(tip, g_caps[hit].label);
    return TRUE;
}

/* ---- refresh ---------------------------------------------------------------------- */

static void refresh_all(void)
{
    if (g_console) gtk_widget_queue_draw(g_console);
    if (g_strip.area) gtk_widget_queue_draw(g_strip.area);
    if (g_keys_area) {
        /* inert and dimmed while Map is on */
        gtk_widget_queue_draw(g_keys_area);
        gtk_widget_set_sensitive(g_keys_area, g_map_state == -2);
        gtk_widget_set_opacity(g_keys_area, g_map_state == -2 ? 1.0 : 0.4);
    }
}

/* Every frame while the window is on screen: what the console sees held,
 * painted in the accent, and the stick caps eased toward it. Redraws only
 * when something moved. */
static gboolean frame_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer udata)
{
    unsigned held[ADAM_PORTS];
    gboolean changed = FALSE;
    int idx, port;
    (void)clock; (void)udata;

    for (port = 0; port < ADAM_PORTS; port++)
        held[port] = adamsession_buttons_held(g_session, port);
    for (idx = 0; idx < g_part_count && idx < MAX_PARTS; idx++) {
        const gboolean lit = part_held(&g_layout[idx], held);
        if (lit != g_lit[idx]) {
            g_lit[idx] = lit;
            changed = TRUE;
        }
    }
    for (port = 0; port < ADAM_PORTS; port++)
        if (ease_cap(port, held[port]))
            changed = TRUE;
    if (changed)
        gtk_widget_queue_draw(widget);
    if (sample_caps() && g_keys_area)
        gtk_widget_queue_draw(g_keys_area);
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int idx;
    for (idx = 0; idx < g_pad_row_count; idx++) {
        const int eff = adamsession_gamepad_effective_port(g_session, idx);
        char text[32];
        if (eff >= 0) g_snprintf(text, sizeof text, "drives controller %d", eff + 1);
        else g_snprintf(text, sizeof text, "drives nothing");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_eff[idx])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_eff[idx]), text);
    }
}

static void on_assign_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    const int idx = GPOINTER_TO_INT(udata);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    (void)pspec;
    /* 0 = automatic (connection order), 1-2 = that controller */
    adamsession_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name, the controller it drives and the one it is set
 * to. The area is PAD_ROWS rows tall whatever is plugged in (more pads
 * scroll), so the window never changes size. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Controller 1", "Controller 2", NULL };
    GtkWidget *child;
    int count, idx;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_pad_row_count = 0;

    count = adamsession_gamepad_count(g_session);
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected - plug one in");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_widget_set_size_request(none, -1, PAD_ROW_H);
        gtk_widget_set_tooltip_text(none, "A gamepad is picked up as it appears");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PADS; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *drop, *eff;
        char text[128];
        const int assign = adamsession_gamepad_assignment(g_session, idx);

        adamsession_gamepad_name(g_session, idx, text, sizeof text);
        gtk_widget_set_size_request(row, -1, PAD_ROW_H);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        drop = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drop),
                                   assign < 0 || assign >= ADAM_PORTS ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(drop, FALSE);
        gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
        g_signal_connect(drop, "notify::selected", G_CALLBACK(on_assign_changed),
                         GINT_TO_POINTER(idx));
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), drop);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_eff[g_pad_row_count++] = eff;
    }
    refresh_pad_ports();
}

/* The pad rows (hot plug, automatic assignment changes). */
static gboolean info_tick(gpointer udata)
{
    unsigned gen;
    (void)udata;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;

    gen = adamsession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

/* Map mode's share of a key, for every window's key path (the main window
 * asks too: Map can be armed with either window focused). While Map is armed
 * a press is the key to bind (once a target is picked) or is swallowed; Escape
 * steps back. The window's own keys (F9, F11, F12) are never Map's, and neither
 * is a Ctrl chord. Returns TRUE when Map took the key. A release of a key Map
 * took reaches the session as a release of nothing, which is harmless. */
gboolean adam_controllers_map_key(guint keyval, guint code, GdkModifierType mods, gboolean down)
{
    guint32 keysym;
    char stolen[128], name[32];

    if (!g_window || g_map_state == -2)
        return FALSE;
    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12 ||
        (mods & GDK_CONTROL_MASK))
        return FALSE;
    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    if (!down)
        return TRUE;
    if (keyval == GDK_KEY_Escape) {
        g_swallow_code = code;
        set_map_state(g_map_state >= 0 ? -1 : -2);
        return TRUE;
    }
    if (g_map_state < 0)
        return TRUE;              /* armed, no target yet: typing is not typed */
    keysym = code >= 8 ? adamsession_keysym_from_evdev(code - 8) : 0;
    if (!keysym) keysym = keyval;
    adamsession_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
    adamsession_keysym_name(keysym, name, sizeof name);
    g_swallow_code = code;
    set_map_state(-1);            /* stay armed: remapping several in a row is normal */
    say_bound(name, stolen);
    return TRUE;
}

static gboolean on_key(guint keyval, guint code, GdkModifierType mods, gboolean down)
{
    /* an open popover has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    if (adam_controllers_map_key(keyval, code, mods, down))
        return TRUE;
    /* Otherwise exactly what the main window does with it. */
    if (!g_parent) return FALSE;
    return adam_window_key(ADAM_WINDOW(g_parent), keyval, code, mods, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    return on_key(keyval, code, mods, TRUE);
}

static void on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                            guint code, GdkModifierType mods, gpointer udata)
{
    (void)ctrl; (void)udata;
    on_key(keyval, code, mods, FALSE);
}

/* Keys typed here went through the main window, so a key held into a focus
 * change must be let go there too (and the mouse's, here). */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer udata)
{
    (void)ctrl; (void)udata;
    mouse_release();
    if (g_parent)
        adam_window_release_keys(ADAM_WINDOW(g_parent));
    g_swallow_code = 0;     /* its release went elsewhere */
}

static void on_active_changed(GObject *obj, GParamSpec *pspec, gpointer udata)
{
    (void)pspec; (void)udata;
    if (!gtk_window_is_active(GTK_WINDOW(obj)))
        mouse_release();
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *button, gpointer udata)
{
    (void)button; (void)udata;
    adamsession_bindings_reset(g_session);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Bindings back to their defaults");
    refresh_all();
}

/* Takes the picked control's key and gamepad button away. */
static void on_clear_clicked(GtkButton *button, gpointer udata)
{
    char stolen[16];
    (void)button; (void)udata;
    if (g_map_state < 0) return;
    adamsession_binding_set_key(g_session, g_map_state, 0, stolen, sizeof stolen);
    adamsession_binding_set_button(g_session, g_map_state, ADAM_PAD_BTN_NONE, stolen,
                                     sizeof stolen);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Cleared. Press a key or a gamepad button to bind it again");
    refresh_all();
}

static void on_show_bindings(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    adamsession_set_int(g_session, "controllers_show_bindings",
                          gtk_check_button_get_active(button) ? 1 : 0);
    refresh_all();
}

/* The window is as wide as its widest part: the controllers' case alone, or
 * the keyboard's when it is shown. Toggling resizes it (it is not user
 * resizable, so the compositor still floats it). */
static void size_areas(void)
{
    const gboolean keys = gtk_check_button_get_active(GTK_CHECK_BUTTON(g_keyboard_check));
    gtk_widget_set_visible(g_keys_area, keys);
    gtk_widget_set_size_request(g_root, keys ? MAX(console_width(), keys_width()) : console_width(), -1);
}

static void on_keyboard_toggled(GtkCheckButton *button, gpointer udata)
{
    (void)udata;
    adamsession_set_int(g_session, "controllers_keyboard",
                        gtk_check_button_get_active(button) ? 1 : 0);
    if (!gtk_check_button_get_active(button)) {
        cap_release();
        adamsession_keyboard_release(g_session);
    }
    size_areas();
    sample_caps();
    refresh_all();
}

static void hide_window(void)
{
    set_map_state(-2);
    mouse_release();
    adamsession_keyboard_release(g_session);    /* the keyboard's latches */
    g_swallow_code = 0;
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *window, gpointer udata)
{
    (void)window; (void)udata;
    hide_window();
    return TRUE;
}

/* The pad line's timer runs only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. (The per-frame tick
 * stops by itself while the console is unmapped.) */
static void on_window_map(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (!g_info_timer) g_info_timer = g_timeout_add(250, info_tick, NULL);
    info_tick(NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_info_timer) g_source_remove(g_info_timer);
    g_info_timer = 0;
}

static void on_destroy(GtkWidget *widget, gpointer udata)
{
    (void)widget; (void)udata;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_info_timer) g_source_remove(g_info_timer);
    g_capture_timer = g_info_timer = 0;
    memset(&g_strip, 0, sizeof g_strip);
    g_console = NULL;
    g_keys_area = g_keyboard_check = g_root = NULL;
    g_press_cap = -1;
    memset(g_cap_bits, 0, sizeof g_cap_bits);
    g_window = NULL;
    g_pad_box = NULL;
    g_pad_row_count = 0;
    g_map_state = -2;
    g_hover = g_pressed = -1;
    g_drag.active = FALSE;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *maprow, *toolbar, *header, *defaults, *padscroll;
    GtkEventController *keys, *focus, *motion;
    GtkGesture *drag;

    adam_css_install();
    g_layout = adamsession_controls_layout(&g_part_count, &g_layout_wid, &g_layout_hgt);
    if (g_part_count > MAX_PARTS) g_part_count = MAX_PARTS;
    g_caps = adamsession_keyboard_layout(&g_cap_count, &g_cap_wide, &g_cap_high);
    if (g_cap_count > ADAM_KBD_CAPS_MAX) g_cap_count = ADAM_KBD_CAPS_MAX;
    memset(g_cap_bits, 0, sizeof g_cap_bits);
    g_press_cap = -1;
    memset(g_lit, 0, sizeof g_lit);
    memset(g_anim, 0, sizeof g_anim);
    g_hover = g_pressed = -1;

    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* min == max: Wayland compositors float it */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    /* the controllers */
    g_console = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_console), console_width());
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_console), console_height());
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_console), draw_console, NULL, NULL);
    gtk_widget_set_halign(g_console, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a control must not steal focus. */
    gtk_widget_set_focusable(g_console, FALSE);
    gtk_widget_set_has_tooltip(g_console, TRUE);
    g_signal_connect(g_console, "query-tooltip", G_CALLBACK(on_console_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_console_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_console_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_console_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_console_cancel), NULL);
    gtk_widget_add_controller(g_console, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_console_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_console_leave), NULL);
    gtk_widget_add_controller(g_console, motion);
    gtk_widget_add_tick_callback(g_console, frame_tick, NULL, NULL);

    /* the strip */
    memset(&g_strip, 0, sizeof g_strip);
    g_strip.area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_strip.area), draw_strip, &g_strip, NULL);
    gtk_widget_set_halign(g_strip.area, GTK_ALIGN_CENTER);
    gtk_widget_set_focusable(g_strip.area, FALSE);
    gtk_widget_set_has_tooltip(g_strip.area, TRUE);
    g_signal_connect(g_strip.area, "query-tooltip", G_CALLBACK(on_strip_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_strip_begin), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_strip_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_strip_cancel), NULL);
    gtk_widget_add_controller(g_strip.area, GTK_EVENT_CONTROLLER(drag));
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_strip_motion), NULL);
    g_signal_connect(motion, "leave", G_CALLBACK(on_strip_leave), NULL);
    gtk_widget_add_controller(g_strip.area, motion);
    build_strip();

    /* the keyboard: hidden unless the Keyboard check is on */
    g_keys_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_keys_area), draw_keys, NULL, NULL);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_keys_area), keys_width());
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_keys_area), keys_height());
    gtk_widget_set_halign(g_keys_area, GTK_ALIGN_CENTER);
    /* Not focusable: clicking a cap must not steal the focus from typing. */
    gtk_widget_set_focusable(g_keys_area, FALSE);
    gtk_widget_set_has_tooltip(g_keys_area, TRUE);
    g_signal_connect(g_keys_area, "query-tooltip", G_CALLBACK(on_keys_tooltip), NULL);
    drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_keys_begin), NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_keys_update), NULL);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_keys_end), NULL);
    g_signal_connect(drag, "cancel", G_CALLBACK(on_keys_cancel), NULL);
    gtk_widget_add_controller(g_keys_area, GTK_EVENT_CONTROLLER(drag));

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    g_root = root;
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);
    gtk_box_append(GTK_BOX(root), g_console);
    gtk_box_append(GTK_BOX(root), g_strip.area);
    gtk_box_append(GTK_BOX(root), g_keys_area);

    /* the pad rows: a fixed-height area, so the window never resizes */
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PAD_GAP);
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_H + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(root), padscroll);
    g_pad_generation = adamsession_gamepad_generation(g_session);
    refresh_pads();

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    g_clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_focus_on_click(g_clear_button, FALSE);
    gtk_widget_set_tooltip_text(g_clear_button, "Take the picked control's key and gamepad button away");
    g_signal_connect(g_clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_show_bindings = gtk_check_button_new_with_label("Show bindings");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_show_bindings),
        show_bindings_saved());
    gtk_widget_set_focus_on_click(g_show_bindings, FALSE);
    g_signal_connect(g_show_bindings, "toggled", G_CALLBACK(on_show_bindings), NULL);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    g_keyboard_check = gtk_check_button_new_with_label("Keyboard");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(g_keyboard_check),
        adamsession_get_int(g_session, "controllers_keyboard", 0) != 0);
    gtk_widget_set_focus_on_click(g_keyboard_check, FALSE);
    gtk_widget_set_tooltip_text(g_keyboard_check, "Show the ADAM's keyboard under the controllers");
    g_signal_connect(g_keyboard_check, "toggled", G_CALLBACK(on_keyboard_toggled), NULL);
    gtk_box_append(GTK_BOX(maprow), g_show_bindings);
    gtk_box_append(GTK_BOX(maprow), g_keyboard_check);
    gtk_box_append(GTK_BOX(root), maprow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint ("Press a key or a gamepad button for Controller 1: Right
     * Fire (Esc cancels)") wraps onto a second line rather than clip its
     * "Esc cancels" or widen the window; two lines are always reserved, so
     * the window never changes height either */
    gtk_label_set_wrap(GTK_LABEL(g_map_hint), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(g_map_hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(g_map_hint), 2);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    gtk_label_set_yalign(GTK_LABEL(g_map_hint), 0);
    {
        PangoLayout *twolines = gtk_widget_create_pango_layout(g_map_hint, "A\nA");
        int linesw, linesh;
        pango_layout_get_pixel_size(twolines, &linesw, &linesh);
        g_object_unref(twolines);
        gtk_widget_set_size_request(g_map_hint, -1, linesh);
    }
    gtk_box_append(GTK_BOX(root), g_map_hint);

    header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    adw_window_title_new("Controllers", "Coleco ADAM"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), adam_help_button("controllers"));
    adam_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    size_areas();
    set_map_state(-2);
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void ensure_window(GtkWindow *parent, adamsession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
}

void adam_controllers_show(GtkWindow *parent, adamsession *session)
{
    ensure_window(parent, session);
    gtk_window_present(GTK_WINDOW(g_window));
}

void adam_controllers_toggle(GtkWindow *parent, adamsession *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        gtk_window_present(GTK_WINDOW(g_window));
}

gboolean adam_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
