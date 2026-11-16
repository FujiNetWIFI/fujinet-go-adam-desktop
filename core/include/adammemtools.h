/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out.
 *
 * A watch is text the user typed (an address, a label, an expression, or a
 * range low-high / low..high) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a label
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAMMEMTOOLS_H
#define ADAMMEMTOOLS_H

#include <stdint.h>

#include "adamdebug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "low..high", "low-high" (plain numbers or labels on both sides), or one
 * expression (an address, label, label+3, [ptr]). A bare token of 1-4 hex
 * digits ("C000") is hex unless it names a label; "$C000" and expressions
 * work as in the console. Returns 1 and fills low and high (inclusive,
 * low <= high), 0 if the text means nothing. */
int adamsym_parse_range(adamdebug *dbg, const char *text, uint16_t *low, uint16_t *high);

/* ---- watches ---- */
#define ADAMWATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  adamwatch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). */
int  adamwatch_add(const char *text);
void adamwatch_remove(int index);
void adamwatch_clear(void);
const char *adamwatch_text(int index);
unsigned adamwatch_generation(void);
/* "score  $C010  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  adamwatch_format(adamdebug *dbg, int index, char *dst, int dstsz);
/* The range a watch covers (for "show in Memory"). 1 if it resolves. */
int  adamwatch_range(adamdebug *dbg, int index, uint16_t *low, uint16_t *high);

/* ---- memory search ---- */
enum { ADAMSEARCH_HEX = 0, ADAMSEARCH_TEXT };
#define ADAMSEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, count, mask (0xFF) and flags for
 * adamdebug_search. Returns 1, or 0 for empty / malformed text. */
int adamsearch_parse(int kind, const char *text, uint8_t *pat, int *count, uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  $C010 (score)  if value != old". Returns the length. */
int adambp_describe(adamdebug *dbg, const adamdebug_breakpoint *bpt, char *dst, int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from adamdebug_memory_map) holding addr
 * ("Cartridge (32768 bytes, upper bank 3)"), for labelling Memory rows. Fetch the map once per
 * refresh and look rows up in it. Returns the length, 0 and "" when no region
 * covers addr. */
int adammem_region_name(const adamdebug_region *map, int count, uint16_t addr, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* ADAMMEMTOOLS_H */
