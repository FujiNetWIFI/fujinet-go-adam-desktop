/*
 * Where the picture goes inside the display, shared by the GPU and the
 * software display: integer scale, 4:3 or square pixels (256:212), centred.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QEvent>
#include <QKeyEvent>
#include <QRectF>
#include <QSizeF>
#include <QtGlobal>

#include "DisplayView.h"
#include "adamsession.h"

inline QRectF displayRect(QSizeF avail, int mode)
{
    const qreal w = avail.width(), h = avail.height();
    qreal dw, dh;

    if (mode == DisplayView::AspectInteger) {
        int scale = (int)qMin(w / ADAMSESSION_FB_WIDTH, h / ADAMSESSION_FB_HEIGHT);
        if (scale < 1) scale = 1;
        dw = scale * ADAMSESSION_FB_WIDTH;
        dh = scale * ADAMSESSION_FB_HEIGHT;
    } else {
        const qreal aspect = mode == DisplayView::AspectTv43
                                 ? 4.0 / 3.0
                                 : (qreal)ADAMSESSION_FB_WIDTH / ADAMSESSION_FB_HEIGHT;
        if (w / h > aspect) {
            dh = h;
            dw = h * aspect;
        } else {
            dw = w;
            dh = w / aspect;
        }
    }
    return QRectF((w - dw) / 2.0, (h - dh) / 2.0, dw, dh);
}

/* Claim every key except the four reserved chords while focused, so menu
 * accelerators can't shadow what the ADAM should receive. */
inline bool claimShortcut(QEvent *event)
{
    if (event->type() != QEvent::ShortcutOverride)
        return false;
    const int k = static_cast<QKeyEvent *>(event)->key();
    if (k == Qt::Key_F9 || k == Qt::Key_F10 || k == Qt::Key_F11 || k == Qt::Key_F12)
        return false;
    event->accept();
    return true;
}
