/*
 * vidcap_internal.h -- what session.c calls in vidcap.c. Not API.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAM_VIDCAP_INTERNAL_H
#define ADAM_VIDCAP_INTERNAL_H

#include "adamcore.h"

/* Emulator thread: the new core, or NULL just before it is destroyed. */
void adam_vidcap_core_set(adamcore *core);
/* Emulator thread, top of each frame: follow the tabs' wish for line capture. */
void adam_vidcap_sync(void);
/* Emulator thread, a frame just finished. */
void adam_vidcap_frame_done(void);

#endif
