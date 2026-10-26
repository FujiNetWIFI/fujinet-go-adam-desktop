/*
 * debugger_internal -- the engine's side of the console (prompt.c) and of
 * the session glue (src/session_internal.h). Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAM_DEBUGGER_INTERNAL_H
#define ADAM_DEBUGGER_INTERNAL_H

#include "adamdebug.h"
#include "symbols.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The console: step n instructions (stopped only). */
void adamdebug_step_count(adamdebug *d, long n);
/* Wait up to ms for the machine to be stopped with a generation other than
 * gen (read before issuing the command). 1 if it stopped. */
int adamdebug_wait_stop(adamdebug *d, unsigned gen, int ms);
/* A command ran: bump the generation so windows refresh. */
void adamdebug_touch(adamdebug *d);
/* Register names the evaluator knows, for completion. */
extern const char *const adamdebug_reg_names[];
/* Label names starting with word (case-insensitive), one per line into dst,
 * at most maxn. Returns the count. */
int adamdebug_label_completions(adamdebug *d, const char *word, char *dst,
                                int dstsz, int maxn);

#ifdef __cplusplus
}
#endif

#endif /* ADAM_DEBUGGER_INTERNAL_H */
