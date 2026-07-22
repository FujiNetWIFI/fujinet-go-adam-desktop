/*
 * DisplayWidget: paints the latest emulator frame with QPainter over a
 * QOpenGLWidget, letterboxed to the chosen aspect. The frameSwapped signal
 * (vsync-aligned with the default swap interval of 1) both feeds the
 * session's vsync phase-lock and schedules the next repaint, forming a
 * self-sustaining vsync-paced present loop. Keyboard events are translated
 * to ADAM key bytes here; ShortcutOverride is accepted for everything except
 * F10/F11/F12 so menu accelerators cannot steal emulator keys while the
 * display has focus.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DisplayWidget.h"

#include <QKeyEvent>
#include <QPainter>

#include <ctime>

namespace {

qint64 monotonic_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (qint64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Map the Qt key event onto the X keysym + unicode contract of
 * adam_key_from_event so both frontends share one mapping table. */
int adamCodeForKey(const QKeyEvent *event)
{
    quint32 keysym = 0;
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:     keysym = 0xFF0D; break;
    case Qt::Key_Escape:    keysym = 0xFF1B; break;
    case Qt::Key_Tab:       keysym = 0xFF09; break;
    case Qt::Key_Backspace: keysym = 0xFF08; break;
    case Qt::Key_Delete:    keysym = 0xFFFF; break;
    case Qt::Key_Insert:    keysym = 0xFF63; break;
    case Qt::Key_Undo:      keysym = 0xFF65; break;
    case Qt::Key_Home:      keysym = 0xFF50; break;
    case Qt::Key_Left:      keysym = 0xFF51; break;
    case Qt::Key_Up:        keysym = 0xFF52; break;
    case Qt::Key_Right:     keysym = 0xFF53; break;
    case Qt::Key_Down:      keysym = 0xFF54; break;
    default:
        if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F6)
            keysym = 0xFFBE + (quint32)(event->key() - Qt::Key_F1);
        break;
    }

    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    quint32 unicode = 0;
    if (!event->text().isEmpty())
        unicode = event->text().at(0).unicode();

    if (keysym == 0) {
        if (ctrl && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z)
            keysym = 'a' + (quint32)(event->key() - Qt::Key_A);
        else
            keysym = unicode;
    }
    return adam_key_from_event(keysym, unicode, ctrl ? 1 : 0);
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
    /* Claim every key except the three reserved chords while focused, so
     * menu accelerators can't shadow what the ADAM should receive. */
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() != Qt::Key_F10 && ke->key() != Qt::Key_F11 &&
            ke->key() != Qt::Key_F12) {
            event->accept();
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

void DisplayWidget::keyPressEvent(QKeyEvent *event)
{
    /* Ctrl+digit presses the game-controller keypad (game select on
     * cartridges and tape games); released in keyReleaseEvent. */
    if (event->modifiers().testFlag(Qt::ControlModifier) &&
        event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9) {
        adamsession_joystick_raw(
            m_session, 0,
            adam_controller_encode(0, 0, 0, 0, 0, 0,
                                   event->key() - Qt::Key_0));
        event->accept();
        return;
    }

    const int code = adamCodeForKey(event);
    if (code >= 0) {
        adamsession_key(m_session, (uint8_t)code);
        event->accept();
        return;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void DisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() >= Qt::Key_0 && event->key() <= Qt::Key_9) {
        adamsession_joystick_raw(
            m_session, 0, adam_controller_encode(0, 0, 0, 0, 0, 0, -1));
        event->accept();
        return;
    }
    QOpenGLWidget::keyReleaseEvent(event);
}
