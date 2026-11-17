/*
 * The app's accent colour (ADAMSESSION_ACCENT_RGB, the icon colour: the ADAM
 * light blue-violet, the pair of the ColecoVision app's dark blue). Buttons in
 * the accent state -- Map while armed, the remap target -- carry the dynamic
 * property "accent" and wear it with the accent's ink; main.cpp's style sheet
 * draws them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPushButton>
#include <QStyle>

#include "adamsession.h"

inline QColor adamCtlAccent()
{
    return QColor((ADAMSESSION_ACCENT_RGB >> 16) & 0xff,
                  (ADAMSESSION_ACCENT_RGB >> 8) & 0xff,
                  ADAMSESSION_ACCENT_RGB & 0xff);
}

/* The accent's edge (a held part's outline shade) and the ink on it. */
inline QColor adamCtlAccentDark() { return adamCtlAccent().darker(140); }
inline QColor adamCtlAccentInk()
{
    return QColor((ADAMSESSION_ACCENT_INK_RGB >> 16) & 0xff,
                  (ADAMSESSION_ACCENT_INK_RGB >> 8) & 0xff,
                  ADAMSESSION_ACCENT_INK_RGB & 0xff);
}

/* Puts a button in or out of the accent state and repolishes it, so the
 * style sheet's [accent="true"] rule takes effect at once. */
inline void adamCtlSetAccent(QPushButton *b, bool on)
{
    if (b->property("accent").toBool() == on) return;
    b->setProperty("accent", on);
    b->style()->unpolish(b);
    b->style()->polish(b);
}
