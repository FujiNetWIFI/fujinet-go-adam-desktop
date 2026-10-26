/*
 * AdamWindow: main window of the GNOME frontend. Header bar + menu over the
 * emulator display; keyboard capture routes everything except F9 (keypad
 * window), F10 (menu), F11 (fullscreen) and F12 (debugger) to the ADAM. No
 * on-screen input panels are shown unless the user asks for them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include <stdlib.h>

#include "debugger/dbg_window.h"
#include "display.h"
#include "keypad/keypad_window.h"
#include "prefs.h"
#include "webview.h"

struct _AdamWindow {
    AdwApplicationWindow parent_instance;

    adamsession *session;
    AdamDisplay *display;
    AdwToastOverlay *toasts;
    GtkMenuButton *menu_button;
};

G_DEFINE_FINAL_TYPE(AdamWindow, adam_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---- the display CSS ----------------------------------------------------
 * One display-wide provider, installed at startup so every window gets it:
 * checked toggles and highlighted buttons in the icon's colour
 * (ADAMSESSION_ACCENT_RGB) with BLACK text (the accent is light), in light
 * and dark. */

void adam_css_install(void)
{
    static gboolean done;
    GtkCssProvider *provider;
    g_autofree char *css = NULL;
    GdkDisplay *display = gdk_display_get_default();
    unsigned rgb = ADAMSESSION_ACCENT_RGB, hover = 0;
    int shift;

    if (done || !display)
        return;
    done = TRUE;
    /* a little lighter under the pointer */
    for (shift = 16; shift >= 0; shift -= 8) {
        unsigned c = (rgb >> shift) & 0xFF;
        hover |= (c + (255 - c) * 18 / 100) << shift;
    }
    css = g_strdup_printf(
        "button.adam-toggle:checked, togglebutton.adam-toggle:checked,\n"
        "button.adam-accent {\n"
        "  background-image: none; background-color: #%06X; color: #000000;\n"
        "}\n"
        "button.adam-toggle:checked:hover, button.adam-accent:hover {\n"
        "  background-image: none; background-color: #%06X; color: #000000;\n"
        "}\n"
        ".adam-accent-text { color: #%06X; font-weight: bold; }\n"
        "button.adam-key { padding-left: 4px; padding-right: 4px; }\n",
        rgb, hover, rgb);
    provider = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(provider, css);
#else
    gtk_css_provider_load_from_data(provider, css, -1);
#endif
    gtk_style_context_add_provider_for_display(
        display, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ---- helpers ------------------------------------------------------------ */

void adam_window_toast(AdamWindow *self, const char *message)
{
    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(message));
}

void adam_window_restart_session(AdamWindow *self)
{
    adamsession_start_opts opts;
    adamsession_stop(self->session);
    adamsession_default_opts(self->session, &opts);
    if (adamsession_start(self->session, &opts) != 0)
        adam_window_toast(self, adamsession_last_error(self->session));
}

static void apply_display_settings(AdamWindow *self)
{
    adam_display_set_aspect_mode(
        self->display,
        (AdamAspectMode)adamsession_get_int(self->session, "aspect_mode",
                                            ADAM_ASPECT_SQUARE_PIXELS));
    adam_display_set_smooth(
        self->display, adamsession_get_int(self->session, "smooth_scaling", 0));
}

/* ---- keyboard capture --------------------------------------------------- */

/* The session keysym of a key event. The physical key (evdev code, GDK's
 * keycode - 8) rather than the layout's keyval, so the keypad's 8/4/6/2
 * still bind with NumLock off or Shift held and a binding means the same key
 * in every frontend; a key the table lacks falls back to the keyval. */
static guint32 session_keysym(guint keyval, guint keycode)
{
    guint32 keysym = keycode >= 8 ? adamsession_keysym_from_evdev(keycode - 8) : 0;
    return keysym ? keysym : keyval;
}

gboolean adam_window_input_key(adamsession *session, guint keyval,
                               guint keycode, GdkModifierType state,
                               gboolean down)
{
    guint32 keysym = session_keysym(keyval, keycode);

    if (down && adam_keypad_window_map_key(keysym))
        return TRUE;
    /* A release always reaches the session: it clears exactly what the
     * press asserted, even if Map mode was armed in between. */
    return adamsession_input_key(session, keysym, down,
                                 (state & GDK_CONTROL_MASK) != 0) != 0;
}

gboolean adam_window_type_key(adamsession *session, guint keyval,
                              GdkModifierType state)
{
    int code;

    /* Leave Alt combos to mnemonics/WM; the ADAM keyboard has no Alt. */
    if (state & GDK_ALT_MASK)
        return FALSE;
    code = adam_key_from_event(keyval, gdk_keyval_to_unicode(keyval),
                               (state & GDK_CONTROL_MASK) != 0);
    if (code < 0)
        return FALSE;
    adamsession_key(session, (uint8_t)code);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)controller;

    /* Map capture and the controller bindings first (Ctrl+digit is the
     * built-in chord for the port 1 keypad). */
    if (adam_window_input_key(self->session, keyval, keycode, state, TRUE))
        return TRUE;

    switch (keyval) {
    case GDK_KEY_F9:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.keypad", NULL);
        return TRUE;
    case GDK_KEY_F10: /* open the primary menu */
        gtk_menu_button_popup(self->menu_button);
        return TRUE;
    case GDK_KEY_F11:
        if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
            gtk_window_unfullscreen(GTK_WINDOW(self));
        else
            gtk_window_fullscreen(GTK_WINDOW(self));
        return TRUE;
    case GDK_KEY_F12:
        gtk_widget_activate_action(GTK_WIDGET(self), "win.debugger", NULL);
        return TRUE;
    default:
        break;
    }

    return adam_window_type_key(self->session, keyval, state);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)controller;
    adam_window_input_key(self->session, keyval, keycode, state, FALSE);
}

/* Keys held into a focus change must not stay down. */
static void on_active_changed(GObject *object, GParamSpec *pspec,
                              gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)pspec;
    if (!gtk_window_is_active(GTK_WINDOW(object)))
        adamsession_release_all(self->session);
}

/* ---- actions ------------------------------------------------------------ */

void adam_window_run_sysaction(AdamWindow *self, int sysact)
{
    adamsession_sysaction(self->session, sysact);
    adam_window_toast(self, sysact == ADAM_SYSACT_RESET_GAME ? "Game reset"
                                                             : "Computer reset");
}

static void action_reset_adam(GSimpleAction *action, GVariant *param,
                              gpointer user_data)
{
    (void)action;
    (void)param;
    adam_window_run_sysaction(ADAM_WINDOW(user_data), ADAM_SYSACT_RESET_COMPUTER);
}

static void action_reset_cv(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    (void)action;
    (void)param;
    adam_window_run_sysaction(ADAM_WINDOW(user_data), ADAM_SYSACT_RESET_GAME);
}

static void import_done(GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    char dest[1024];
    g_autofree char *path = NULL;
    g_autofree char *msg = NULL;

    if (!file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    if (adamsession_import_media(self->session, path, dest, sizeof(dest)) != 0) {
        adam_window_toast(self, adamsession_last_error(self->session));
        return;
    }
    msg = g_strdup_printf("Copied to FujiNet SD: %s", strrchr(dest, '/') + 1);
    adam_window_toast(self, msg);
}

static void action_import_media(GSimpleAction *action, GVariant *param,
                                gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    (void)action;
    (void)param;

    gtk_file_filter_set_name(filter, "ADAM disk / data pack images");
    gtk_file_filter_add_suffix(filter, "dsk");
    gtk_file_filter_add_suffix(filter, "ddp");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Import Disk or Data Pack");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, import_done, self);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void cart_done(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    char dest[1024];
    g_autofree char *path = NULL;

    if (!file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    if (adamsession_import_media(self->session, path, dest, sizeof(dest)) != 0) {
        adam_window_toast(self, adamsession_last_error(self->session));
        return;
    }
    /* A cartridge boots in ColecoVision mode; persist and relaunch. */
    adamsession_set_str(self->session, "cart_path", dest);
    adamsession_set_int(self->session, "machine", 1);
    adam_window_restart_session(self);
    adam_window_toast(self, "Cartridge loaded");
}

static void action_load_cart(GSimpleAction *action, GVariant *param,
                             gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkFileFilter *filter = gtk_file_filter_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    (void)action;
    (void)param;

    gtk_file_filter_set_name(filter, "ColecoVision cartridges");
    gtk_file_filter_add_suffix(filter, "rom");
    gtk_file_filter_add_suffix(filter, "col");
    gtk_file_filter_add_suffix(filter, "bin");
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_title(dialog, "Load Cartridge");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, cart_done, self);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void action_eject_cart(GSimpleAction *action, GVariant *param,
                              gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adamsession_set_str(self->session, "cart_path", "");
    adamsession_set_int(self->session, "machine", 0);
    adam_window_restart_session(self);
    adam_window_toast(self, "Cartridge ejected; back to ADAM");
}

static void action_fujinet_config(GSimpleAction *action, GVariant *param,
                                  gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adam_fujinet_config_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adam_fujinet_log_show(GTK_WINDOW(self), self->session);
}

static void action_preferences(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adam_prefs_show(self, self->session, apply_display_settings);
}

static void action_keypad(GSimpleAction *action, GVariant *param,
                          gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adam_keypad_window_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adam_debugger_toggle(GTK_WINDOW(self), self->session);
}

static void action_about(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adw_show_about_dialog(
        GTK_WIDGET(self),
        "application-name", "FujiNet Go Adam",
        "application-icon", adam_icon_name(),
        "developer-name", "Thomas Cherryhomes",
        "version", ADAM_VERSION_STRING,
        "license-type", GTK_LICENSE_GPL_3_0,
        "comments", "Self-contained Coleco ADAM with built-in FujiNet",
        "website", "https://fujinet.online/",
        NULL);
}

/* ---- construction ------------------------------------------------------- */

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new();
    GMenu *media = g_menu_new();
    GMenu *fujinet = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *tail = g_menu_new();

    g_menu_append(machine, "Reset Computer (ADAM)", "win.reset-adam");
    g_menu_append(machine, "Reset Game (ColecoVision)", "win.reset-cv");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));

    g_menu_append(media, "Import Disk or Data Pack…", "win.import-media");
    g_menu_append(media, "Load Cartridge…", "win.load-cart");
    g_menu_append(media, "Eject Cartridge", "win.eject-cart");
    g_menu_append_section(menu, "Media", G_MENU_MODEL(media));

    g_menu_append(fujinet, "FujiNet Configuration…", "win.fujinet-config");
    g_menu_append(fujinet, "FujiNet Console Log…", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujinet));

    g_menu_append(view, "Controllers (F9)", "win.keypad");
    g_menu_append(view, "Debugger (F12)", "win.debugger");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));

    g_menu_append(tail, "Preferences…", "win.preferences");
    g_menu_append(tail, "About FujiNet Go Adam", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(machine);
    g_object_unref(media);
    g_object_unref(fujinet);
    g_object_unref(view);
    g_object_unref(tail);
    return menu;
}

static const GActionEntry win_actions[] = {
    {.name = "reset-adam", .activate = action_reset_adam},
    {.name = "reset-cv", .activate = action_reset_cv},
    {.name = "import-media", .activate = action_import_media},
    {.name = "load-cart", .activate = action_load_cart},
    {.name = "eject-cart", .activate = action_eject_cart},
    {.name = "fujinet-config", .activate = action_fujinet_config},
    {.name = "fujinet-log", .activate = action_fujinet_log},
    {.name = "preferences", .activate = action_preferences},
    {.name = "keypad", .activate = action_keypad},
    {.name = "debugger", .activate = action_debugger},
    {.name = "about", .activate = action_about},
};

static void adam_window_class_init(AdamWindowClass *klass)
{
    (void)klass;
}

static void adam_window_init(AdamWindow *self)
{
    (void)self;
}

GtkWidget *adam_window_new(AdwApplication *app, adamsession *session)
{
    AdamWindow *self = g_object_new(ADAM_TYPE_WINDOW,
                                    "application", app,
                                    "title", "FujiNet Go Adam",
                                    "default-width", 1088,
                                    "default-height", 950,
                                    NULL);
    AdwToolbarView *view;
    AdwHeaderBar *header;
    GtkEventController *keys;
    GMenu *menu;

    self->session = session;

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);

    header = ADW_HEADER_BAR(adw_header_bar_new());
    self->menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(self->menu_button, "open-menu-symbolic");
    menu = build_menu();
    gtk_menu_button_set_menu_model(self->menu_button, G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(header, GTK_WIDGET(self->menu_button));

    self->display = ADAM_DISPLAY(adam_display_new(session));
    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->display));

    view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(view, GTK_WIDGET(header));
    adw_toolbar_view_set_content(view, GTK_WIDGET(self->toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(view));

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released),
                     self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);
    g_signal_connect(self, "notify::is-active", G_CALLBACK(on_active_changed),
                     self);

    apply_display_settings(self);
    gtk_widget_grab_focus(GTK_WIDGET(self->display));

    /* Developer affordances: open the debugger / the controllers window
     * alongside the main window (neither is shown unless asked for). */
    if (getenv("ADAM_OPEN_DEBUGGER"))
        adam_debugger_show(GTK_WINDOW(self), session);
    if (getenv("ADAM_OPEN_KEYPAD"))
        adam_keypad_window_toggle(GTK_WINDOW(self), session);
    return GTK_WIDGET(self);
}
