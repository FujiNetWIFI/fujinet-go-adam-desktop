/*
 * bindings -- private interface to bindings.c. The table is process-global,
 * like gamepad_sdl.c's pad table: one machine, one settings store live at a
 * time. adamsession.h carries the frontend-facing API.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAM_BINDINGS_H
#define ADAM_BINDINGS_H

#include "adamsession.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reloads the table: every slot to its default, then overridden by whatever
 * "bindings" holds in s's settings store (s may be NULL). */
void bindings_init(adamsession *s);

/* The (folded) keysym a target is bound to, 0 for none. */
uint32_t bindings_keysym_of(int target);
/* The gamepad button a target is bound to, ADAM_PAD_BTN_NONE for none. */
int bindings_button_of(int target);
/* Every target a keysym drives, written to out[] (room for
 * ADAM_TARGET_COUNT); returns the count. */
int bindings_targets_for_key(uint32_t keysym, int *out);
/* Case fold used for every keysym comparison. */
uint32_t bindings_fold(uint32_t keysym);

#ifdef __cplusplus
}
#endif

#endif /* ADAM_BINDINGS_H */
