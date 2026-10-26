/*
 * The controllers panel: two ColecoVision controllers (keypad, stick, two
 * fire buttons each), the system actions and the Map row, in a fixed-size
 * floating panel.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "adamsession.h"

@interface ADAMKeypadWindow : NSWindowController <NSWindowDelegate>

/* Shows the panel (creating it on first use), or hides it if it is up. */
+ (void)toggleWithSession:(adamsession *)session;
+ (BOOL)isVisible;

@end
