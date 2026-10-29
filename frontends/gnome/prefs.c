/*
 * Preferences dialog: machine options (applied via session restart on
 * close) and display options (applied live). Values persist through the
 * shared settings store, so the KDE frontend sees the same configuration.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "window.h"

/* Option lists mirror the Android app's Settings.kt. */
static const char *const machine_names[] = {"ADAM (computer)",
                                            "ColecoVision (game)", NULL};
static const char *const palette_names[] = {"Default (TMS9928)", "Palette 2",
                                            "Palette 3", "Palette 4", NULL};
static const char *const expansion_names[] = {
    "None",
    "Roller controller (mouse)",
    "Roller controller (joystick)",
    "Driving module (joystick)",
    "Driving module (mouse)",
    "Super Action speed roller, both ports (mouse)",
    "Speed roller, port 1 (mouse)",
    "Speed roller, port 2 (mouse)",
    NULL};
static const char *const joystick_names[] = {"No joystick", "Both ports",
                                             "Port 2 only", "Port 1 only",
                                             NULL};
static const char *const aspect_names[] = {"Square pixels (256:212)",
                                           "TV (4:3)", "Integer scale", NULL};

typedef struct {
    AdamWindow *window;
    adamsession *session;
    void (*display_changed)(AdamWindow *window);
    gboolean machine_dirty;
} PrefsState;

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    gboolean is_display;
} RowBinding;

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (adamsession_get_int(b->state->session, b->key, b->def) == sel)
        return;
    adamsession_set_int(b->state->session, b->key, sel);
    if (b->is_display) {
        if (b->state->display_changed)
            b->state->display_changed(b->state->window);
    } else {
        b->state->machine_dirty = TRUE;
    }
}

static void switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (adamsession_get_int(b->state->session, b->key, b->def) == on)
        return;
    adamsession_set_int(b->state->session, b->key, on);
    if (b->is_display) {
        if (b->state->display_changed)
            b->state->display_changed(b->state->window);
    } else {
        b->state->machine_dirty = TRUE;
    }
}

static RowBinding *binding_new(PrefsState *state, const char *key, int def,
                               gboolean is_display)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    b->is_display = is_display;
    return b;
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *key, int def,
                            const char *const *names, gboolean is_display)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(names);
    RowBinding *b = binding_new(state, key, def, is_display);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(
        ADW_COMBO_ROW(row),
        (guint)adamsession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          b, (GClosureNotify)g_free, 0);
    g_object_unref(model);
    return row;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *key, int def, gboolean is_display)
{
    GtkWidget *row = adw_switch_row_new();
    RowBinding *b = binding_new(state, key, def, is_display);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              adamsession_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed),
                          b, (GClosureNotify)g_free, 0);
    return row;
}

#define EXTERNAL_PLACEHOLDER "0.0.0.0:65216"

/* The address field: valid text (or none, meaning 127.0.0.1:65216) is saved
 * and marks the machine dirty; anything else is flagged and not saved. */
static void addr_changed(GtkEditable *entry, gpointer user_data)
{
    PrefsState *state = user_data;
    const char *text = gtk_editable_get_text(entry);
    char host[64];
    int port;

    if (adamsession_parse_hostport(text, host, sizeof host, &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(adamsession_get_str(state->session, "fujinet_external_addr",
                                      ""), text) == 0)
        return;
    adamsession_set_str(state->session, "fujinet_external_addr", text);
    state->machine_dirty = TRUE;
}

static void external_toggled(GObject *row, GParamSpec *pspec,
                             gpointer user_data)
{
    gtk_widget_set_sensitive(
        GTK_WIDGET(user_data),
        adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    (void)pspec;
}

static void prefs_closed(GtkWidget *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->machine_dirty) {
        adam_window_restart_session(state->window);
        adam_window_toast(state->window,
                          "Machine options applied (session restarted)");
    }
    g_free(state);
}

void adam_prefs_show(AdamWindow *parent, adamsession *session,
                     void (*display_changed)(AdamWindow *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog =
        ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page =
        ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *display =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *fujinet =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *external, *addr;

    state->window = parent;
    state->session = session;
    state->display_changed = display_changed;

    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_description(
        machine, "Applied by restarting the session when this dialog closes");
    adw_preferences_group_add(
        machine, combo_row(state, "Machine", "machine", 0, machine_names,
                           FALSE));
    adw_preferences_group_add(
        machine, combo_row(state, "Palette", "palette", 0, palette_names,
                           FALSE));
    adw_preferences_group_add(
        machine, combo_row(state, "Expansion module", "expansion", 0,
                           expansion_names, FALSE));
    adw_preferences_group_add(
        machine, combo_row(state, "Joystick mode", "joystick_mode", 1,
                           joystick_names, FALSE));
    adw_preferences_group_add(
        machine,
        switch_row(state, "Swap joystick buttons", "swap_buttons", 0, FALSE));
    adw_preferences_group_add(
        machine,
        switch_row(state, "Reverse keypad", "reverse_keypad", 0, FALSE));

    adw_preferences_group_set_title(fujinet, "FujiNet");
    adw_preferences_group_set_description(
        fujinet,
        "Skip the built-in FujiNet and wait for another instance to connect. "
        "Set that instance's [BOIP] host and port to this address. "
        "0.0.0.0 accepts connections from the network; 127.0.0.1 only from "
        "this computer.");
    external = switch_row(state, "Use external FujiNet", "fujinet_external", 0,
                          FALSE);
    addr = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr),
                                  "Listen address:port, e.g. "
                                  EXTERNAL_PLACEHOLDER);
    gtk_editable_set_text(GTK_EDITABLE(addr),
                          adamsession_get_str(session, "fujinet_external_addr",
                                              ""));
    g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), state);
    gtk_widget_set_sensitive(addr, adw_switch_row_get_active(
                                       ADW_SWITCH_ROW(external)));
    g_signal_connect(external, "notify::active", G_CALLBACK(external_toggled),
                     addr);
    adw_preferences_group_add(fujinet, external);
    adw_preferences_group_add(fujinet, addr);

    adw_preferences_group_set_title(display, "Display");
    adw_preferences_group_add(
        display, combo_row(state, "Aspect ratio", "aspect_mode", 0,
                           aspect_names, TRUE));
    adw_preferences_group_add(
        display,
        switch_row(state, "Smooth scaling", "smooth_scaling", 0, TRUE));

    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, fujinet);
    adw_preferences_page_add(page, display);
    adw_preferences_dialog_add(dialog, page);
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
