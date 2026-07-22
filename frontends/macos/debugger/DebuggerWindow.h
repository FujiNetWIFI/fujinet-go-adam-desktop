/*
 * Debugger window for the macOS frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#include "adamsession.h"

@interface DebuggerWindow : NSObject

/* Shows (creating on first use) the debugger for the session. */
+ (void)showForSession:(adamsession *)session;

@end
