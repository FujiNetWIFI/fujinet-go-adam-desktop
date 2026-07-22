#!/usr/bin/env python3
"""Generate the EOS/OS7 debugger symbol tables.

Sources (all local, see COMPLIANCE.md for provenance):
  * EOS public entry points: the eoslib C wrappers (~/Workspace/eoslib),
    where each src/eos_NAME.c calls its EOS jump-table entry as
    AsmCall(0xADDR, ...). NAME becomes the symbol at ADDR.
  * EOS named constants: `NAME EQU <decimal>` lines from Richard F.
    Drushel's "EOS 5 Disassembly" (the A<decimal> positional labels are
    deliberately skipped -- they carry no information beyond the address).
  * OS7: the Ghidra-style listing in ~/Workspace/os7lib/sre, taking named
    code labels and function signatures (BYTE_/DAT_ data labels skipped)
    with the address of the next `OS7:ram:XXXX` line.

Outputs (committed):
  core/debugger/symbols/eos.sym, core/debugger/symbols/os7.sym
  core/debugger/symbols_builtin.c  (the same tables as C strings)

Usage: extract-symbols.py [--eoslib DIR] [--drushel FILE] [--os7 FILE]
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
WS = Path.home() / "Workspace"


def extract_eoslib(eoslib_dir):
    syms = {}
    for src in sorted(eoslib_dir.glob("src/eos_*.c")):
        m = re.search(r"AsmCall\(0x([0-9A-Fa-f]{2,4})", src.read_text(errors="replace"))
        if not m:
            continue
        addr = int(m.group(1), 16)
        name = src.stem.upper()
        syms.setdefault(addr, name)
    return syms


def extract_drushel(asm_path):
    syms = {}
    for line in asm_path.read_text(errors="replace").splitlines():
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+EQU\s+(\d+)", line)
        if not m:
            continue
        name, addr = m.group(1), int(m.group(2))
        if re.fullmatch(r"A\d+", name) or addr > 0xFFFF:
            continue
        syms.setdefault(addr, name)
    return syms


# --- named-label address reconstruction ------------------------------------
# The Drushel source's named internal labels (__WRITE_VRAM, __POLLER, ...)
# carry no addresses; only the positional A<decimal> labels do. Rather than
# requiring a Z80 assembler, walk the source accumulating encoded
# instruction lengths from ORG, and validate at every A<decimal> anchor:
# a segment whose computed address disagrees with its anchor is discarded
# (only its labels), so a parsing gap can never emit a wrong address.

REG8 = {"A", "B", "C", "D", "E", "H", "L", "I", "R"}
REG16 = {"AF", "AF'", "BC", "DE", "HL", "SP", "IX", "IY"}


def op_kind(op):
    u = op.strip().upper()
    if u in REG8:
        return "r8"
    if u in REG16:
        return "rr", u
    if u in ("(HL)", "(BC)", "(DE)", "(SP)"):
        return "mrr", u
    if u == "(C)":
        return "portc"
    if u.startswith("(IX") or u.startswith("(IY"):
        return "idx"
    if u in ("IXH", "IXL", "IYH", "IYL"):
        return "xh"
    if u.startswith("("):
        return "mimm"
    return "imm"


def kindname(k):
    return k[0] if isinstance(k, tuple) else k


def split_operands(text):
    ops, cur, q = [], "", None
    for ch in text:
        if q:
            cur += ch
            if ch == q:
                q = None
        elif ch in "'\"":
            q = ch
            cur += ch
        elif ch == ",":
            ops.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return ops


def eval_expr(text, equ):
    t = text.strip()
    t = re.sub(r"[A-Za-z_][A-Za-z0-9_]*",
               lambda m: str(equ.get(m.group(0), m.group(0))), t)
    if re.fullmatch(r"[0-9+*\-() ]+", t):
        try:
            return int(eval(t))  # charset-restricted arithmetic only
        except (SyntaxError, ValueError):
            return None
    return None


def data_len(mn, ops, equ):
    if mn == "DB":
        n = 0
        for op in ops:
            if len(op) >= 3 and op[0] in "'\"" and op[-1] == op[0]:
                n += len(op) - 2
            else:
                n += 1
        return n
    if mn == "DW":
        return 2 * len(ops)
    if mn == "DS":
        return eval_expr(ops[0], equ)
    return None


def insn_len(mn, ops):
    kinds = [op_kind(o) for o in ops]
    names = [kindname(k) for k in kinds]
    ix = any(n in ("idx",) or (isinstance(k, tuple) and k[1] in ("IX", "IY"))
             for k, n in zip(kinds, names))
    d = 1 if "idx" in names else 0

    if mn in ("NOP", "HALT", "DI", "EI", "EXX", "RLCA", "RRCA", "RLA",
              "RRA", "DAA", "CPL", "SCF", "CCF"):
        return 1
    if mn in ("LDI", "LDD", "LDIR", "LDDR", "CPI", "CPD", "CPIR", "CPDR",
              "INI", "IND", "INIR", "INDR", "OUTI", "OUTD", "OTIR", "OTDR",
              "NEG", "RETI", "RETN", "RRD", "RLD", "IM"):
        return 2
    if mn in ("JR", "DJNZ"):
        return 2
    if mn == "CALL":
        return 3
    if mn == "RST" or mn == "RET":
        return 1
    if mn == "JP":
        if names and names[-1] in ("mrr",):
            return 1  # JP (HL)
        if names and names[-1] == "idx":
            return 2  # JP (IX) parses as idx
        return 3
    if mn in ("PUSH", "POP"):
        return 2 if ix else 1
    if mn in ("INC", "DEC"):
        if names[0] == "idx":
            return 3
        if names[0] == "xh" or ix:
            return 2
        return 1
    if mn in ("ADD", "ADC", "SBC"):
        first = ops[0].strip().upper() if ops else ""
        if first in ("HL", "IX", "IY"):
            if first != "HL":
                return 2
            return 1 if mn == "ADD" else 2
        # accumulator form
        last = names[-1]
        if last == "imm":
            return 2
        if last == "idx":
            return 3
        if last == "xh":
            return 2
        return 1
    if mn in ("AND", "OR", "XOR", "CP", "SUB"):
        last = names[-1]
        if last == "imm":
            return 2
        if last == "idx":
            return 3
        if last == "xh":
            return 2
        return 1
    if mn in ("BIT", "SET", "RES"):
        return 4 if names[-1] == "idx" else 2
    if mn in ("RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"):
        return 4 if names[-1] == "idx" else 2
    if mn == "EX":
        return 2 if ix else 1
    if mn in ("IN", "OUT"):
        return 2
    if mn == "LD":
        if len(ops) != 2:
            return None
        (ka, kb) = kinds
        na, nb = names
        a = ops[0].strip().upper()
        b = ops[1].strip().upper()
        if na == "idx" or nb == "idx":
            # LD (IX+d),n is 4; register forms are 3
            if nb == "imm" and na == "idx":
                return 4
            return 3
        if na == "xh" or nb == "xh":
            return 3 if (nb == "imm") else 2
        if na == "rr":
            if a == "SP" and b in ("HL", "IX", "IY"):
                return 1 if b == "HL" else 2
            if nb == "mimm":
                if a == "HL":
                    return 3
                return 4  # BC/DE/SP via ED, IX/IY via DD/FD
            n16 = 4 if a in ("IX", "IY") else 3
            return n16  # LD rr,nn
        if nb == "rr":
            # LD (nn),rr
            if b == "HL":
                return 3
            return 4
        if na == "mimm" or nb == "mimm":
            return 3  # LD A,(nn) / LD (nn),A
        if na == "mrr" or nb == "mrr":
            return 2 if nb == "imm" else 1  # LD (HL),n=2; LD (HL),r=1
        if (a in ("I", "R") and b == "A") or (a == "A" and b in ("I", "R")):
            return 2
        if nb == "imm":
            return 2  # LD r,n / LD (HL),n handled above via mrr? no:
        return 1
    return None


def reconstruct_drushel_labels(asm_path):
    addr = None
    pending = []          # (name, addr) since the last good anchor
    dirty = False         # a line in this segment could not be sized
    committed = {}
    stats = {"anchors": 0, "drift": 0, "unknown": 0}
    equ = {}

    for raw in asm_path.read_text(errors="replace").splitlines():
        line = raw
        # strip comment outside quotes
        out, q = "", None
        for ch in line:
            if q:
                out += ch
                if ch == q:
                    q = None
            elif ch in "'\"":
                q = ch
                out += ch
            elif ch == ";":
                break
            else:
                out += ch
        line = out.rstrip()
        if not line.strip():
            continue

        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+EQU\s+(\d+)", line)
        if m:
            equ[m.group(1)] = int(m.group(2))
            continue

        m = re.match(r"^\s+ORG\s+(\S+)", line)
        if m:
            v = m.group(1)
            addr = equ.get(v, int(v) if v.isdigit() else None)
            continue

        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*):?\s*$", line)
        if m:
            name = m.group(1)
            am = re.fullmatch(r"A(\d+)", name)
            if am:
                expect = int(am.group(1))
                stats["anchors"] += 1
                if addr == expect and not dirty:
                    for n, a in pending:
                        committed.setdefault(n, a)
                elif addr != expect:
                    stats["drift"] += 1
                addr = expect  # anchors are ground truth; resync
                pending = []
                dirty = False
            elif addr is not None:
                pending.append((name, addr))
            continue

        # instruction / data line
        parts = line.strip().split(None, 1)
        mn = parts[0].upper()
        ops = split_operands(parts[1]) if len(parts) > 1 else []
        n = data_len(mn, ops, equ)
        if n is None:
            n = insn_len(mn, ops)
        if n is None:
            stats["unknown"] += 1
            dirty = True   # poison the segment; the next anchor resyncs
            continue
        if addr is not None:
            addr += n

    return committed, stats


def extract_os7(listing_path):
    syms = {}
    pending = []
    label_re = re.compile(r"^\s{8,}([A-Za-z_][A-Za-z0-9_]*):")
    func_re = re.compile(r"^\s{8,};\s*(?:undefined|void|byte|int|char)\s+([A-Za-z_]\w*)\s*\(")
    addr_re = re.compile(r"^OS7:ram:([0-9a-f]{1,4})\s")
    for line in listing_path.read_text(errors="replace").splitlines():
        m = label_re.match(line)
        if m:
            name = m.group(1)
            if not name.startswith(("BYTE_", "DAT_", "UNK_", "PTR_")):
                pending.append(name)
            continue
        m = func_re.match(line)
        if m:
            pending.append(m.group(1))
            continue
        m = addr_re.match(line)
        if m and pending:
            addr = int(m.group(1), 16)
            for name in pending:
                syms.setdefault(addr, name.rstrip("_"))
            pending = []
    return syms


def write_sym(path, syms, header):
    lines = [f"# {header}", "# Generated by tools/symbols/extract-symbols.py"]
    for addr in sorted(syms):
        lines.append(f"{addr:04X} {syms[addr]}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")
    return len(syms)


def c_string(text):
    out = []
    for line in text.splitlines():
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'    "{esc}\\n"')
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eoslib", type=Path, default=WS / "eoslib")
    ap.add_argument("--drushel", type=Path,
                    default=WS / "fujinet-firmware-learn-adamnet-bug" /
                    "EOS 5 Disassembly.asm")
    ap.add_argument("--os7", type=Path,
                    default=WS / "os7lib" / "sre" / "os7lib-dkong.txt")
    args = ap.parse_args()

    eos = {}
    if args.drushel.exists():
        eos.update(extract_drushel(args.drushel))
        rec, stats = reconstruct_drushel_labels(args.drushel)
        for a, n in ((a, n) for n, a in rec.items()):
            eos.setdefault(a, n)
        print(f"reconstructed {len(rec)} internal EOS labels "
              f"({stats['anchors']} anchors, {stats['drift']} drifted, "
              f"{stats['unknown']} unsized lines)", file=sys.stderr)
    else:
        print(f"warning: {args.drushel} missing", file=sys.stderr)
    if args.eoslib.exists():
        eos.update(extract_eoslib(args.eoslib))  # eoslib names win
    else:
        print(f"warning: {args.eoslib} missing", file=sys.stderr)

    os7 = {}
    if args.os7.exists():
        os7 = extract_os7(args.os7)
    else:
        print(f"warning: {args.os7} missing", file=sys.stderr)

    symdir = REPO / "core" / "debugger" / "symbols"
    n_eos = write_sym(symdir / "eos.sym", eos,
                      "EOS symbols: eoslib jump-table entries + Drushel EQUs")
    n_os7 = write_sym(symdir / "os7.sym", os7,
                      "OS7 symbols: os7lib SRE listing labels")

    builtin = REPO / "core" / "debugger" / "symbols_builtin.c"
    builtin.write_text(
        "/* Generated by tools/symbols/extract-symbols.py -- do not edit.\n"
        " * Built-in EOS/OS7 symbol tables; provenance in COMPLIANCE.md.\n"
        " * SPDX-License-Identifier: GPL-3.0-or-later\n"
        " */\n\n"
        '#include "symbols.h"\n\n'
        "const char adamdebug_builtin_eos_sym[] =\n"
        + c_string((symdir / "eos.sym").read_text())
        + ";\n\n"
        "const char adamdebug_builtin_os7_sym[] =\n"
        + c_string((symdir / "os7.sym").read_text())
        + ";\n"
    )
    print(f"eos.sym: {n_eos} symbols, os7.sym: {n_os7} symbols")


if __name__ == "__main__":
    main()
