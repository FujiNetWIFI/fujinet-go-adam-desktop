/*
 * KeyForward -- see the header.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeyForward.h"

/* Hardware key codes of the modifier keys (HIToolbox kVK_*; six constants
 * are not worth importing Carbon for). Command is left out on purpose: Cmd
 * chords belong to the menus. */
enum {
    kADAMVKShift = 0x38, kADAMVKOption = 0x3A, kADAMVKControl = 0x3B,
    kADAMVKRightShift = 0x3C, kADAMVKRightOption = 0x3D,
    kADAMVKRightControl = 0x3E
};

/* Which modifier keys are down: a flag stays set while the other side of
 * the pair is still held, so the flag alone cannot tell a press from a
 * release. */
static unsigned g_modDown;

uint32_t ADAMKeysymFromEvent(NSEvent *event)
{
    const uint32_t ks = adamsession_keysym_from_macos_keycode(event.keyCode);
    if (ks)
        return ks;

    /* A key the table does not know: the character typed. */
    NSString *chars = event.charactersIgnoringModifiers;
    if (chars.length == 1) {
        const unichar c = [chars characterAtIndex:0];
        if (c >= 'A' && c <= 'Z')
            return (uint32_t)(c + 32);
        if (c >= 0x20 && c <= 0x7e)
            return (uint32_t)c;
    }
    return 0;
}

uint32_t ADAMKeysymFromFlagsChange(NSEvent *event, int *down)
{
    static const struct {
        unsigned short code;
        NSEventModifierFlags flag;
    } mods[6] = {
        { kADAMVKShift,        NSEventModifierFlagShift },
        { kADAMVKRightShift,   NSEventModifierFlagShift },
        { kADAMVKControl,      NSEventModifierFlagControl },
        { kADAMVKRightControl, NSEventModifierFlagControl },
        /* Option is where a Mac keyboard puts the key a PC calls Alt. */
        { kADAMVKOption,       NSEventModifierFlagOption },
        { kADAMVKRightOption,  NSEventModifierFlagOption },
    };
    const NSEventModifierFlags flags = event.modifierFlags;

    *down = 0;
    for (unsigned i = 0; i < 6; i++) {
        if (event.keyCode != mods[i].code)
            continue;
        const unsigned bit = 1u << i;
        if ((flags & mods[i].flag) && !(g_modDown & bit)) {
            g_modDown |= bit;
            *down = 1;
        } else {
            g_modDown &= ~bit;
        }
        return adamsession_keysym_from_macos_keycode(mods[i].code);
    }
    return 0;
}

BOOL ADAMKeyDown(adamsession *session, NSEvent *event)
{
    NSString *chars = event.charactersIgnoringModifiers;
    unichar c = chars.length ? [chars characterAtIndex:0] : 0;
    const BOOL ctrl =
        (event.modifierFlags & NSEventModifierFlagControl) != 0;
    const BOOL cmd =
        (event.modifierFlags & NSEventModifierFlagCommand) != 0;

    /* Controller targets first: a bound key, a system action, Ctrl+digit.
     * Not under Command: AppKit sends no keyUp for those, so the key would
     * stay held. */
    const uint32_t ks = ADAMKeysymFromEvent(event);
    if (ks && !cmd && adamsession_input_key(session, ks, 1, ctrl ? 1 : 0))
        return YES;

    uint32_t keysym = 0;
    switch (c) {
    case NSUpArrowFunctionKey:    keysym = 0xFF52; break;
    case NSDownArrowFunctionKey:  keysym = 0xFF54; break;
    case NSLeftArrowFunctionKey:  keysym = 0xFF51; break;
    case NSRightArrowFunctionKey: keysym = 0xFF53; break;
    case NSHomeFunctionKey:       keysym = 0xFF50; break;
    case NSInsertFunctionKey:     keysym = 0xFF63; break;
    case NSDeleteFunctionKey:     keysym = 0xFFFF; break; /* forward del */
    case 0x7F:                    keysym = 0xFF08; break; /* backspace */
    case 0x0D: case 0x03:         keysym = 0xFF0D; break;
    case 0x09:                    keysym = 0xFF09; break;
    case 0x1B:                    keysym = 0xFF1B; break;
    default:
        if (c >= NSF1FunctionKey && c <= NSF1FunctionKey + 5)
            keysym = 0xFFBE + (uint32_t)(c - NSF1FunctionKey);
        break;
    }

    uint32_t unicode = (c >= 0x20 && c < 0xF700) ? c : 0;
    int code = adam_key_from_event(keysym ? keysym : unicode, unicode,
                                   ctrl ? 1 : 0);
    if (code >= 0) {
        adamsession_key(session, (uint8_t)code);
        return YES;
    }
    return NO;
}

void ADAMKeyUp(adamsession *session, NSEvent *event)
{
    /* Releases clear exactly what the press asserted; the ADAM keyboard is
     * type-a-byte and needs none. */
    const uint32_t ks = ADAMKeysymFromEvent(event);
    if (ks)
        adamsession_input_key(session, ks, 0, 0);
}

void ADAMFlagsChanged(adamsession *session, NSEvent *event)
{
    int down = 0;
    const uint32_t ks = ADAMKeysymFromFlagsChange(event, &down);
    if (ks)
        adamsession_input_key(session, ks, down, 0);
}

void ADAMKeyFocusLost(adamsession *session)
{
    g_modDown = 0;
    adamsession_release_all(session);
}
