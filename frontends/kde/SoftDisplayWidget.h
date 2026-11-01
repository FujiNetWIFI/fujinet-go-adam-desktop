/*
 * The software display: the fallback for a machine with no usable GPU
 * device (or QT_QUICK_BACKEND=software style setups). A QWidget that scales
 * with QPainter, fed by FrameFeed like the GPU one.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QWidget>
#include <cstdint>
#include <vector>

#include "DisplayView.h"
#include "FrameFeed.h"
#include "adamsession.h"

class SoftDisplayWidget : public QWidget, public DisplayView {
    Q_OBJECT
public:
    explicit SoftDisplayWidget(adamsession *session, QWidget *parent = nullptr);
    ~SoftDisplayWidget() override;

    QWidget *widget() override { return this; }
    void setAspectMode(int mode) override;
    void setSmooth(bool smooth) override;

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool event(QEvent *e) override;
    bool focusNextPrevChild(bool) override { return false; }

private:
    void onFrame();
    void updateActive();

    adamsession *m_session;
    FrameFeed *m_feed;
    std::vector<uint16_t> m_fb;   /* RGB565 straight from the session */
    QImage m_image;            /* Format_RGB32, FB_WIDTH x FB_HEIGHT */
    uint64_t m_serial = 0;
    int m_aspectMode = AspectSquarePixels;
    bool m_smooth = false;
};
