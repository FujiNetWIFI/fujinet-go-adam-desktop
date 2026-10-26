/*
 * The Controllers window: both ColecoVision controllers on screen (keypad,
 * stick, Fire L / Fire R) that press the machine's controls and light up
 * with whatever the keyboard, a gamepad or the mouse is holding, the reset
 * buttons, and Map mode for rebinding any control to a key or a gamepad
 * button. A fixed-size floating window, hidden until asked for.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <vector>

#include "adamsession.h"

/* A button that reports press and release, not "clicked": a controller
 * control is HELD, since the machine samples it once per frame. Its checked
 * state is the highlight (held, or picked in Map mode), never a toggle, so
 * the app-wide accent style for checked push buttons lights it. */
class PadButton : public QPushButton {
    Q_OBJECT
public:
    PadButton(const QString &face, int target, QWidget *parent = nullptr);
    int target() const { return m_target; }
    QString face() const { return m_face; }
    void setHeld(bool held) { setChecked(held); }
signals:
    void pressedTarget(int target);
    /* inside = the pointer was still over the button when it let go. */
    void releasedTarget(int target, bool inside);
protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
    void nextCheckState() override {}
private:
    int m_target;
    QString m_face;
    bool m_down = false;
};

class KeypadWindow : public QWidget {
    Q_OBJECT
public:
    explicit KeypadWindow(adamsession *session, QWidget *parent = nullptr);

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void changeEvent(QEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    QWidget *buildController(int port);
    PadButton *control(const QString &face, int target);
    void onPressed(int target);
    void onReleased(int target, bool inside);
    void setMapState(int state);
    void refreshLabels();
    void setBound(int target, const QString &what, const char *stolen);
    void poll();

    adamsession *m_session;
    std::vector<PadButton *> m_controls;
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QLabel *m_hint = nullptr;
    QTimer m_pollTimer;
    int m_mouseHeld = -1;   /* the target the mouse is holding, if any */
    /* -2 idle, -1 armed and waiting for a target, >=0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};
