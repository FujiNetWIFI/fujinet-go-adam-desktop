/*
 * The AppKit debugger window over the Z80 debugger engine (adamdebug.h).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "adamsession.h"

@interface DebuggerWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate>
/* Shows (creating on first use) the debugger for the session; attaching the
 * engine stops the machine, as the other frontends do. */
+ (void)showForSession:(adamsession *)session;
/* F12: shows it, or closes it (detaching, so the machine runs on). */
+ (void)toggleForSession:(adamsession *)session;
@end
