/*
 * DisplayWidget: paints the latest emulator frame with QPainter over a
 * QOpenGLWidget, letterboxed to the chosen aspect. The frameSwapped signal
 * (vsync-aligned with the default swap interval of 1) both feeds the
 * session's vsync phase-lock and schedules the next repaint, forming a
 * self-sustaining vsync-paced present loop. Keyboard events go through
 * adamForwardKey (controller bindings first, then the ADAM keyboard);
 * ShortcutOverride is accepted for everything except F9/F10/F11/F12 so menu
 * accelerators cannot steal emulator keys while the display has focus.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QPainter>

#include <ctime>

#include "KeyForward.h"

namespace {

qint64 monotonic_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (qint64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

} // namespace

DisplayWidget::DisplayWidget(adamsession *session, QWidget *parent)
    : QOpenGLWidget(parent),
      m_session(session),
      m_image(ADAMSESSION_FB_WIDTH, ADAMSESSION_FB_HEIGHT,
              QImage::Format_RGB16)
{
    m_image.fill(Qt::black);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(ADAMSESSION_FB_WIDTH, ADAMSESSION_FB_HEIGHT);
    connect(this, &QOpenGLWidget::frameSwapped, this,
            &DisplayWidget::onFrameSwapped, Qt::QueuedConnection);
}

void DisplayWidget::setAspectMode(int mode)
{
    m_aspectMode = mode;
    update();
}

void DisplayWidget::setSmooth(bool smooth)
{
    m_smooth = smooth;
    update();
}

void DisplayWidget::onFrameSwapped()
{
    adamsession_notify_vsync(m_session, monotonic_ns());
    update();
}

void DisplayWidget::showEvent(QShowEvent *event)
{
    QOpenGLWidget::showEvent(event);
    update(); /* kick the frameSwapped/update loop */
}

QRectF DisplayWidget::destRect() const
{
    const qreal w = width(), h = height();
    qreal dw, dh;

    if (m_aspectMode == AspectInteger) {
        int scale = (int)qMin(w / ADAMSESSION_FB_WIDTH,
                              h / ADAMSESSION_FB_HEIGHT);
        if (scale < 1) scale = 1;
        dw = scale * ADAMSESSION_FB_WIDTH;
        dh = scale * ADAMSESSION_FB_HEIGHT;
    } else {
        const qreal aspect = m_aspectMode == AspectTv43
                                 ? 4.0 / 3.0
                                 : (qreal)ADAMSESSION_FB_WIDTH /
                                       ADAMSESSION_FB_HEIGHT;
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

void DisplayWidget::paintGL()
{
    uint64_t serial = m_serial;
    if (adamsession_copy_frame(m_session, (uint16_t *)m_image.bits(), &serial))
        m_serial = serial;

    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
    p.drawImage(destRect(), m_image);
}

bool DisplayWidget::event(QEvent *event)
{
    /* Claim every key except the four reserved chords while focused, so
     * menu accelerators can't shadow what the ADAM should receive. */
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() != Qt::Key_F9 && ke->key() != Qt::Key_F10 &&
            ke->key() != Qt::Key_F11 && ke->key() != Qt::Key_F12) {
            event->accept();
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

/* Tab belongs to the ADAM keyboard (and to Map mode), not to focus
 * traversal. */
bool DisplayWidget::focusNextPrevChild(bool)
{
    return false;
}

void DisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (adamForwardKey(m_session, event, true)) {
        event->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void DisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (adamForwardKey(m_session, event, false)) {
        event->accept();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(event);
}

/* A key released while another window had focus never arrives here. */
void DisplayWidget::focusOutEvent(QFocusEvent *event)
{
    adamsession_release_all(m_session);
    QOpenGLWidget::focusOutEvent(event);
}
