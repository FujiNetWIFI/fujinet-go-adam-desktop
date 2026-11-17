/*
 * MainWindow: the main window of the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCloseEvent>
#include <QMainWindow>
#include <QTimer>

#include "adamsession.h"

class DisplayView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(adamsession *session, QWidget *parent = nullptr);

    /* Stop and relaunch the session with the current persisted settings. */
    void restartSession();
    void applyDisplaySettings();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void buildMenus();
    void useSoftwareDisplay();
    void importMedia();
    void loadCartridge();
    void ejectCartridge();
    void toggleFullscreen();
    void toggleControllers();
    /* the news the gamepad thread leaves: console actions and hot-plugs */
    void tick();
    void runSysaction(int sysact);
    void showPreferences();
    void showFujiNetConfig();
    void showFujiNetLog();
    void showAbout();

    adamsession *m_session;
    DisplayView *m_display;
    QTimer m_tickTimer;
    unsigned m_padGen = 0;
};
