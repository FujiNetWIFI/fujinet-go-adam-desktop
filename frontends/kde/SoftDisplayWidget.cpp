/*
 * SoftDisplayWidget -- see SoftDisplayWidget.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SoftDisplayWidget.h"

#include "DisplayGeometry.h"
#include "FrameConvert.h"
#include "KeyForward.h"

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QPainter>

SoftDisplayWidget::SoftDisplayWidget(adamsession *session, QWidget *parent)
    : QWidget(parent), m_session(session), m_feed(new FrameFeed(session, this)),
      m_image(ADAMSESSION_FB_WIDTH, ADAMSESSION_FB_HEIGHT, QImage::Format_RGB32)
{
    m_image.fill(Qt::black);
    m_fb.resize((size_t)ADAMSESSION_FB_WIDTH * ADAMSESSION_FB_HEIGHT);
    setMinimumSize(ADAMSESSION_FB_WIDTH, ADAMSESSION_FB_HEIGHT);
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(false);
    connect(m_feed, &FrameFeed::frameReady, this, &SoftDisplayWidget::onFrame);
}

SoftDisplayWidget::~SoftDisplayWidget()
{
    m_feed->setActive(false);
}

void SoftDisplayWidget::setAspectMode(int mode) { m_aspectMode = mode; update(); }
void SoftDisplayWidget::setSmooth(bool smooth) { m_smooth = smooth; update(); }

void SoftDisplayWidget::updateActive()
{
    m_feed->setActive(isVisible() && window() && !window()->isMinimized());
}

void SoftDisplayWidget::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (window())
        window()->installEventFilter(this);
    updateActive();
}

void SoftDisplayWidget::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (window())
        window()->removeEventFilter(this);
    updateActive();
}

bool SoftDisplayWidget::eventFilter(QObject *watched, QEvent *e)
{
    if (e->type() == QEvent::WindowStateChange || e->type() == QEvent::Show ||
        e->type() == QEvent::Hide)
        updateActive();
    return QWidget::eventFilter(watched, e);
}

bool SoftDisplayWidget::event(QEvent *e)
{
    if (claimShortcut(e))
        return true;
    return QWidget::event(e);
}

void SoftDisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (adamForwardKey(m_session, event, true)) {
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SoftDisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (adamForwardKey(m_session, event, false)) {
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

/* A key released while another window had focus never arrives here. */
void SoftDisplayWidget::focusOutEvent(QFocusEvent *event)
{
    adamsession_release_all(m_session);
    QWidget::focusOutEvent(event);
}

void SoftDisplayWidget::onFrame()
{
    if (!adamsession_copy_frame(m_session, m_fb.data(), &m_serial))
        return;
    rgb565ToXrgb(m_fb.data(), reinterpret_cast<quint32 *>(m_image.bits()), m_fb.size());
    update();
}

void SoftDisplayWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);

    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(displayRect(QSizeF(size()), m_aspectMode), m_image);
}
