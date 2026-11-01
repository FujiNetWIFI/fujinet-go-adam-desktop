/*
 * RGB565 -> XRGB8888 (0xFFRRGGBB), shared by both displays. The 5- and 6-bit
 * fields are expanded by replicating their high bits, not by shifting left: a
 * plain shift maps full-scale 0x1F to 0xF8, so white comes out slightly grey
 * and every colour a touch dark.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QtGlobal>
#include <cstddef>
#include <cstdint>

inline void rgb565ToXrgb(const uint16_t *src, quint32 *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const uint16_t p = src[i];
        unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        dst[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}
