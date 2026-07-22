/*
 * MainWindow: the main window of the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>

#include "adamsession.h"

class DisplayWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(adamsession *session, QWidget *parent = nullptr);

    /* Stop and relaunch the session with the current persisted settings. */
    void restartSession();
    void applyDisplaySettings();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    void buildMenus();
    void importMedia();
    void loadCartridge();
    void ejectCartridge();
    void toggleFullscreen();
    void showPreferences();
    void showFujiNetConfig();
    void showFujiNetLog();
    void showAbout();

    adamsession *m_session;
    DisplayWidget *m_display;
};
