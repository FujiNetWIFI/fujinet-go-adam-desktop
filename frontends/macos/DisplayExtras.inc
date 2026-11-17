/*
 * DisplayExtras.inc -- the ADAM keyboard and the aspect modes, included into
 * the display view's @implementation (see DisplayGlue.h).
 *
 * Keyboard events go through KeyForward: the session's controller bindings
 * first (Ctrl+digit presses the game-controller keypad, as in the other
 * frontends), the ADAM keyboard for the rest.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

- (void)extraInit {}

/* The frame clock follows the window (viewDidMoveToWindow); the app asks for
 * it explicitly once the window is ordered in. */
- (void)start { [self updateVisibility]; }

/* 0: square pixels, 1: the television's 4:3, 2: whole-number scale. */
- (void)setAspectMode:(int)mode
{
    atomic_store(&s_fn_aspect_mode, mode);
    atomic_store(&_redraw, 1);
    [self setNeedsDisplay:YES];
}

/* ---- input ---------------------------------------------------------------- */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

/* Losing the keyboard with keys held would leave the machine believing they
 * are still down. (Losing key status altogether is the window delegate's.) */
- (BOOL)resignFirstResponder
{
    ADAMKeyFocusLost(_session);
    return YES;
}

- (void)keyDown:(NSEvent *)event
{
    if (!ADAMKeyDown(_session, event))
        [super keyDown:event];
}

- (void)keyUp:(NSEvent *)event
{
    ADAMKeyUp(_session, event);
}

/* Modifier keys arrive as flagsChanged, not keyDown/keyUp; a binding moved
 * onto Shift, Control or Option would otherwise never press. */
- (void)flagsChanged:(NSEvent *)event
{
    ADAMFlagsChanged(_session, event);
}
