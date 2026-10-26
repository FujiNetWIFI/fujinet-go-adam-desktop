/*
 * The app's accent colour for the macOS frontend (ADAMSESSION_ACCENT_RGB, the
 * icon's background): toggled-on buttons, the armed Map button, the debugger's
 * current-line highlight. It is light, so text on it is black.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

NSColor *ADAMAccentColor(void);

/* Toggled-on look for a push button: accent bezel and a black title. Off
 * restores the plain bezel and title. Call again after setting a new title
 * on a button that is on (a plain title drops the attributed one). */
void ADAMSetButtonAccent(NSButton *button, BOOL on);
