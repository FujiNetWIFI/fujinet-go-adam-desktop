/*
 * FujiNet Go Adam -- GNOME frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>

#include "adamsession.h"
#include "window.h"

static adamsession *g_session;

static void on_activate(AdwApplication *app, gpointer user_data)
{
    GtkWindow *win;
    (void)user_data;

    win = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (win) {
        gtk_window_present(win);
        return;
    }

    if (!g_session) {
        adamsession_start_opts opts;
        g_session = adamsession_new(NULL);
        if (!g_session) {
            g_printerr("fatal: could not create the session\n");
            g_application_quit(G_APPLICATION(app));
            return;
        }
        adamsession_default_opts(g_session, &opts);
        if (adamsession_start(g_session, &opts) != 0)
            g_printerr("session start: %s\n",
                       adamsession_last_error(g_session));
    }

    win = GTK_WINDOW(adam_window_new(app, g_session));
    gtk_window_set_icon_name(win, "online.fujinet.go.adam.gnome");
    gtk_window_present(win);
}

static void on_shutdown(GApplication *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    if (g_session) {
        adamsession_free(g_session);
        g_session = NULL;
    }
}

int main(int argc, char *argv[])
{
    g_autoptr(AdwApplication) app = adw_application_new(
        "online.fujinet.go.adam.gnome", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
