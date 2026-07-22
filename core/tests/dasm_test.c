/*
 * Golden-file tests for the Z80 disassembler, hand-checked against the
 * Zilog Z80 CPU User Manual encoding tables (plus the usual undocumented
 * forms). Covers every prefix family and the metadata (length, target,
 * flow flags) the stepper depends on.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "adamdebug.h"
#include "z80dasm.h"

static int failures;

static void check(uint16_t addr, const uint8_t *code, const char *want_text,
                  int want_len, uint8_t want_flags, int want_target)
{
    z80d_insn insn;
    uint8_t buf[4] = {0, 0, 0, 0};
    memcpy(buf, code, 4);
    z80_disassemble(&insn, addr, buf);

    if (strcmp(insn.text, want_text) != 0) {
        fprintf(stderr, "FAIL %02X..: text \"%s\" want \"%s\"\n", code[0],
                insn.text, want_text);
        failures++;
    }
    if (insn.len != want_len) {
        fprintf(stderr, "FAIL %s: len %d want %d\n", want_text, insn.len,
                want_len);
        failures++;
    }
    if (insn.flags != want_flags) {
        fprintf(stderr, "FAIL %s: flags %02X want %02X\n", want_text,
                insn.flags, want_flags);
        failures++;
    }
    if (want_target >= 0 && insn.target != (uint16_t)want_target) {
        fprintf(stderr, "FAIL %s: target %04X want %04X\n", want_text,
                insn.target, want_target);
        failures++;
    }
}

#define C(...) (const uint8_t[4]){__VA_ARGS__}

int main(void)
{
    check(0, C(0x00), "NOP", 1, 0, -1);
    check(0, C(0x76), "HALT", 1, ADAMDASM_HALT, -1);
    check(0, C(0x08), "EX AF,AF'", 1, 0, -1);
    check(0, C(0x27), "DAA", 1, 0, -1);
    check(0, C(0x3F), "CCF", 1, 0, -1);
    check(0, C(0x3E, 0x42), "LD A,$42", 2, 0, -1);
    check(0, C(0x36, 0x55), "LD (HL),$55", 2, 0, -1);
    check(0, C(0x22, 0x34, 0x12), "LD ($1234),HL", 3, 0, -1);
    check(0, C(0x3A, 0x00, 0xFD), "LD A,($FD00)", 3, 0, -1);
    check(0, C(0x41), "LD B,C", 1, 0, -1);
    check(0, C(0x86), "ADD A,(HL)", 1, 0, -1);
    check(0, C(0xFE, 0x0D), "CP $0D", 2, 0, -1);
    check(0, C(0xE3), "EX (SP),HL", 1, 0, -1);
    check(0, C(0xE9), "JP (HL)", 1, ADAMDASM_JUMP, -1);
    check(0, C(0xC9), "RET", 1, ADAMDASM_RET, -1);
    check(0, C(0xC0), "RET NZ", 1, ADAMDASM_RET | ADAMDASM_COND, -1);
    check(0, C(0xC7), "RST $00", 1, ADAMDASM_CALL, 0x0000);
    check(0, C(0xFF), "RST $38", 1, ADAMDASM_CALL, 0x0038);

    check(0, C(0xC3, 0x34, 0x12), "JP $1234", 3, ADAMDASM_JUMP, 0x1234);
    check(0, C(0xFA, 0x00, 0x80), "JP M,$8000", 3,
          ADAMDASM_JUMP | ADAMDASM_COND, 0x8000);
    check(0, C(0xCD, 0xCD, 0xAB), "CALL $ABCD", 3, ADAMDASM_CALL, 0xABCD);
    check(0, C(0xD4, 0x00, 0x90), "CALL NC,$9000", 3,
          ADAMDASM_CALL | ADAMDASM_COND, 0x9000);

    /* Relative targets from a nonzero base. */
    check(0x0100, C(0x18, 0xFE), "JR $0100", 2,
          ADAMDASM_JUMP | ADAMDASM_RELATIVE, 0x0100);
    check(0x0100, C(0x20, 0x05), "JR NZ,$0107", 2,
          ADAMDASM_JUMP | ADAMDASM_RELATIVE | ADAMDASM_COND, 0x0107);
    check(0x0100, C(0x10, 0x05), "DJNZ $0107", 2,
          ADAMDASM_JUMP | ADAMDASM_RELATIVE | ADAMDASM_COND, 0x0107);

    /* CB page, incl. undocumented SLL. */
    check(0, C(0xCB, 0x27), "SLA A", 2, 0, -1);
    check(0, C(0xCB, 0x37), "SLL A", 2, 0, -1);
    check(0, C(0xCB, 0x46), "BIT 0,(HL)", 2, 0, -1);
    check(0, C(0xCB, 0xC7), "SET 0,A", 2, 0, -1);

    /* ED page. */
    check(0, C(0xED, 0x44), "NEG", 2, 0, -1);
    check(0, C(0xED, 0x4D), "RETI", 2, ADAMDASM_RET, -1);
    check(0, C(0xED, 0x45), "RETN", 2, ADAMDASM_RET, -1);
    check(0, C(0xED, 0xB0), "LDIR", 2, ADAMDASM_BLOCK, -1);
    check(0, C(0xED, 0xA1), "CPI", 2, 0, -1);
    check(0, C(0xED, 0x57), "LD A,I", 2, 0, -1);
    check(0, C(0xED, 0x67), "RRD", 2, 0, -1);
    check(0, C(0xED, 0x78), "IN A,(C)", 2, 0, -1);
    check(0, C(0xED, 0x4B, 0x00, 0x70), "LD BC,($7000)", 4, 0, -1);
    check(0, C(0xED, 0x43, 0x00, 0x70), "LD ($7000),BC", 4, 0, -1);

    /* DD/FD index pages. */
    check(0, C(0xDD, 0x21, 0x00, 0x40), "LD IX,$4000", 4, 0, -1);
    check(0, C(0xDD, 0x7E, 0x05), "LD A,(IX+5)", 3, 0, -1);
    check(0, C(0xFD, 0x77, 0xFB), "LD (IY-5),A", 3, 0, -1);
    check(0, C(0xDD, 0x34, 0x05), "INC (IX+5)", 3, 0, -1);
    check(0, C(0xDD, 0x36, 0x05, 0x66), "LD (IX+5),$66", 4, 0, -1);
    check(0, C(0xDD, 0xE3), "EX (SP),IX", 2, 0, -1);
    check(0, C(0xDD, 0xE9), "JP (IX)", 2, ADAMDASM_JUMP, -1);
    check(0, C(0xDD, 0x24), "INC IXH", 2, 0, -1);
    check(0, C(0xDD, 0x86, 0x02), "ADD A,(IX+2)", 3, 0, -1);

    /* DDCB: displacement precedes the operation byte. */
    check(0, C(0xFD, 0xCB, 0x03, 0x46), "BIT 0,(IY+3)", 4, 0, -1);
    check(0, C(0xDD, 0xCB, 0x02, 0x16), "RL (IX+2)", 4, 0, -1);
    check(0, C(0xDD, 0xCB, 0x02, 0x10), "RL (IX+2),B", 4, 0, -1);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("dasm_test: all checks passed\n");
    return 0;
}
