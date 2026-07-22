/*
 * FujiNet Go Adam -- macOS frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import <Cocoa/Cocoa.h>

#import "AppDelegate.h"

#include <stdio.h>

#include "adamsession.h"

int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;
    @autoreleasepool {
        adamsession *session = adamsession_new(NULL);
        if (!session) {
            fprintf(stderr, "fatal: could not create the session\n");
            return 1;
        }
        adamsession_start_opts opts;
        adamsession_default_opts(session, &opts);
        if (adamsession_start(session, &opts) != 0)
            fprintf(stderr, "session start: %s\n",
                    adamsession_last_error(session));

        NSApplication *app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        AppDelegate *delegate =
            [[AppDelegate alloc] initWithSession:session];
        app.delegate = delegate;
        [app run];

        adamsession_free(session);
    }
    return 0;
}
