/*
 * Keyboard path shared by the main window and the keypad window.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAM_WIN_KEYS_H
#define ADAM_WIN_KEYS_H

#include <stdint.h>
#include <windows.h>

#include "adamsession.h"

/* The keysym of the physical key a WM_KEYDOWN/WM_KEYUP reports (its scan
 * code, so Map mode and the bindings see one key the same way whatever the
 * layout or NumLock say), or 0 if the key has none. */
uint32_t adam_win_keysym(WPARAM vk, LPARAM lp);

/* Runs a WM_KEYDOWN / WM_SYSKEYDOWN / WM_KEYUP / WM_SYSKEYUP through the
 * session: adamsession_input_key first (a bound key drives a controller
 * target and is not typed), then the EOS special keys. Returns 1 when the
 * key was consumed (the window proc returns 0), 0 to let default processing
 * go on (menu mnemonics, Alt+F4). */
int adam_win_key_msg(adamsession *session, UINT msg, WPARAM wp, LPARAM lp);

/* A WM_CHAR: types it on the ADAM's keyboard, unless the key that produced
 * it was consumed by adam_win_key_msg. */
void adam_win_char(adamsession *session, WPARAM ch);

#endif
