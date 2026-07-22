/*
 * ADAM system ROM images embedded in the binary (generated at build time by
 * tools/adamcore/embed-roms.py from the ROM files in tools/adamcore/roms,
 * which are not part of the repository -- see COMPLIANCE.md).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ROMS_EMBEDDED_H
#define ROMS_EMBEDDED_H

#include <stdint.h>

#include "adamcore.h"

extern const uint8_t adam_rom_os7[ADAMCORE_OS7_ROM_SIZE];
extern const uint8_t adam_rom_eos[ADAMCORE_EOS_ROM_SIZE];
extern const uint8_t adam_rom_wp[ADAMCORE_WP_ROM_SIZE];

#endif /* ROMS_EMBEDDED_H */
