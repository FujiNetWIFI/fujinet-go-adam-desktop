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

    MainWindow win(session);
    win.show();
    const int rc = app.exec();

    adamsession_free(session);
    return rc;
}
