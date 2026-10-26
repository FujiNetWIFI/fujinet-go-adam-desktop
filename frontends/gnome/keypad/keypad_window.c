/*
 * AdamKeypadWindow -- both ColecoVision hand controllers side by side: a 3x4
 * keypad, a four-way stick and the two fire buttons each, the ADAM's two
 * resets, and Map mode.
 *
 * Every control is pressed with a raw GtkGestureClick (press AND release),
 * not GtkButton's "clicked": the emulator samples the controller register
 * once per frame, so a value present only for the instant of a click falls
 * between frames and a polling game never sees it. A control on screen is
 * HELD for as long as the mouse button is down, like the plastic one. The
 * gesture's "cancel" is wired to the release path too, so dragging off a
 * control cannot leave the machine believing it is held forever.
 *
 * What the machine sees held -- keyboard, gamepads and this window together
 * (adamsession_buttons_held) -- is painted in the accent colour, polled every
 * 50 ms while the window is visible, so a numpad press or a gamepad lights
 * the matching control and the window doubles as an input tester.
 *
 * FIXED SIZE and FLOATING: a non-resizable transient window, so a tiling
 * compositor floats it instead of stretching the buttons. SINGLETON, hidden
 * rather than destroyed, so it comes back where it was.
 *
 * FOCUS: GTK4 has no reliable way to refuse focus on a toplevel, so this
 * window forwards keys through the same path as the main window and typing
 * drives the machine whichever window is focused.
 *
 * MAP MODE: the Map toggle arms a two-step rebind -- click a control to pick
 * the target, then press a key (any key, modifiers and Escape included) OR a
 * gamepad button. While armed the gestures pick instead of injecting input and
 * keys go to Map, not the machine. Rebinding steals the key from whatever
 * held it (core/src/bindings.c). Leaving Map cancels the capture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#include <string.h>

#include "../window.h"

#define EM_DASH "\xe2\x80\x94"

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the main window: resets go through it */
static adamsession *g_session;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_clear_button;
static GtkWidget *g_map_hint;
static guint g_tick_id;

typedef struct {
    GtkWidget *button;
    GtkWidget *label;
    int target;
    gboolean lit;
} control;

static control g_controls[ADAM_TARGET_COUNT];
static int g_ncontrols;

static void refresh_labels(void);

/* ---- pressing ------------------------------------------------------------- */

static void press_target(int target, int down)
{
    if (target >= ADAM_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off must not reset the machine. */
        if (!down) {
            int sa = target - ADAM_TARGET_SYSACT(0);
            if (g_parent)
                adam_window_run_sysaction(ADAM_WINDOW(g_parent), sa);
            else
                adamsession_sysaction(g_session, sa);
        }
        return;
    }
    adamsession_press(g_session, target, down);
}

static void set_hint(const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_map_hint), text);
}

static void set_lit(control *c, gboolean on)
{
    if (c->lit == on)
        return;
    c->lit = on;
    if (on) gtk_widget_add_css_class(c->button, "adam-accent");
    else gtk_widget_remove_css_class(c->button, "adam-accent");
}

static void set_map_state(int state)
{
    g_map_state = state;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_map_button), state != -2);
    gtk_widget_set_sensitive(g_clear_button, state >= 0);
    if (state >= 0) {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s",
                   adam_target_name(state));
        set_hint(msg);
        adamsession_gamepad_capture_begin(g_session);
    } else {
        adamsession_gamepad_capture_cancel(g_session);
        set_hint(state == -1 ? "Click a control to remap" : "");
    }
    refresh_labels();
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y,
                       gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;

    if (g_map_state == -1) {
        set_map_state(target);
        return;
    }
    if (g_map_state >= 0)
        return;   /* waiting for a key; ignore further clicks */
    press_target(target, 1);
}

static void on_released(GtkGestureClick *g, int n, double x, double y,
                        gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq,
                         gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)seq;
    if (g_map_state != -2) return;
    if (target >= ADAM_TARGET_SYSACT(0)) return;   /* dragged off: no reset */
    press_target(target, 0);
}

/* A fixed-size control whose face is `face`; in Map mode the label shows the
 * binding instead (ellipsized, so a long key name never resizes the window). */
static GtkWidget *control_button(const char *face, int target, int w, int h)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *label = gtk_label_new(face);
    GtkGesture *g = gtk_gesture_click_new();

    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_button_set_child(GTK_BUTTON(b), label);
    gtk_widget_add_css_class(b, "adam-key");
    gtk_widget_set_size_request(b, w, h);
    /* Raw press/release, never "clicked": a keypad key is held. */
    g_signal_connect(g, "pressed", G_CALLBACK(on_pressed), GINT_TO_POINTER(target));
    g_signal_connect(g, "released", G_CALLBACK(on_released), GINT_TO_POINTER(target));
    g_signal_connect(g, "cancel", G_CALLBACK(on_cancelled), GINT_TO_POINTER(target));
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    /* Never focusable: clicking a control must not steal focus from the
     * display, and tabbing through 38 buttons is nobody's idea of input. */
    gtk_widget_set_focusable(b, FALSE);
    gtk_widget_set_focus_on_click(b, FALSE);
    g_object_set_data_full(G_OBJECT(b), "face", g_strdup(face), g_free);

    if (g_ncontrols < ADAM_TARGET_COUNT) {
        g_controls[g_ncontrols].button = b;
        g_controls[g_ncontrols].label = label;
        g_controls[g_ncontrols].target = target;
        g_controls[g_ncontrols].lit = FALSE;
        g_ncontrols++;
    }
    return b;
}

/* In Map mode each control shows the key (and gamepad button) bound to it, so
 * choosing what to change does not require remembering the table; the picked
 * one is in the accent colour. */
static void refresh_labels(void)
{
    int i;
    for (i = 0; i < g_ncontrols; i++) {
        control *c = &g_controls[i];
        if (g_map_state != -2) {
            adam_binding bind = adamsession_binding_get(g_session, c->target);
            char key[48], text[96];
            adamsession_keysym_name(bind.keysym, key, sizeof key);
            if (bind.button != ADAM_PAD_BTN_NONE)
                g_snprintf(text, sizeof text, "%s\n%s", *key ? key : EM_DASH,
                           adam_pad_button_name(bind.button));
            else
                g_snprintf(text, sizeof text, "%s", *key ? key : EM_DASH);
            gtk_label_set_text(GTK_LABEL(c->label), text);
            gtk_widget_add_css_class(c->label, "caption");
            set_lit(c, c->target == g_map_state);
        } else {
            gtk_label_set_text(GTK_LABEL(c->label),
                               g_object_get_data(G_OBJECT(c->button), "face"));
            gtk_widget_remove_css_class(c->label, "caption");
            set_lit(c, FALSE);   /* tick() relights what is held */
        }
    }
}

/* ---- polling -------------------------------------------------------------- */

static void capture_poll(void)
{
    int button;
    char stolen[128], msg[256];
    int target = g_map_state;

    if (!adamsession_gamepad_capture_poll(g_session, &button))
        return;
    adamsession_binding_set_button(g_session, target, button, stolen, sizeof stolen);
    set_map_state(-1);   /* stay armed: remapping several in a row is normal */
    if (stolen[0])
        g_snprintf(msg, sizeof msg, "%s = pad %s (was %s)",
                   adam_target_name(target), adam_pad_button_name(button), stolen);
    else
        g_snprintf(msg, sizeof msg, "%s = pad %s", adam_target_name(target),
                   adam_pad_button_name(button));
    set_hint(msg);
}

static gboolean tick(gpointer d)
{
    uint32_t held[2];
    int i;
    (void)d;

    if (g_map_state >= 0) {
        capture_poll();
        return G_SOURCE_CONTINUE;
    }
    if (g_map_state != -2)
        return G_SOURCE_CONTINUE;
    held[0] = adamsession_buttons_held(g_session, 0);
    held[1] = adamsession_buttons_held(g_session, 1);
    for (i = 0; i < g_ncontrols; i++) {
        int t = g_controls[i].target;
        gboolean on = FALSE;
        if (t < 2 * ADAM_ACT_PER_PORT)
            on = (held[t / ADAM_ACT_PER_PORT] >> (t % ADAM_ACT_PER_PORT)) & 1;
        set_lit(&g_controls[i], on);
    }
    return G_SOURCE_CONTINUE;
}

static void on_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_tick_id)
        g_tick_id = g_timeout_add(50, tick, NULL);
}

/* Hiding the window (close, F9, menu) leaves Map and stops the poll. */
static void on_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_tick_id) {
        g_source_remove(g_tick_id);
        g_tick_id = 0;
    }
    if (g_window && g_map_state != -2)
        set_map_state(-2);
}

static void on_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_tick_id) {
        g_source_remove(g_tick_id);
        g_tick_id = 0;
    }
    g_window = NULL;
    g_parent = NULL;
    g_map_state = -2;
    g_ncontrols = 0;
}

/* ---- one controller ------------------------------------------------------- */

#define KEY_W 84
#define KEY_H 38
#define FIRE_H 44   /* two lines of Map-mode label: no jump when Map toggles */

/* The physical 3x4 layout: 1-9 in reading order, then * 0 #; the entries are
 * ADAM_ACT_KEYPAD offsets (0-9 digits, 10 '*', 11 '#'). */
static const int keypad_order[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };
static const char *const keypad_face[12] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#"
};

static GtkWidget *stick_button(const char *face, int port, int act)
{
    return control_button(face, ADAM_TARGET_PORT(port, act), KEY_W, KEY_H);
}

static GtkWidget *build_controller(int port)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *pad = gtk_grid_new();
    GtkWidget *stick = gtk_grid_new();
    GtkWidget *fires = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *title;
    char label[24];
    int i;

    g_snprintf(label, sizeof label, "Controller %d", port + 1);
    title = gtk_label_new(label);
    gtk_widget_add_css_class(title, "heading");
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);

    gtk_grid_set_row_spacing(GTK_GRID(pad), 6);
    gtk_grid_set_column_spacing(GTK_GRID(pad), 6);
    for (i = 0; i < 12; i++) {
        int key = keypad_order[i];
        gtk_grid_attach(GTK_GRID(pad),
                        control_button(keypad_face[key],
                                       ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + key),
                                       KEY_W, KEY_H),
                        i % 3, i / 3, 1, 1);
    }

    gtk_grid_set_row_spacing(GTK_GRID(stick), 6);
    gtk_grid_set_column_spacing(GTK_GRID(stick), 6);
    gtk_grid_attach(GTK_GRID(stick), stick_button("\xe2\x96\xb2", port, ADAM_ACT_UP), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(stick), stick_button("\xe2\x97\x80", port, ADAM_ACT_LEFT), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(stick), stick_button("\xe2\x96\xb6", port, ADAM_ACT_RIGHT), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(stick), stick_button("\xe2\x96\xbc", port, ADAM_ACT_DOWN), 1, 2, 1, 1);

    gtk_box_append(GTK_BOX(fires),
                   control_button("Fire L", ADAM_TARGET_PORT(port, ADAM_ACT_FIRE_L),
                                  (3 * KEY_W + 12 - 6) / 2, FIRE_H));
    gtk_box_append(GTK_BOX(fires),
                   control_button("Fire R", ADAM_TARGET_PORT(port, ADAM_ACT_FIRE_R),
                                  (3 * KEY_W + 12 - 6) / 2, FIRE_H));

    gtk_widget_set_halign(pad, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(stick, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(fires, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), pad);
    gtk_box_append(GTK_BOX(box), stick);
    gtk_box_append(GTK_BOX(box), fires);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

/* ---- keyboard ------------------------------------------------------------- */

gboolean adam_keypad_window_map_key(guint32 keysym)
{
    char stolen[128], msg[256], name[48];
    int target = g_map_state;

    if (g_map_state == -2)
        return FALSE;
    if (g_map_state == -1)
        return TRUE;   /* swallow keys while choosing a target */

    adamsession_binding_set_key(g_session, target, keysym, stolen, sizeof stolen);
    adamsession_keysym_name(keysym, name, sizeof name);
    set_map_state(-1);   /* stay armed: remapping several in a row is normal */
    if (stolen[0])
        g_snprintf(msg, sizeof msg, "%s = %s (was %s)", adam_target_name(target),
                   name, stolen);
    else
        g_snprintf(msg, sizeof msg, "%s = %s", adam_target_name(target), name);
    set_hint(msg);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;

    /* Exactly like the main window, so playing works whichever window has
     * focus: Map capture and the bindings first, then F9, then the keyboard. */
    if (adam_window_input_key(g_session, keyval, code, st, TRUE))
        return TRUE;
    if (keyval == GDK_KEY_F9) {
        adam_keypad_window_toggle(NULL, g_session);
        return TRUE;
    }
    return adam_window_type_key(g_session, keyval, st);
}

static void on_key_released(GtkEventControllerKey *c, guint keyval,
                            guint code, GdkModifierType st, gpointer d)
{
    (void)c; (void)d;
    adam_window_input_key(g_session, keyval, code, st, FALSE);
}

/* Keys held into a focus change must not stay down. */
static void on_active_changed(GObject *o, GParamSpec *ps, gpointer d)
{
    (void)ps; (void)d;
    if (!gtk_window_is_active(GTK_WINDOW(o)))
        adamsession_release_all(g_session);
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_toggled(GtkToggleButton *b, gpointer d)
{
    int want;
    (void)d;
    want = gtk_toggle_button_get_active(b) ? (g_map_state != -2 ? g_map_state : -1) : -2;
    if (want != g_map_state)
        set_map_state(want);
}

/* Clears both the key and the gamepad button of the picked control. */
static void on_clear_clicked(GtkButton *b, gpointer d)
{
    char msg[128];
    int target = g_map_state;
    (void)b; (void)d;
    if (target < 0)
        return;
    adamsession_binding_set_key(g_session, target, 0, NULL, 0);
    adamsession_binding_set_button(g_session, target, ADAM_PAD_BTN_NONE, NULL, 0);
    set_map_state(-1);
    g_snprintf(msg, sizeof msg, "%s cleared", adam_target_name(target));
    set_hint(msg);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    adamsession_bindings_reset(g_session);
    refresh_labels();
    set_hint("Bindings reset to the defaults");
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)d;
    /* Hide, do not destroy: the position survives closing it. Unmap leaves
     * Map mode. */
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *ports, *system, *maprow, *toolbar, *header, *defaults;
    GtkEventController *keys;

    g_parent = parent;
    g_window = adw_window_new();
    g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&g_parent);
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    gtk_window_set_icon_name(GTK_WINDOW(g_window), adam_icon_name());
    gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "map", G_CALLBACK(on_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_unmap), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_signal_connect(g_window, "notify::is-active", G_CALLBACK(on_active_changed), NULL);

    g_ncontrols = 0;

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    ports = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(ports), build_controller(0));
    gtk_box_append(GTK_BOX(ports), build_controller(1));
    gtk_box_append(GTK_BOX(root), ports);

    system = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(system, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(system),
                   control_button("Reset Computer",
                                  ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_COMPUTER), 160, 36));
    gtk_box_append(GTK_BOX(system),
                   control_button("Reset Game",
                                  ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_GAME), 160, 36));
    gtk_box_append(GTK_BOX(root), system);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_toggle_button_new_with_label("Map");
    gtk_widget_add_css_class(g_map_button, "adam-toggle");
    gtk_widget_set_size_request(g_map_button, 72, -1);
    gtk_widget_set_focusable(g_map_button, FALSE);
    gtk_widget_set_focus_on_click(g_map_button, FALSE);
    g_signal_connect(g_map_button, "toggled", G_CALLBACK(on_map_toggled), NULL);
    g_clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_size_request(g_clear_button, 72, -1);
    gtk_widget_set_focusable(g_clear_button, FALSE);
    gtk_widget_set_focus_on_click(g_clear_button, FALSE);
    gtk_widget_set_sensitive(g_clear_button, FALSE);
    g_signal_connect(g_clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    gtk_widget_set_size_request(defaults, 84, -1);
    gtk_widget_set_focusable(defaults, FALSE);
    gtk_widget_set_focus_on_click(defaults, FALSE);
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    gtk_label_set_wrap(GTK_LABEL(g_map_hint), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(g_map_hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(g_map_hint), 2);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(g_map_hint), 24);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 24);
    gtk_widget_set_size_request(g_map_hint, -1, 40);   /* two lines, always */
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(root), maprow);

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    set_map_state(-2);
}

void adam_keypad_window_toggle(GtkWindow *parent, adamsession *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);

    if (gtk_widget_get_visible(g_window))
        gtk_widget_set_visible(g_window, FALSE);   /* unmap leaves Map mode */
    else
        gtk_window_present(GTK_WINDOW(g_window));
}

gboolean adam_keypad_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
