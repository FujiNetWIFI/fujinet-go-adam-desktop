/*
 * The app's accent colour for the debugger windows: the icon's blue, from
 * the session header, so the PC line, the stopped Run button and the
 * changed-register rows all follow the icon. ink is the text on an accent
 * fill; light is the accent for lines and text on a dark ground.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAM_WIN_DBG_ACCENT_H
#define ADAM_WIN_DBG_ACCENT_H

#include <windows.h>
#include <commctrl.h>

#include "adamsession.h"

#define ADAM_RGB_OF(hex) RGB(((hex) >> 16) & 0xFF, ((hex) >> 8) & 0xFF, (hex) & 0xFF)
#define ADAM_ACCENT_COLORREF ADAM_RGB_OF(ADAMSESSION_ACCENT_RGB)
#define ADAM_ACCENT_INK_COLORREF ADAM_RGB_OF(ADAMSESSION_ACCENT_INK_RGB)
#define ADAM_ACCENT_LIGHT_COLORREF ADAM_RGB_OF(ADAMSESSION_ACCENT_LIGHT_RGB)

/* Handles one NM_CUSTOMDRAW from a toggle button (comctl32 v6, see
 * app.manifest.in): on = its state; the accent fill with the ink while it is
 * on, the themed look while it is off. The result is what the parent's
 * WM_NOTIFY should return. */
static inline LRESULT adam_accent_customdraw(const NMCUSTOMDRAW *draw, int on)
{
    static HBRUSH accent;
    char text[64];
    RECT area;
    if (draw->dwDrawStage != CDDS_PREPAINT || !on) return CDRF_DODEFAULT;
    if (!accent) accent = CreateSolidBrush(ADAM_ACCENT_COLORREF);
    area = draw->rc;
    FillRect(draw->hdc, &area, accent);
    FrameRect(draw->hdc, &area, (HBRUSH)GetStockObject(GRAY_BRUSH));
    SetBkMode(draw->hdc, TRANSPARENT);
    SetTextColor(draw->hdc, ADAM_ACCENT_INK_COLORREF);
    text[0] = '\0';
    GetWindowTextA(draw->hdr.hwndFrom, text, sizeof text);
    {
        HFONT font = (HFONT)SendMessageA(draw->hdr.hwndFrom, WM_GETFONT, 0, 0);
        HGDIOBJ old = font ? SelectObject(draw->hdc, font) : NULL;
        DrawTextA(draw->hdc, text, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old) SelectObject(draw->hdc, old);
    }
    if (GetFocus() == draw->hdr.hwndFrom) {
        InflateRect(&area, -3, -3);
        DrawFocusRect(draw->hdc, &area);
    }
    return CDRF_SKIPDEFAULT;
}

#endif
