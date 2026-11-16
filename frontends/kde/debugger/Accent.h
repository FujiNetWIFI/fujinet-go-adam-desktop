/*
 * The app's accent as Qt colours, from the session header's macros: the
 * fill, the ink on it, and the accent as text or lines (the light variant on
 * a dark theme, where the fill itself is only ~1.5:1).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPalette>
#include <QWidget>

extern "C" {
#include "adamsession.h"
}

inline QColor adamDebugRgb(unsigned rgb)
{
    return QColor(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF));
}

inline QColor adamDebugAccentColor() { return adamDebugRgb(ADAMSESSION_ACCENT_RGB); }
inline QColor adamDebugAccentInk() { return adamDebugRgb(ADAMSESSION_ACCENT_INK_RGB); }

/* The accent for text and lines: light on a dark theme, the fill otherwise. */
inline QColor adamDebugAccentTextColor(const QWidget *widget)
{
    if (widget && widget->palette().color(QPalette::Window).lightness() < 128)
        return adamDebugRgb(ADAMSESSION_ACCENT_LIGHT_RGB);
    return adamDebugAccentColor();
}

/* A highlight fill and the ink on it: the icon blue with white ink on a light
 * theme; on a dark one the pure blue barely shows, so the light blue with
 * black ink. */
inline bool adamDebugDarkTheme(const QWidget *widget)
{
    return widget && widget->palette().color(QPalette::Window).lightness() < 128;
}
inline QColor adamDebugAccentFill(const QWidget *widget)
{
    return adamDebugDarkTheme(widget) ? adamDebugRgb(ADAMSESSION_ACCENT_LIGHT_RGB) : adamDebugAccentColor();
}
inline QColor adamDebugAccentFillInk(const QWidget *widget)
{
    return adamDebugDarkTheme(widget) ? QColor(Qt::black) : adamDebugAccentInk();
}
