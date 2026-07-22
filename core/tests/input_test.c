/*
 * Input-encoding unit tests, mirroring the Android app's
 * AdamControllerTest.kt and HardwareKeyboardTest.kt so the C port stays
 * bit-exact with what the Kotlin layer sends.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "adamsession.h"

static int failures;

#define CHECK_EQ(what, got, want)                                          \
    do {                                                                   \
        long g = (long)(got), w = (long)(want);                            \
        if (g != w) {                                                      \
            fprintf(stderr, "FAIL %s: got 0x%lX want 0x%lX\n", what, g, w); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void test_controller_encode(void)
{
    CHECK_EQ("idle", adam_controller_encode(0, 0, 0, 0, 0, 0, -1), 0x7F7F);
    CHECK_EQ("up", adam_controller_encode(1, 0, 0, 0, 0, 0, -1), 0x7E7F);
    CHECK_EQ("down", adam_controller_encode(0, 1, 0, 0, 0, 0, -1), 0x7B7F);
    CHECK_EQ("left", adam_controller_encode(0, 0, 1, 0, 0, 0, -1), 0x777F);
    CHECK_EQ("right", adam_controller_encode(0, 0, 0, 1, 0, 0, -1), 0x7D7F);
    CHECK_EQ("fireL", adam_controller_encode(0, 0, 0, 0, 1, 0, -1), 0x3F7F);
    CHECK_EQ("fireR", adam_controller_encode(0, 0, 0, 0, 0, 1, -1), 0x7F3F);
    CHECK_EQ("up+right+fireL",
             adam_controller_encode(1, 0, 0, 1, 1, 0, -1), 0x3C7F);
    CHECK_EQ("keypad 0", adam_controller_encode(0, 0, 0, 0, 0, 0, 0), 0x7F70);
    CHECK_EQ("keypad 5", adam_controller_encode(0, 0, 0, 0, 0, 0, 5), 0x7F75);
    CHECK_EQ("keypad 11 (#)",
             adam_controller_encode(0, 0, 0, 0, 0, 0, 11), 0x7F7B);
    CHECK_EQ("keypad 13 passes",
             adam_controller_encode(0, 0, 0, 0, 0, 0, 13), 0x7F7D);
    CHECK_EQ("keypad 14 out of range",
             adam_controller_encode(0, 0, 0, 0, 0, 0, 14), 0x7F7F);
    CHECK_EQ("keypad with fireR",
             adam_controller_encode(0, 0, 0, 0, 0, 1, 7), 0x7F37);
}

static void test_key_mapping(void)
{
    /* Special keys (Android specialAdamCode parity). */
    CHECK_EQ("Return", adam_key_from_event(0xFF0D, 0, 0), 0x0D);
    CHECK_EQ("KP_Enter", adam_key_from_event(0xFF8D, 0, 0), 0x0D);
    CHECK_EQ("Escape", adam_key_from_event(0xFF1B, 0, 0), 0x1B);
    CHECK_EQ("Tab", adam_key_from_event(0xFF09, 0, 0), 0x09);
    CHECK_EQ("BackSpace", adam_key_from_event(0xFF08, 0, 0), 0x08);
    CHECK_EQ("Delete", adam_key_from_event(0xFFFF, 0, 0), 0x97);
    CHECK_EQ("Insert", adam_key_from_event(0xFF63, 0, 0), 0x94);
    CHECK_EQ("Undo", adam_key_from_event(0xFF65, 0, 0), 0x91);
    CHECK_EQ("Home", adam_key_from_event(0xFF50, 0, 0), 0xA8);
    CHECK_EQ("Up", adam_key_from_event(0xFF52, 0, 0), 0xA0);
    CHECK_EQ("Right", adam_key_from_event(0xFF53, 0, 0), 0xA1);
    CHECK_EQ("Down", adam_key_from_event(0xFF54, 0, 0), 0xA2);
    CHECK_EQ("Left", adam_key_from_event(0xFF51, 0, 0), 0xA3);

    /* Desktop additions: F1-F6 = SmartKeys I-VI, keypad digits. */
    CHECK_EQ("F1=Smart I", adam_key_from_event(0xFFBE, 0, 0), 0x81);
    CHECK_EQ("F6=Smart VI", adam_key_from_event(0xFFC3, 0, 0), 0x86);
    CHECK_EQ("F7 unmapped", adam_key_from_event(0xFFC4, 0, 0), -1);
    CHECK_EQ("KP_5", adam_key_from_event(0xFFB5, 0, 0), '5');
    CHECK_EQ("KP_Multiply", adam_key_from_event(0xFFAA, 0, 0), '*');

    /* Ctrl + letter -> control code, either letter case. */
    CHECK_EQ("Ctrl-c", adam_key_from_event('c', 'c', 1), 0x03);
    CHECK_EQ("Ctrl-C", adam_key_from_event('C', 'C', 1), 0x03);
    CHECK_EQ("Ctrl-z", adam_key_from_event('z', 'z', 1), 0x1A);
    CHECK_EQ("Ctrl-5 ignored", adam_key_from_event('5', '5', 1), '5');

    /* Printable keys pass through as (possibly shifted) ASCII. */
    CHECK_EQ("a", adam_key_from_event('a', 'a', 0), 'a');
    CHECK_EQ("A", adam_key_from_event('A', 'A', 0), 'A');
    CHECK_EQ("space", adam_key_from_event(' ', ' ', 0), ' ');
    CHECK_EQ("tilde", adam_key_from_event('~', '~', 0), '~');

    /* Not forwarded: bare modifiers / non-ASCII. */
    CHECK_EQ("Shift_L", adam_key_from_event(0xFFE1, 0, 0), -1);
    CHECK_EQ("F12", adam_key_from_event(0xFFC9, 0, 0), -1);
}

int main(void)
{
    test_controller_encode();
    test_key_mapping();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("input_test: all checks passed\n");
    return 0;
}
