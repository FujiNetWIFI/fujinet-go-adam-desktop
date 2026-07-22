/*
 * Z80 disassembler (fresh implementation; see z80dasm.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef Z80DASM_H
#define Z80DASM_H

#include <stdint.h>

/* One decoded instruction; text/flags conventions match adamdasm_line in
 * adamdebug.h (which embeds this data). */
typedef struct {
    uint16_t addr;
    uint8_t len; /* 1..4 */
    uint8_t bytes[4];
    char text[32];
    uint16_t target;
    uint8_t flags; /* ADAMDASM_* */
} z80d_insn;

/* Decodes the instruction at addr from code[0..3]; returns its length. */
int z80_disassemble(z80d_insn *out, uint16_t addr, const uint8_t code[4]);

#endif /* Z80DASM_H */
