# Compliance and provenance

This repository is licensed **GPL-3.0-or-later** as a whole. Every
component it builds from is GPLv3-compatible:

| Component | Origin | License |
|---|---|---|
| adamcore (staged into `core/adamcore-generated/`) | github: clean-room ADAM/CV core by Thomas Cherryhomes (see its PROVENANCE.md) | GPL-3.0-or-later |
| libadamsession, frontends, debugger engine, tools | this repository | GPL-3.0-or-later |
| fujinet-firmware / fujinet-pc (ADAM), built as `libfujinet.so` | local fujinet-pc-adam checkout | GPL-3.0 |
| Bundled by the FujiNet build: libssh, libsmb2, libnfs | fujinet-firmware components | LGPL-2.1 |
| Bundled by the FujiNet build: expat (system), cJSON, mongoose | fujinet-firmware components / system | MIT / GPL-2.0 dual (mongoose) |
| mbedTLS (system libraries) | distribution packages | Apache-2.0 |
| SDL3, GTK4/libadwaita/WebKitGTK, Qt6 | system libraries, dynamically linked | zlib / LGPL |

## Deliberately not used

- **ADAMEm** (Marcel de Kogel) and its SDL port carry a *non-commercial*
  license that is not GPL-compatible. **No code from ADAMEm is present in
  this repository**, including its `z80dasm` disassembler. The debugger's
  Z80 disassembler (`core/debugger/z80dasm.c`) is a fresh implementation
  from the Zilog Z80 CPU User Manual's encoding description.
- **Gearcoleco** (GPLv3) was consulted only as a feature checklist for the
  debugger views; no code was taken.

## System ROMs

The Coleco ADAM system ROMs (`EOS.rom`, `OS7.rom`, `WP.rom`) are
copyrighted by Coleco and are **not part of this repository**
(`tools/adamcore/roms/` is git-ignored). Builders supply their own copies;
`tools/adamcore/embed-roms.py` embeds them into locally built binaries at
build time. Do not distribute binaries containing the ROMs unless you have
the right to distribute the ROMs themselves.

## Debug symbols

`core/debugger/symbols/{eos,os7}.sym` (and the generated
`symbols_builtin.c`) are tables of **facts** — names and addresses —
extracted by `tools/symbols/extract-symbols.py` from:

- Richard F. Drushel's public "EOS 5 Disassembly" (label/EQU names),
- the eoslib project's EOS jump-table wrappers (entry-point names),
- the os7lib project's OS7 listing (code labels).

They contain no program code.
