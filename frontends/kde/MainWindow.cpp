/*
 * MainWindow: menu bar over the emulator display; F10 opens the menu, F11
 * toggles fullscreen, F12 opens the debugger. No on-screen input panels
 * appear unless the user explicitly asks.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QFileDialog>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

#include <cstdlib>

#include "DisplayWidget.h"
#include "FujiNetWindows.h"
#include "SettingsDialog.h"
#include "debugger/DebuggerWindow.h"

MainWindow::MainWindow(adamsession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Adam"));
    resize(1088, 950);

    m_display = new DisplayWidget(session, this);
    setCentralWidget(m_display);
    buildMenus();
    applyDisplaySettings();
    m_display->setFocus();

    /* Developer affordance: open the debugger alongside the main window. */
    if (getenv("ADAM_OPEN_DEBUGGER"))
        DebuggerWindow::showFor(this, m_session);
}

void MainWindow::applyDisplaySettings()
{
    m_display->setAspectMode(
        adamsession_get_int(m_session, "aspect_mode", 0));
    m_display->setSmooth(
        adamsession_get_int(m_session, "smooth_scaling", 0) != 0);
}

void MainWindow::restartSession()
{
    adamsession_start_opts opts;
    adamsession_stop(m_session);
    adamsession_default_opts(m_session, &opts);
    if (adamsession_start(m_session, &opts) != 0)
        statusBar()->showMessage(
            QString::fromUtf8(adamsession_last_error(m_session)), 6000);
}

void MainWindow::buildMenus()
{
    QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    machine->addAction(QStringLiteral("Reset Computer (ADAM)"), this, [this] {
        adamsession_reset(m_session, 0);
        statusBar()->showMessage(QStringLiteral("Computer reset"), 3000);
    });
    machine->addAction(QStringLiteral("Reset Game (ColecoVision)"), this,
                       [this] {
                           adamsession_reset(m_session, 1);
                           statusBar()->showMessage(
                               QStringLiteral("Game reset"), 3000);
                       });
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Preferences…"), this,
                       &MainWindow::showPreferences);

    QMenu *media = menuBar()->addMenu(QStringLiteral("M&edia"));
    media->addAction(QStringLiteral("Import Disk or Data Pack…"), this,
                     &MainWindow::importMedia);
    media->addAction(QStringLiteral("Load Cartridge…"), this,
                     &MainWindow::loadCartridge);
    media->addAction(QStringLiteral("Eject Cartridge"), this,
                     &MainWindow::ejectCartridge);

    QMenu *fujinet = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fujinet->addAction(QStringLiteral("Configuration…"), this,
                       &MainWindow::showFujiNetConfig);
    fujinet->addAction(QStringLiteral("Console Log…"), this,
                       &MainWindow::showFujiNetLog);

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("Fullscreen"), QKeySequence(Qt::Key_F11),
                    this, &MainWindow::toggleFullscreen);
    view->addAction(QStringLiteral("Debugger"), QKeySequence(Qt::Key_F12),
                    this,
                    [this] { DebuggerWindow::showFor(this, m_session); });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("About FujiNet Go Adam"), this,
                    &MainWindow::showAbout);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_F10:
        menuBar()->setFocus();
        return;
    case Qt::Key_F11:
        toggleFullscreen();
        return;
    default:
        QMainWindow::keyPressEvent(event);
    }
}

void MainWindow::toggleFullscreen()
{
    if (isFullScreen()) {
        showNormal();
        menuBar()->show();
    } else {
        menuBar()->hide();
        showFullScreen();
    }
}

void MainWindow::importMedia()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Disk or Data Pack"), QString(),
        QStringLiteral("ADAM disk / data pack images (*.dsk *.ddp)"));
    if (path.isEmpty())
        return;
    char dest[1024];
    if (adamsession_import_media(m_session, path.toUtf8().constData(), dest,
                                 sizeof(dest)) != 0) {
        statusBar()->showMessage(
            QString::fromUtf8(adamsession_last_error(m_session)), 6000);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Copied to FujiNet SD: %1")
            .arg(QString::fromUtf8(dest).section(QLatin1Char('/'), -1)),
        6000);
}

void MainWindow::loadCartridge()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load Cartridge"), QString(),
        QStringLiteral("ColecoVision cartridges (*.rom *.col *.bin)"));
    if (path.isEmpty())
        return;
    char dest[1024];
    if (adamsession_import_media(m_session, path.toUtf8().constData(), dest,
                                 sizeof(dest)) != 0) {
        statusBar()->showMessage(
            QString::fromUtf8(adamsession_last_error(m_session)), 6000);
        return;
    }
    adamsession_set_str(m_session, "cart_path", dest);
    adamsession_set_int(m_session, "machine", 1);
    restartSession();
    statusBar()->showMessage(QStringLiteral("Cartridge loaded"), 4000);
}

void MainWindow::ejectCartridge()
{
    adamsession_set_str(m_session, "cart_path", "");
    adamsession_set_int(m_session, "machine", 0);
    restartSession();
    statusBar()->showMessage(
        QStringLiteral("Cartridge ejected; back to ADAM"), 4000);
}

void MainWindow::showPreferences()
{
    SettingsDialog dialog(m_session, this);
    const bool machineChanged = dialog.exec() == QDialog::Accepted &&
                                dialog.machineOptionsChanged();
    applyDisplaySettings();
    if (machineChanged) {
        restartSession();
        statusBar()->showMessage(
            QStringLiteral("Machine options applied (session restarted)"),
            4000);
    }
}

void MainWindow::showFujiNetConfig()
{
    fujinet_config_show(this, m_session);
}

void MainWindow::showFujiNetLog()
{
    fujinet_log_show(this, m_session);
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this, QStringLiteral("About FujiNet Go Adam"),
        QStringLiteral(
            "<b>FujiNet Go Adam</b> 0.1.0<br>"
            "Self-contained Coleco ADAM with built-in FujiNet.<br>"
            "Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later<br>"
            "<a href=\"https://fujinet.online/\">fujinet.online</a>"));
}
