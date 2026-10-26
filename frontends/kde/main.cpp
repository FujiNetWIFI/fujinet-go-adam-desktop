/*
 * FujiNet Go Adam -- KDE/Qt frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>
#include <QSurfaceFormat>

#include <cstdio>

#include "FujiNetWindows.h"
#include "MainWindow.h"
#include "adamsession.h"

int main(int argc, char *argv[])
{
    /* Vsync'd swaps for the display widget; shared contexts for WebEngine. */
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("FujiNet Go Adam"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.adam.kde"));
#ifdef ADAM_ICON_FILE
    /* Dev-tree fallback; installed runs resolve the themed icon. */
    app.setWindowIcon(QIcon::fromTheme(
        QStringLiteral("online.fujinet.go.adam.kde"),
        QIcon(QStringLiteral(ADAM_ICON_FILE))));
#endif

    /* Toggled-on push buttons (Map, the keypad's held controls) wear the
     * accent colour with black text, light or dark. The rule spells out the
     * whole box: with only a background colour the style still draws its own
     * bevel, which darkens it (Breeze), lightens it (Fusion) or ignores it
     * (Kvantum). The box differs from the native button's, so a toggle in a
     * layout that must not jump needs a fixed size. */
    const QColor accent = adamAccentColor();
    app.setStyleSheet(
        QStringLiteral(
            "QPushButton:checked { background-color: %1; color: black;"
            " border: 1px solid %3; border-radius: 4px; padding: 4px 12px; }"
            "QPushButton:checked:hover { background-color: %2; }")
            .arg(accent.name(), accent.lighter(112).name(),
                 accent.darker(140).name()));

    adamsession *session = adamsession_new(nullptr);
    if (!session) {
        fprintf(stderr, "fatal: could not create the session\n");
        return 1;
    }
    adamsession_start_opts opts;
    adamsession_default_opts(session, &opts);
    if (adamsession_start(session, &opts) != 0)
        fprintf(stderr, "session start: %s\n",
                adamsession_last_error(session));

    int rc;
    {
        /* The windows go before the session does: the keypad window still
         * calls into it as it is hidden. */
        MainWindow win(session);
        win.show();
        rc = app.exec();
    }

    adamsession_free(session);
    return rc;
}
