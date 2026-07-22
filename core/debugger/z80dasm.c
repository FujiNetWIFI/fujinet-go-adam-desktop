/*
 * Z80 disassembler for the adamdebug engine. Written fresh from the Zilog
 * Z80 CPU User Manual's instruction encoding (the standard x/y/z octal
 * field decomposition), including the undocumented SLL, IXH/IXL halves,
 * and DDCB store-forms. No code from any existing disassembler.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "adamdebug.h"
#include "z80dasm.h"

static const char *const r_name[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
static const char *const rp_name[4] = {"BC", "DE", "HL", "SP"};
static const char *const rp2_name[4] = {"BC", "DE", "HL", "AF"};
static const char *const cc_name[8] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
static const char *const alu_name[8] = {"ADD A,", "ADC A,", "SUB ", "SBC A,",
                                        "AND ",   "XOR ",   "OR ",  "CP "};
static const char *const rot_name[8] = {"RLC", "RRC", "RL",  "RR",
                                        "SLA", "SRA", "SLL", "SRL"};
static const char *const im_name[8] = {"0", "0/1", "1", "2", "0", "0/1", "1", "2"};
static const char *const bli_name[4][4] = {
    {"LDI", "CPI", "INI", "OUTI"},
    {"LDD", "CPD", "IND", "OUTD"},
    {"LDIR", "CPIR", "INIR", "OTIR"},
    {"LDDR", "CPDR", "INDR", "OTDR"},
};

typedef struct {
    const uint8_t *code; /* up to 4 bytes at addr */
    int pos;
    uint16_t addr;
    z80d_insn *out;
} dctx;

static uint8_t fetch(dctx *d)
{
    uint8_t b = d->pos < 4 ? d->code[d->pos] : 0;
    if (d->pos < 4)
        d->out->bytes[d->pos] = b;
    d->pos++;
    return b;
}

static uint16_t fetch16(dctx *d)
{
    uint16_t lo = fetch(d);
    return (uint16_t)(lo | ((uint16_t)fetch(d) << 8));
}

static void emit(dctx *d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->out->text, sizeof(d->out->text), fmt, ap);
    va_end(ap);
}

/* Index-substituted register name: with a DD/FD prefix, H/L become the
 * index halves and (HL) becomes (IX+d) -- but not in the same instruction
 * as an (IX+d) operand's partner (handled by callers passing ix=0). */
static const char *r_ix(int r, int ix, char *scratch, int d8)
{
    static const char *const hx[2] = {"IXH", "IYH"};
    static const char *const lx[2] = {"IXL", "IYL"};
    if (!ix)
        return r_name[r];
    if (r == 4) return hx[ix - 1];
    if (r == 5) return lx[ix - 1];
    if (r == 6) {
        sprintf(scratch, "(I%c%+d)", ix == 1 ? 'X' : 'Y', (int8_t)d8);
        return scratch;
    }
    return r_name[r];
}

static const char *rp_ix(int p, int ix)
{
    if (ix && p == 2)
        return ix == 1 ? "IX" : "IY";
    return rp_name[p];
}

static void dasm_cb(dctx *d, int ix, uint8_t disp)
{
    uint8_t op = fetch(d);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    char m[12];

    if (ix)
        sprintf(m, "(I%c%+d)", ix == 1 ? 'X' : 'Y', (int8_t)disp);
    else
        strcpy(m, r_name[z]);

    if (x == 0) {
        if (ix && z != 6) /* undocumented store form: RLC (IX+d),r */
            emit(d, "%s %s,%s", rot_name[y], m, r_name[z]);
        else
            emit(d, "%s %s", rot_name[y], ix ? m : r_name[z]);
    } else if (x == 1) {
        emit(d, "BIT %d,%s", y, ix ? m : r_name[z]);
    } else if (x == 2) {
        if (ix && z != 6)
            emit(d, "RES %d,%s,%s", y, m, r_name[z]);
        else
            emit(d, "RES %d,%s", y, ix ? m : r_name[z]);
    } else {
        if (ix && z != 6)
            emit(d, "SET %d,%s,%s", y, m, r_name[z]);
        else
            emit(d, "SET %d,%s", y, ix ? m : r_name[z]);
    }
}

static void dasm_ed(dctx *d)
{
    uint8_t op = fetch(d);
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    z80d_insn *o = d->out;

    if (x == 1) {
        switch (z) {
        case 0:
            if (y == 6) emit(d, "IN (C)");
            else emit(d, "IN %s,(C)", r_name[y]);
            return;
        case 1:
            if (y == 6) emit(d, "OUT (C),0");
            else emit(d, "OUT (C),%s", r_name[y]);
            return;
        case 2:
            emit(d, "%s HL,%s", q ? "ADC" : "SBC", rp_name[p]);
            return;
        case 3: {
            uint16_t nn = fetch16(d);
            if (q) emit(d, "LD %s,($%04X)", rp_name[p], nn);
            else emit(d, "LD ($%04X),%s", nn, rp_name[p]);
            return;
        }
        case 4: emit(d, "NEG"); return;
        case 5:
            emit(d, y == 1 ? "RETI" : "RETN");
            o->flags |= ADAMDASM_RET;
            return;
        case 6: emit(d, "IM %s", im_name[y]); return;
        default:
            switch (y) {
            case 0: emit(d, "LD I,A"); return;
            case 1: emit(d, "LD R,A"); return;
            case 2: emit(d, "LD A,I"); return;
            case 3: emit(d, "LD A,R"); return;
            case 4: emit(d, "RRD"); return;
            case 5: emit(d, "RLD"); return;
            default: emit(d, "NOP*"); return;
            }
        }
    }
    if (x == 2 && z <= 3 && y >= 4) {
        emit(d, "%s", bli_name[y - 4][z]);
        if (y >= 6)
            o->flags |= ADAMDASM_BLOCK;
        return;
    }
    emit(d, "NOP*");
}

static void dasm_main(dctx *d, int ix)
{
    uint8_t op = fetch(d);
    int x, y, z, p, q;
    z80d_insn *o = d->out;
    char m1[12], m2[12];
    uint8_t disp = 0;

    /* An (IX+d) memory operand fetches its displacement between opcode and
     * any immediate; peek it where the encoding calls for one. */

    x = op >> 6;
    y = (op >> 3) & 7;
    z = op & 7;
    p = y >> 1;
    q = y & 1;

    if (x == 0) {
        switch (z) {
        case 0:
            switch (y) {
            case 0: emit(d, "NOP"); return;
            case 1: emit(d, "EX AF,AF'"); return;
            case 2: {
                int8_t e = (int8_t)fetch(d);
                o->target = (uint16_t)(d->addr + 2 + e);
                o->flags |= ADAMDASM_JUMP | ADAMDASM_RELATIVE | ADAMDASM_COND;
                emit(d, "DJNZ $%04X", o->target);
                return;
            }
            case 3: {
                int8_t e = (int8_t)fetch(d);
                o->target = (uint16_t)(d->addr + 2 + e);
                o->flags |= ADAMDASM_JUMP | ADAMDASM_RELATIVE;
                emit(d, "JR $%04X", o->target);
                return;
            }
            default: {
                int8_t e = (int8_t)fetch(d);
                o->target = (uint16_t)(d->addr + 2 + e);
                o->flags |= ADAMDASM_JUMP | ADAMDASM_RELATIVE | ADAMDASM_COND;
                emit(d, "JR %s,$%04X", cc_name[y - 4], o->target);
                return;
            }
            }
        case 1:
            if (q == 0) {
                uint16_t nn = fetch16(d);
                emit(d, "LD %s,$%04X", rp_ix(p, ix), nn);
            } else {
                emit(d, "ADD %s,%s", rp_ix(2, ix),
                     p == 2 ? rp_ix(2, ix) : rp_name[p]);
            }
            return;
        case 2:
            if (p < 2) {
                static const char *const forms[2][2] = {
                    {"LD (BC),A", "LD A,(BC)"},
                    {"LD (DE),A", "LD A,(DE)"},
                };
                emit(d, "%s", forms[p][q]);
            } else {
                uint16_t nn = fetch16(d);
                if (p == 2) {
                    if (q) emit(d, "LD %s,($%04X)", rp_ix(2, ix), nn);
                    else emit(d, "LD ($%04X),%s", nn, rp_ix(2, ix));
                } else {
                    if (q) emit(d, "LD A,($%04X)", nn);
                    else emit(d, "LD ($%04X),A", nn);
                }
            }
            return;
        case 3:
            emit(d, "%s %s", q ? "DEC" : "INC", rp_ix(p, ix));
            return;
        case 4:
            if (ix && y == 6) disp = fetch(d);
            emit(d, "INC %s", r_ix(y, ix, m1, disp));
            return;
        case 5:
            if (ix && y == 6) disp = fetch(d);
            emit(d, "DEC %s", r_ix(y, ix, m1, disp));
            return;
        case 6: {
            uint8_t n;
            if (ix && y == 6) disp = fetch(d);
            n = fetch(d);
            emit(d, "LD %s,$%02X", r_ix(y, ix, m1, disp), n);
            return;
        }
        default: {
            static const char *const acc_ops[8] = {"RLCA", "RRCA", "RLA",
                                                   "RRA",  "DAA",  "CPL",
                                                   "SCF",  "CCF"};
            emit(d, "%s", acc_ops[y]);
            return;
        }
        }
    }

    if (x == 1) {
        if (op == 0x76) {
            o->flags |= ADAMDASM_HALT;
            emit(d, "HALT");
            return;
        }
        /* With an index prefix, only the (IX+d) side substitutes; the
         * plain register partner keeps its name. */
        if (ix && (y == 6 || z == 6)) {
            disp = fetch(d);
            emit(d, "LD %s,%s",
                 y == 6 ? r_ix(6, ix, m1, disp) : r_name[y],
                 z == 6 ? r_ix(6, ix, m2, disp) : r_name[z]);
        } else {
            emit(d, "LD %s,%s", r_ix(y, ix, m1, 0), r_ix(z, ix, m2, 0));
        }
        return;
    }

    if (x == 2) {
        if (ix && z == 6) disp = fetch(d);
        emit(d, "%s%s", alu_name[y], r_ix(z, ix, m1, disp));
        return;
    }

    /* x == 3 */
    switch (z) {
    case 0:
        o->flags |= ADAMDASM_RET | ADAMDASM_COND;
        emit(d, "RET %s", cc_name[y]);
        return;
    case 1:
        if (q == 0) {
            emit(d, "POP %s", p == 2 ? rp_ix(2, ix) : rp2_name[p]);
        } else {
            switch (p) {
            case 0:
                o->flags |= ADAMDASM_RET;
                emit(d, "RET");
                return;
            case 1: emit(d, "EXX"); return;
            case 2:
                o->flags |= ADAMDASM_JUMP;
                emit(d, "JP (%s)", rp_ix(2, ix));
                return;
            default: emit(d, "LD SP,%s", rp_ix(2, ix)); return;
            }
        }
        return;
    case 2: {
        uint16_t nn = fetch16(d);
        o->target = nn;
        o->flags |= ADAMDASM_JUMP | ADAMDASM_COND;
        emit(d, "JP %s,$%04X", cc_name[y], nn);
        return;
    }
    case 3:
        switch (y) {
        case 0: {
            uint16_t nn = fetch16(d);
            o->target = nn;
            o->flags |= ADAMDASM_JUMP;
            emit(d, "JP $%04X", nn);
            return;
        }
        case 2: emit(d, "OUT ($%02X),A", fetch(d)); return;
        case 3: emit(d, "IN A,($%02X)", fetch(d)); return;
        case 4: emit(d, "EX (SP),%s", rp_ix(2, ix)); return;
        case 5: emit(d, "EX DE,HL"); return;
        case 6: emit(d, "DI"); return;
        default: emit(d, "EI"); return;
        }
    case 4: {
        uint16_t nn = fetch16(d);
        o->target = nn;
        o->flags |= ADAMDASM_CALL | ADAMDASM_COND;
        emit(d, "CALL %s,$%04X", cc_name[y], nn);
        return;
    }
    case 5:
        if (q == 0) {
            emit(d, "PUSH %s", p == 2 ? rp_ix(2, ix) : rp2_name[p]);
        } else if (p == 0) {
            uint16_t nn = fetch16(d);
            o->target = nn;
            o->flags |= ADAMDASM_CALL;
            emit(d, "CALL $%04X", nn);
        }
        return; /* p 1..3 are prefixes, handled by the caller */
    case 6: {
        uint8_t n;
        (void)ix;
        n = fetch(d);
        emit(d, "%s$%02X", alu_name[y], n);
        return;
    }
    default:
        o->target = (uint16_t)(y * 8);
        o->flags |= ADAMDASM_CALL;
        emit(d, "RST $%02X", y * 8);
        return;
    }
}

int z80_disassemble(z80d_insn *out, uint16_t addr, const uint8_t code[4])
{
    dctx d;
    uint8_t op;

    memset(out, 0, sizeof(*out));
    out->addr = addr;
    d.code = code;
    d.pos = 0;
    d.addr = addr;
    d.out = out;

    op = code[0];
    if (op == 0xCB) {
        fetch(&d);
        dasm_cb(&d, 0, 0);
    } else if (op == 0xED) {
        fetch(&d);
        dasm_ed(&d);
    } else if (op == 0xDD || op == 0xFD) {
        int ix = op == 0xDD ? 1 : 2;
        fetch(&d);
        if (code[1] == 0xCB) {
            uint8_t disp;
            fetch(&d);            /* CB */
            disp = fetch(&d);     /* displacement precedes the op */
            dasm_cb(&d, ix, disp);
        } else if (code[1] == 0xDD || code[1] == 0xFD || code[1] == 0xED) {
            /* A prefix overridden by another: the first is a no-op. */
            snprintf(out->text, sizeof(out->text), "NOP* (prefix)");
        } else {
            dasm_main(&d, ix);
        }
    } else {
        dasm_main(&d, 0);
    }

    out->len = (uint8_t)(d.pos > 4 ? 4 : d.pos);
    if (out->len == 0)
        out->len = 1;
    return out->len;
}
