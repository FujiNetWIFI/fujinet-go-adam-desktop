/*
 * keyboard_test -- the on-screen ADAM keyboard: 75 caps, none overlapping,
 * every code the one the host keyboard path sends for the same key, and the
 * SHIFT / CONTROL / LOCK latches. No machine: keyboard_press_code returns the
 * byte adamsession_keyboard_press would send.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "session_internal.h"
#include "test_tmpdir.h"

static int failures;

static void check(int pass, const char *what)
{
    printf("  %s %s\n", pass ? "ok " : "FAIL", what);
    if (!pass) failures++;
}

static int find(const char *label, int nth)
{
    int count, idx;
    const adam_keycap *caps = adamsession_keyboard_layout(&count, NULL, NULL);
    for (idx = 0; idx < count; idx++)
        if (!strcmp(caps[idx].label, label) && nth-- == 0)
            return idx;
    return -1;
}

/* press + release cap `label`; the byte sent, or -1 */
static int tap(adamsession *s, const char *label)
{
    const int idx = find(label, 0);
    int code;
    if (idx < 0) return -2;
    code = keyboard_press_code(s, idx, 1);
    keyboard_press_code(s, idx, 0);
    return code;
}

int main(void)
{
    adamsession_paths paths;
    char cfg[512], data[560];
    adamsession *s;
    int count, idx, other;
    float wide, high;
    const adam_keycap *caps = adamsession_keyboard_layout(&count, &wide, &high);

    test_tmpdir(cfg, sizeof cfg, "kbd");
    snprintf(data, sizeof data, "%s-d", cfg);
    test_mkdir(data);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = adamsession_new(&paths);
    if (!s) return 1;

    printf("the layout:\n");
    check(count == 75, "75 caps, as on the real keyboard");
    check(count <= ADAM_KBD_CAPS_MAX, "within the session's per-cap state");
    check(wide > 20.0f && high > 6.0f, "an extent for the toolkit to size by");
    {
        int bad = 0;
        for (idx = 0; idx < count; idx++) {
            const adam_keycap *a = &caps[idx];
            if (a->left < 0 || a->top < 0 || a->left + a->wide > wide + 0.001f
                || a->top + a->high > high + 0.001f)
                bad++;
            for (other = idx + 1; other < count; other++) {
                const adam_keycap *b = &caps[other];
                if (a->left < b->left + b->wide - 0.001f && b->left < a->left + a->wide - 0.001f
                    && a->top < b->top + b->high - 0.001f && b->top < a->top + a->high - 0.001f) {
                    printf("       %s overlaps %s\n", a->label, b->label);
                    bad++;
                }
            }
        }
        check(bad == 0, "every cap is inside the extent and none overlap");
    }

    printf("what the caps send:\n");
    check(tap(s, "A") == 'a' && tap(s, "Z") == 'z' && tap(s, "5") == '5' && tap(s, "SPACE") == ' ',
          "letters lower case, digits, space");
    check(tap(s, "RETURN") == 0x0D && tap(s, "BACK SPACE") == 0x08 && tap(s, "TAB") == 0x09
          && tap(s, "ESCAPE/WP") == 0x1B, "RETURN, BACK SPACE, TAB, ESCAPE");
    check(tap(s, "I") == 0x81 && tap(s, "VI") == 0x86, "SmartKeys I and VI are 0x81 and 0x86");
    check(tap(s, "WILD CARD") == 0x90 && tap(s, "UNDO") == 0x91 && tap(s, "MOVE/COPY") == 0x92
          && tap(s, "STORE/GET") == 0x93 && tap(s, "INSERT") == 0x94 && tap(s, "PRINT") == 0x95
          && tap(s, "CLEAR") == 0x96 && tap(s, "DELETE") == 0x97, "the editing block, 0x90-0x97");
    check(tap(s, "\xE2\x86\x91") == 0xA0 && tap(s, "\xE2\x86\x92") == 0xA1
          && tap(s, "\xE2\x86\x93") == 0xA2 && tap(s, "\xE2\x86\x90") == 0xA3
          && tap(s, "HOME") == 0xA8, "the arrows 0xA0-0xA3 (up right down left) and HOME 0xA8");
    /* the same bytes the host keyboard sends for the same keys */
    check(adam_key_from_event(0xFF63, 0, 0) == tap(s, "INSERT")
          && adam_key_from_event(0xFFFF, 0, 0) == tap(s, "DELETE")
          && adam_key_from_event(0xFF50, 0, 0) == tap(s, "HOME")
          && adam_key_from_event(0xFFBE, 0, 0) == tap(s, "I")
          && adam_key_from_event(0xFF52, 0, 0) == tap(s, "\xE2\x86\x91"),
          "and they agree with adam_key_from_event");
    check(keyboard_press_code(s, find("SHIFT", 0), 1) == -1 && keyboard_press_code(s, 9999, 1) == -1,
          "a modifier and a bad index send nothing");
    keyboard_press_code(s, find("SHIFT", 0), 0);
    adamsession_keyboard_release(s);

    printf("the latches:\n");
    keyboard_press_code(s, find("SHIFT", 0), 1);
    check(adamsession_keyboard_held(s, find("SHIFT", 0)) & 2, "SHIFT lights while latched");
    check(adamsession_keyboard_held(s, find("SHIFT", 1)) & 2, "and so does the other SHIFT: one latch");
    check(tap(s, "A") == 'A', "SHIFT shifts the next key");
    keyboard_press_code(s, find("SHIFT", 0), 1);
    check(tap(s, "1") == '!', "digits give their shifted legend");
    check(tap(s, "A") == 'a' && !(adamsession_keyboard_held(s, find("SHIFT", 0)) & 2),
          "and is spent by it");
    keyboard_press_code(s, find("SHIFT", 1), 1);
    keyboard_press_code(s, find("SHIFT", 0), 1);
    check(tap(s, "A") == 'a', "pressing a latched SHIFT again lets it go");
    keyboard_press_code(s, find("SHIFT", 1), 1);
    check(tap(s, "DELETE") == 0x9F && (keyboard_press_code(s, find("SHIFT", 0), 1), tap(s, "HOME")) == 0xA9,
          "SHIFT + the editing block is +8, SHIFT + HOME 0xA9");
    adamsession_keyboard_release(s);
    keyboard_press_code(s, find("CONTROL", 0), 1);
    check(tap(s, "C") == 0x03 && tap(s, "C") == 'c', "CONTROL + C is 0x03, then it is spent");
    keyboard_press_code(s, find("CONTROL", 0), 1);
    check(tap(s, "RETURN") == 0x0D, "CONTROL leaves a non-letter alone");
    keyboard_press_code(s, find("LOCK", 0), 1);
    check(adamsession_keyboard_held(s, find("LOCK", 0)) & 2, "LOCK lights");
    check(tap(s, "A") == 'A' && tap(s, "B") == 'B' && tap(s, "7") == '7', "LOCK makes capitals and keeps going");
    keyboard_press_code(s, find("LOCK", 0), 1);
    check(tap(s, "A") == 'a', "a second press releases it");
    adamsession_keyboard_release(s);

    printf("the flash:\n");
    {
        const int idxa = find("A", 0);
        keyboard_press_code(s, idxa, 1);
        check(adamsession_keyboard_held(s, idxa) & 1, "a cap under the mouse is lit");
        keyboard_press_code(s, idxa, 0);
        check(adamsession_keyboard_held(s, idxa) & 1, "and stays lit a moment after");
        adamsession_keyboard_release(s);
        check(adamsession_keyboard_held(s, idxa) == 0, "keyboard_release clears it");
    }

    adamsession_free(s);
    printf("keyboard_test: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
