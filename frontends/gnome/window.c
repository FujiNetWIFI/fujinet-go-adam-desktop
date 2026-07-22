/*
 * AdamWindow: main window of the GNOME frontend. Header bar + menu over the
 * emulator display; keyboard capture routes everything except F10 (menu),
 * F11 (fullscreen) and F12 (debugger) to the ADAM. No on-screen input
 * panels are shown unless the user asks for them.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include <stdlib.h>

#include "debugger/dbg_window.h"
#include "display.h"
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

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    int code;
    (void)controller;
    (void)keycode;

    switch (keyval) {
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

    /* Leave Alt combos to mnemonics/WM; the ADAM keyboard has no Alt. */
    if (state & GDK_ALT_MASK)
        return FALSE;

    /* Ctrl+digit presses the game-controller keypad (game select on
     * cartridges and tape games); released in on_key_released. */
    if ((state & GDK_CONTROL_MASK) && keyval >= GDK_KEY_0 &&
        keyval <= GDK_KEY_9) {
        adamsession_joystick_raw(
            self->session, 0,
            adam_controller_encode(0, 0, 0, 0, 0, 0,
                                   (int)(keyval - GDK_KEY_0)));
        return TRUE;
    }

    code = adam_key_from_event(keyval, gdk_keyval_to_unicode(keyval),
                               (state & GDK_CONTROL_MASK) != 0);
    if (code < 0)
        return FALSE;
    adamsession_key(self->session, (uint8_t)code);
    return TRUE;
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)controller;
    (void)keycode;
    (void)state;
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9)
        adamsession_joystick_raw(self->session, 0,
                                 adam_controller_encode(0, 0, 0, 0, 0, 0, -1));
}

/* ---- actions ------------------------------------------------------------ */

static void action_reset_adam(GSimpleAction *action, GVariant *param,
                              gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adamsession_reset(self->session, 0);
    adam_window_toast(self, "Computer reset");
}

static void action_reset_cv(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adamsession_reset(self->session, 1);
    adam_window_toast(self, "Game reset");
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

static void action_debugger(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    AdamWindow *self = ADAM_WINDOW(user_data);
    (void)action;
    (void)param;
    adam_debugger_show(GTK_WINDOW(self), self->session);
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
        "application-icon", "online.fujinet.go.adam.gnome",
        "developer-name", "Thomas Cherryhomes",
        "version", "0.1.0",
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

    apply_display_settings(self);
    gtk_widget_grab_focus(GTK_WIDGET(self->display));

    /* Developer affordance: open the debugger alongside the main window. */
    if (getenv("ADAM_OPEN_DEBUGGER"))
        adam_debugger_show(GTK_WINDOW(self), session);
    return GTK_WIDGET(self);
}
