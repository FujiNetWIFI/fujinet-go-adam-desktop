/*
 * ADAM/ColecoVision input encoding, ported bit-exactly from the Android
 * app's AdamInput.kt / HardwareKeyboard.kt (the unit tests mirror the
 * Kotlin ones). The controller word is active-low, idle 0x7F7F; keyboard
 * codes are ASCII for printable keys plus the EOS special codes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adamsession.h"

/* Direction / fire bits, cleared when pressed. */
#define JOY_UP      0x0100
#define JOY_RIGHT   0x0200
#define JOY_DOWN    0x0400
#define JOY_LEFT    0x0800
#define JOY_FIRE_L  0x4000
#define JOY_FIRE_R  0x0040
#define KEYPAD_NONE 0x0F

uint16_t adam_controller_encode(int up, int down, int left, int right,
                                int fire_left, int fire_right, int keypad)
{
    unsigned s = 0x7F7F;
    if (up) s &= ~JOY_UP;
    if (down) s &= ~JOY_DOWN;
    if (left) s &= ~JOY_LEFT;
    if (right) s &= ~JOY_RIGHT;
    if (fire_left) s &= ~JOY_FIRE_L;
    if (fire_right) s &= ~JOY_FIRE_R;
    s = (s & 0xFFF0) | ((keypad >= 0 && keypad <= 13) ? (unsigned)keypad
                                                      : KEYPAD_NONE);
    return (uint16_t)s;
}

/* ADAM keyboard byte codes (AdamKeys in the Android app). */
#define ADAM_RETURN    0x0D
#define ADAM_BACKSPACE 0x08
#define ADAM_ESCAPE    0x1B
#define ADAM_TAB       0x09
#define ADAM_SMART_I   0x81 /* ..0x86 = SmartKeys I-VI */
#define ADAM_UNDO      0x91
#define ADAM_INSERT    0x94
#define ADAM_DELETE    0x97
#define ADAM_CUR_UP    0xA0
#define ADAM_CUR_RIGHT 0xA1
#define ADAM_CUR_DOWN  0xA2
#define ADAM_CUR_LEFT  0xA3
#define ADAM_HOME      0xA8

/* X11/xkb keysyms (== GDK keyvals) for the non-printable keys we map. */
#define XKS_BackSpace    0xFF08
#define XKS_Tab          0xFF09
#define XKS_Return       0xFF0D
#define XKS_Escape       0xFF1B
#define XKS_Home         0xFF50
#define XKS_Left         0xFF51
#define XKS_Up           0xFF52
#define XKS_Right        0xFF53
#define XKS_Down         0xFF54
#define XKS_Insert       0xFF63
#define XKS_Undo         0xFF65
#define XKS_KP_Enter     0xFF8D
#define XKS_KP_Multiply  0xFFAA
#define XKS_KP_Add       0xFFAB
#define XKS_KP_Subtract  0xFFAD
#define XKS_KP_Decimal   0xFFAE
#define XKS_KP_Divide    0xFFAF
#define XKS_KP_0         0xFFB0 /* ..0xFFB9 */
#define XKS_F1           0xFFBE /* ..F6 = 0xFFC3 */
#define XKS_Delete       0xFFFF

int adam_key_from_event(uint32_t keysym, uint32_t unicode, int ctrl_down)
{
    switch (keysym) {
    case XKS_Return:
    case XKS_KP_Enter:    return ADAM_RETURN;
    case XKS_Escape:      return ADAM_ESCAPE;
    case XKS_Tab:         return ADAM_TAB;
    case XKS_BackSpace:   return ADAM_BACKSPACE;
    case XKS_Delete:      return ADAM_DELETE;
    case XKS_Insert:      return ADAM_INSERT;
    case XKS_Undo:        return ADAM_UNDO;
    case XKS_Home:        return ADAM_HOME;
    case XKS_Up:          return ADAM_CUR_UP;
    case XKS_Right:       return ADAM_CUR_RIGHT;
    case XKS_Down:        return ADAM_CUR_DOWN;
    case XKS_Left:        return ADAM_CUR_LEFT;
    case XKS_KP_Multiply: return '*';
    case XKS_KP_Add:      return '+';
    case XKS_KP_Subtract: return '-';
    case XKS_KP_Decimal:  return '.';
    case XKS_KP_Divide:   return '/';
    default: break;
    }

    if (keysym >= XKS_F1 && keysym <= XKS_F1 + 5) /* F1-F6 -> SmartKeys I-VI */
        return ADAM_SMART_I + (int)(keysym - XKS_F1);
    if (keysym >= XKS_KP_0 && keysym <= XKS_KP_0 + 9)
        return '0' + (int)(keysym - XKS_KP_0);

    /* Ctrl + letter -> ASCII control code (Ctrl-C = 0x03, ...). Letter
     * keysyms are ASCII; normalize a shifted (uppercase) one. */
    if (ctrl_down) {
        uint32_t base = keysym;
        if (base >= 'A' && base <= 'Z') base += 0x20;
        if (base >= 'a' && base <= 'z')
            return (int)(base - 'a' + 1);
    }

    /* Printable key: the ADAM code is the typed (possibly shifted) ASCII. */
    if (unicode >= 0x20 && unicode <= 0x7E)
        return (int)unicode;
    return -1;
}
