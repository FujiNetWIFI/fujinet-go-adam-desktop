/*
 * A toggle button in the app's accent colour (the icon's light violet, from
 * ADAMSESSION_ACCENT_RGB) with BLACK text while it is on, the stock look
 * while it is off -- black because the accent is light. A real BUTTON control
 * gets it through its own NM_CUSTOMDRAW (comctl32 v6, see app.manifest.in),
 * so an off button keeps the themed look and its focus, hover and press
 * behaviour, and a check state stays the button's own (BM_GETCHECK): only the
 * "on" paint is replaced. A window that draws its own buttons (the keypad)
 * takes adam_accent_brush() and ADAM_ACCENT_TEXT instead.
 *
 * Every checkable push button in the app (BS_AUTOCHECKBOX | BS_PUSHLIKE)
 * handles NM_CUSTOMDRAW in its parent's WM_NOTIFY and returns
 *   adam_accent_customdraw((const NMCUSTOMDRAW *)lp, checked);
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAM_WIN_ACCENT_BUTTON_H
#define ADAM_WIN_ACCENT_BUTTON_H

#include <windows.h>
#include <commctrl.h>

#include "adamsession.h"

#define ADAM_ACCENT_COLORREF                                              \
    RGB((ADAMSESSION_ACCENT_RGB >> 16) & 0xFF,                           \
        (ADAMSESSION_ACCENT_RGB >> 8) & 0xFF, ADAMSESSION_ACCENT_RGB & 0xFF)
#define ADAM_ACCENT_TEXT RGB(0, 0, 0)

/* The accent and a slightly lighter one for the hot (hovered) state; both
 * live for the process. */
static inline HBRUSH adam_accent_brush(void)
{
    static HBRUSH brush;
    if (!brush)
        brush = CreateSolidBrush(ADAM_ACCENT_COLORREF);
    return brush;
}

static inline HBRUSH adam_accent_hot_brush(void)
{
    static HBRUSH brush;
    if (!brush) {
        const COLORREF c = ADAM_ACCENT_COLORREF;
        const int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
        brush = CreateSolidBrush(RGB(r + (255 - r) / 5, g + (255 - g) / 5,
                                     b + (255 - b) / 5));
    }
    return brush;
}

/* Handles one NM_CUSTOMDRAW from a button. on = the button's toggle state;
 * the result is what the parent's WM_NOTIFY should return. */
static inline LRESULT adam_accent_customdraw(const NMCUSTOMDRAW *cd, int on)
{
    char text[64];
    RECT r;

    if (cd->dwDrawStage != CDDS_PREPAINT || !on)
        return CDRF_DODEFAULT;
    r = cd->rc;
    FillRect(cd->hdc, &r,
             (cd->uItemState & CDIS_HOT) ? adam_accent_hot_brush()
                                         : adam_accent_brush());
    FrameRect(cd->hdc, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetBkMode(cd->hdc, TRANSPARENT);
    SetTextColor(cd->hdc, ADAM_ACCENT_TEXT);
    text[0] = '\0';
    GetWindowTextA(cd->hdr.hwndFrom, text, sizeof text);
    {
        /* the control's own font, not the DC's default */
        HFONT f = (HFONT)SendMessageA(cd->hdr.hwndFrom, WM_GETFONT, 0, 0);
        HGDIOBJ old = f ? SelectObject(cd->hdc, f) : NULL;
        DrawTextA(cd->hdc, text, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old) SelectObject(cd->hdc, old);
    }
    if (GetFocus() == cd->hdr.hwndFrom) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(cd->hdc, &r);
    }
    return CDRF_SKIPDEFAULT;
}

#endif
