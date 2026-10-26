/*
 * The keyboard path shared by the machine window and the keypad panel: AppKit
 * key events to X11 keysyms, then the session's input_key first (bound
 * controller keys, system actions, the Ctrl+digit chord) and the ADAM
 * keyboard for whatever it does not consume.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>
#include <stdint.h>

#include "adamsession.h"

/* The keysym of a keyDown/keyUp event, by hardware key code through the
 * session's HID table (a bound key is the physical key: Keypad 8 is not the
 * 8 on the number row, and Shift does not change it), or 0. */
uint32_t ADAMKeysymFromEvent(NSEvent *event);

/* The keysym of a modifier key (flagsChanged), or 0 for Command, Caps Lock
 * and Fn. Sets *down. Keeps state: call once per event. */
uint32_t ADAMKeysymFromFlagsChange(NSEvent *event, int *down);

/* keyDown. YES when the key was consumed (a controller target) or typed on
 * the ADAM; NO when it maps to nothing. */
BOOL ADAMKeyDown(adamsession *session, NSEvent *event);
void ADAMKeyUp(adamsession *session, NSEvent *event);

/* A modifier key press or release (flagsChanged). */
void ADAMFlagsChanged(adamsession *session, NSEvent *event);

/* Key focus lost: release everything the keyboard holds. */
void ADAMKeyFocusLost(adamsession *session);
