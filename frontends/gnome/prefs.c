/*
 * Preferences dialog: machine options (applied via session restart on
 * close) and display options (applied live). Values persist through the
 * shared settings store, so the KDE frontend sees the same configuration.
 * The one live exception to restart-on-close is a gamepad's controller
 * choice on the Input page.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "controllers/controllers_window.h"
#include "window.h"
#include "help/help_window.h"

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

#define MAX_PAD_ROWS 8

typedef struct {
    AdamWindow *window;
    adamsession *session;
    void (*display_changed)(AdamWindow *window);
    gboolean machine_dirty;
    AdwDialog *dialog;
    GtkWidget *pad_group;               /* the Input page's gamepad rows */
    GtkWidget *pad_rows[MAX_PAD_ROWS + 1];
    int pad_row_count;
    unsigned pad_generation;            /* the pad set the rows were built for */
    guint pad_timer;
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

/* The closure-notify form of g_free for the rows' RowBinding. */
static void binding_free(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
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
                          b, binding_free, 0);
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
                          b, binding_free, 0);
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

/* ---- the Input page ----------------------------------------------------- */

/* A row with a title and a line under it, nothing to set. */
static GtkWidget *info_row(const char *title, const char *subtitle)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    return row;
}

/* A pad row's choice: 0 is Automatic (connection order), 1-2 that controller.
 * It is live, not a machine option, so it does not restart the session. */
static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    const int idx = GPOINTER_TO_INT(g_object_get_data(row, "pad-index"));
    const int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    adamsession_gamepad_assign(state->session, idx, sel - 1);
}

/* "Driving controller N" / "Driving nothing" for a pad. */
static void pad_subtitle(PrefsState *state, int idx, char *dst, int dstsz)
{
    const int eff = adamsession_gamepad_effective_port(state->session, idx);
    if (eff >= 0)
        g_snprintf(dst, dstsz, "Driving controller %d", eff + 1);
    else
        g_snprintf(dst, dstsz, "Driving nothing");
}

static void rebuild_pad_rows(PrefsState *state)
{
    static const char *const choices[] = { "Automatic", "Controller 1", "Controller 2", NULL };
    const int count = adamsession_gamepad_count(state->session);
    char name[128], sub[64];
    int idx;

    for (idx = 0; idx < state->pad_row_count; idx++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(state->pad_group),
                                     state->pad_rows[idx]);
    state->pad_row_count = 0;

    if (count == 0) {
        GtkWidget *row = info_row("No gamepads connected",
                                  "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PAD_ROWS; idx++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        const int assign = adamsession_gamepad_assignment(state->session, idx);

        adamsession_gamepad_name(state->session, idx, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        pad_subtitle(state, idx, sub, sizeof sub);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
                                   assign < 0 || assign >= ADAM_PORTS ? 0 : (guint)assign + 1);
        g_object_set_data(G_OBJECT(row), "pad-index", GINT_TO_POINTER(idx));
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed), state);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        g_object_unref(model);
    }
}

/* Once a second: the rows again when a pad came or went, else just the
 * "Driving ..." lines (assigning one pad moves an automatic one). */
static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    const unsigned gen = adamsession_gamepad_generation(state->session);

    if (gen != state->pad_generation) {
        state->pad_generation = gen;
        rebuild_pad_rows(state);
    } else if (adamsession_gamepad_count(state->session) > 0) {
        int idx;
        for (idx = 0; idx < state->pad_row_count; idx++) {
            AdwActionRow *row = ADW_ACTION_ROW(state->pad_rows[idx]);
            char sub[64];
            pad_subtitle(state, idx, sub, sizeof sub);
            if (g_strcmp0(adw_action_row_get_subtitle(row), sub) != 0)
                adw_action_row_set_subtitle(row, sub);
        }
    }
    return G_SOURCE_CONTINUE;
}

static void on_open_controllers(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)button;
    adam_controllers_show(GTK_WINDOW(state->window), state->session);
}

static void on_reset_controls(GtkButton *button, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)button;
    adamsession_bindings_reset(state->session);
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(state->dialog),
                                     adw_toast_new("Controls back to their defaults"));
}

static AdwPreferencesPage *build_input_page(PrefsState *state)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *row, *open, *reset;

    adw_preferences_page_set_title(page, "Input");
    adw_preferences_page_set_name(page, "Input");
    adw_preferences_page_set_icon_name(page, "input-gaming-symbolic");

    adw_preferences_group_set_title(group, "Controllers");
    adw_preferences_group_set_header_suffix(group, adam_help_button("prefs/Input"));
    adw_preferences_group_set_description(
        group, "Applied by restarting the session when this dialog closes");
    adw_preferences_group_add(
        group, switch_row(state, "Enable gamepads", "enable_gamepad", 1, FALSE));
    adw_preferences_group_add(
        group, switch_row(state, "Swap joystick buttons", "swap_buttons", 0, FALSE));
    adw_preferences_page_add(page, group);

    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Gamepads");
    adw_preferences_group_set_description(
        group, "Assigned to the two controllers in connection order unless chosen here.");
    state->pad_group = GTK_WIDGET(group);
    rebuild_pad_rows(state);
    state->pad_generation = adamsession_gamepad_generation(state->session);
    adw_preferences_page_add(page, group);

    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Controls");
    adw_preferences_group_set_description(
        group, "Keys and gamepad buttons are re-mapped in the Controllers window (F9): "
               "press Map, choose a control, then press the key or gamepad button to give it.");
    row = info_row("Controllers window", "Map any of the 38 controls");
    open = gtk_button_new_with_label("Open Controllers");
    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_controllers), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
    adw_preferences_group_add(group, row);
    row = info_row("Reset Controls", "Put every key and gamepad button back to its default");
    reset = gtk_button_new_with_label("Reset Controls");
    gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset_controls), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), reset);
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(page, group);
    return page;
}

static void prefs_closed(GtkWidget *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)dialog;
    if (state->pad_timer)
        g_source_remove(state->pad_timer);
    state->pad_timer = 0;
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

    adw_preferences_page_set_name(page, "Machine");
    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_header_suffix(machine, adam_help_button("prefs/Machine"));
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
    state->dialog = ADW_DIALOG(dialog);
    adw_preferences_dialog_add(dialog, build_input_page(state));
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
