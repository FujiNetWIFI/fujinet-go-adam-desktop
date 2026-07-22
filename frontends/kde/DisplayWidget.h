/*
 * DisplayWidget: the emulator video widget for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QImage>
#include <QOpenGLWidget>

#include "adamsession.h"

class DisplayWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    enum AspectMode {
        AspectSquarePixels = 0, /* 256:212, matches the Android app */
        AspectTv43 = 1,
        AspectInteger = 2,
    };

    explicit DisplayWidget(adamsession *session, QWidget *parent = nullptr);

    void setAspectMode(int mode);
    void setSmooth(bool smooth);

signals:
    void adamKey(int code);

protected:
    void paintGL() override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void onFrameSwapped();
    QRectF destRect() const;

    adamsession *m_session;
    /* QImage::Format_RGB16 is RGB565 -- the frame copies straight in. */
    QImage m_image;
    quint64 m_serial = 0;
    int m_aspectMode = AspectSquarePixels;
    bool m_smooth = false;
};
