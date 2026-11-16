/*
 * debugger_internal -- the engine's side of the console (prompt.c) and of
 * the session glue (src/session_internal.h). Not installed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAM_DEBUGGER_INTERNAL_H
#define ADAM_DEBUGGER_INTERNAL_H

#include "adamcore.h"
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
/* The engine's lock and core, for the getters in iostate.c. The caller must
 * hold the lock (adamdebug_lock) for as long as it uses the core pointer: the
 * emulation thread clears it under the same lock before it destroys the
 * machine. NULL without one. Do not call a public adamdebug_ function with
 * the lock held. */
void adamdebug_lock(adamdebug *d);
void adamdebug_unlock(adamdebug *d);
adamcore *adamdebug_core_locked(adamdebug *d);
adamsession *adamdebug_session_of(adamdebug *d);
/* FujiNet packets seen [0] ADAM -> FujiNet and [1] FujiNet -> ADAM, since the
 * session started. 0 without a trace. */
int adamdebug_fuji_packets(adamdebug *d, uint32_t packets[2]);
/* The symbol tables, for completion (the caller must not keep the pointer
 * across a load). */
const sym_tables *adamdebug_symtabs(adamdebug *d);
/* The Video tab's text hook, or NULL (see adamdebug_set_video_text). */
const char *(*adamdebug_video_text_hook(void))(const char *section);
/* Label names starting with word (case-insensitive), one per line into dst,
 * at most maxn. Returns the count. */
int adamdebug_label_completions(adamdebug *d, const char *word, char *dst,
                                int dstsz, int maxn);

#ifdef __cplusplus
}
#endif

#endif /* ADAM_DEBUGGER_INTERNAL_H */
