/*
 * KeyForward -- see KeyForward.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeyForward.h"

uint32_t adamKeysymFromQt(const QKeyEvent *event)
{
    /* On X11 and Wayland Qt reports the X keycode: the evdev code plus 8. */
    const quint32 sc = event->nativeScanCode();
    if (sc >= 8) {
        const uint32_t ks = adamsession_keysym_from_evdev(sc - 8);
        if (ks)
            return ks;
    }

    const int k = event->key();
    const bool pad = event->modifiers().testFlag(Qt::KeypadModifier);
    switch (k) {
    case Qt::Key_Up:        return ADAMSESSION_KEYSYM_UP;
    case Qt::Key_Down:      return ADAMSESSION_KEYSYM_DOWN;
    case Qt::Key_Left:      return ADAMSESSION_KEYSYM_LEFT;
    case Qt::Key_Right:     return ADAMSESSION_KEYSYM_RIGHT;
    case Qt::Key_Tab:       return ADAMSESSION_KEYSYM_TAB;
    case Qt::Key_Escape:    return ADAMSESSION_KEYSYM_ESCAPE;
    case Qt::Key_Return:    return ADAMSESSION_KEYSYM_RETURN;
    case Qt::Key_Enter:     return ADAMSESSION_KEYSYM_KP_ENTER;
    case Qt::Key_Backspace: return ADAMSESSION_KEYSYM_BACKSPACE;
    case Qt::Key_Space:     return ADAMSESSION_KEYSYM_SPACE;
    case Qt::Key_Shift:     return ADAMSESSION_KEYSYM_LSHIFT;
    case Qt::Key_Control:   return ADAMSESSION_KEYSYM_LCTRL;
    case Qt::Key_Alt:       return ADAMSESSION_KEYSYM_LALT;
    case Qt::Key_Delete:    return 0xFFFF;
    case Qt::Key_Insert:    return 0xFF63;
    case Qt::Key_Home:      return 0xFF50;
    case Qt::Key_End:       return 0xFF57;
    case Qt::Key_PageUp:    return 0xFF55;
    case Qt::Key_PageDown:  return 0xFF56;
    default: break;
    }
    if (k >= Qt::Key_F1 && k <= Qt::Key_F12)
        return ADAMSESSION_KEYSYM_F1 + (uint32_t)(k - Qt::Key_F1);
    if (k >= Qt::Key_0 && k <= Qt::Key_9)
        return (pad ? (uint32_t)ADAMSESSION_KEYSYM_KP_0 : (uint32_t)'0') +
               (uint32_t)(k - Qt::Key_0);
    if (k >= Qt::Key_A && k <= Qt::Key_Z)
        return (uint32_t)('a' + (k - Qt::Key_A));
    if (pad && k == Qt::Key_Asterisk) return ADAMSESSION_KEYSYM_KP_MULTIPLY;
    if (pad && k == Qt::Key_Slash)    return ADAMSESSION_KEYSYM_KP_DIVIDE;
    if (k >= 0x20 && k <= 0x7e)
        return (uint32_t)k;
    return 0;
}

namespace {

/* Map the Qt key event onto the X keysym + unicode contract of
 * adam_key_from_event so every frontend shares one mapping table. */
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

bool adamForwardKey(adamsession *session, QKeyEvent *event, bool down)
{
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const uint32_t ks = adamKeysymFromQt(event);

    /* An auto-repeat release is Qt's synthetic half of a repeat pair; passing
     * it on would let go of a held stick direction for an instant. */
    if (!down) {
        if (event->isAutoRepeat())
            return false;
        return ks && adamsession_input_key(session, ks, 0, ctrl ? 1 : 0);
    }

    /* The session ignores a repeat of a held key, so this is safe to call
     * for repeats too; an unbound key still types repeatedly below. */
    if (ks && adamsession_input_key(session, ks, 1, ctrl ? 1 : 0))
        return true;

    const int code = adamCodeForKey(event);
    if (code < 0)
        return false;
    adamsession_key(session, (uint8_t)code);
    return true;
}
