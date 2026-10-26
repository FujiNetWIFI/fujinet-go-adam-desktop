/*
 * Keyboard path shared by the main window and the keypad window.
 *
 * The EOS special keys (arrows, F1-F6, Home, Insert, Delete) come via
 * WM_KEYDOWN; printable characters and control codes (incl. Ctrl+letter)
 * arrive already translated as WM_CHAR. The two sets do not overlap.
 * Every key press and release goes to adamsession_input_key first: a key
 * bound to a controller target drives it and is NOT also typed, and since
 * TranslateMessage has already queued the WM_CHAR such a key would produce,
 * adam_win_char drops it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keys.h"

static int g_swallow_char;   /* the WM_CHAR after a consumed key is dropped */

static int special_keysym(WPARAM vk)
{
    switch (vk) {
    case VK_UP:     return 0xFF52;
    case VK_DOWN:   return 0xFF54;
    case VK_LEFT:   return 0xFF51;
    case VK_RIGHT:  return 0xFF53;
    case VK_HOME:   return 0xFF50;
    case VK_INSERT: return 0xFF63;
    case VK_DELETE: return 0xFFFF;
    default:
        if (vk >= VK_F1 && vk <= VK_F6)
            return 0xFFBE + (int)(vk - VK_F1);
        return 0;
    }
}

uint32_t adam_win_keysym(WPARAM vk, LPARAM lp)
{
    uint32_t ks;
    /* Windows reports Pause as scan code 0x45 and Num Lock as extended 0x45:
     * the core's table cannot tell them apart, the VK can. */
    if (vk == VK_PAUSE)
        return 0xFF13;
    if (vk == VK_NUMLOCK)
        return 0xFF7F;
    ks = adamsession_keysym_from_win_scancode((unsigned)((lp >> 16) & 0xFF),
                                              (int)((lp >> 24) & 1));
    return ks ? ks : (uint32_t)special_keysym(vk);
}

int adam_win_key_msg(adamsession *session, UINT msg, WPARAM wp, LPARAM lp)
{
    const int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const uint32_t ks = adam_win_keysym(wp, lp);
    int consumed = 0;

    /* Alt+F4 closes the window, whatever F4 would otherwise do */
    if (down && wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
        return 0;

    if (ks && adamsession_input_key(session, ks, down, ctrl))
        consumed = 1;
    if (down)
        g_swallow_char = consumed;
    if (consumed || !down)
        return consumed;

    if (special_keysym(wp)) {
        int code = adam_key_from_event((uint32_t)special_keysym(wp), 0, 0);
        if (code >= 0)
            adamsession_key(session, (uint8_t)code);
        return 1;
    }
    return 0;
}

void adam_win_char(adamsession *session, WPARAM ch)
{
    if (g_swallow_char)
        return;
    if (ch >= 1 && ch <= 0x7E)
        adamsession_key(session, (uint8_t)ch);
}
