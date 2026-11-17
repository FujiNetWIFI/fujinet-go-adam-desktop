/*
 * controllers_test -- the session-level controller model, no machine and no
 * pad: the keyboard, the Controllers window's mouse and a window press are
 * separate sources whose merge the console sees; opposed directions cancel;
 * the keypad strobe carries the lowest held key; the exact controller word;
 * Ctrl + digit chords; a bound key's release clears what its press asserted;
 * system actions fire once on the key and are posted across threads; the
 * "bindings" setting keeps its format (an old file loads unchanged).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "adamsession.h"
#include "test_tmpdir.h"

#define BIT(act) (1u << (act))
#define KP_UP    ADAMSESSION_KEYSYM_KP_8
#define KP_DOWN  ADAMSESSION_KEYSYM_KP_2
#define KP_LEFT  ADAMSESSION_KEYSYM_KP_4
#define KP_RIGHT ADAMSESSION_KEYSYM_KP_6

static int failures;

static void check(int pass, const char *what)
{
    printf("  %s %s\n", pass ? "ok " : "FAIL", what);
    if (!pass) failures++;
}

static void same(const char *what, unsigned got, unsigned want)
{
    check(got == want, what);
    if (got != want) printf("       got 0x%05X want 0x%05X\n", got, want);
}

static adamsession *fresh(const char *tag, adamsession_paths *paths, char *cfg, size_t cfgsz,
                          char *data, size_t datasz)
{
    test_tmpdir(cfg, cfgsz, tag);
    snprintf(data, datasz, "%s-d", cfg);
    test_mkdir(data);
    memset(paths, 0, sizeof *paths);
    paths->config_dir = cfg;
    paths->data_dir = data;
    paths->fujinet_lib = "";
    return adamsession_new(paths);
}

int main(void)
{
    adamsession_paths paths;
    char cfg[512], data[560];
    adamsession *s = fresh("ctl", &paths, cfg, sizeof cfg, data, sizeof data);
    int act = -1;

    if (!s) return 1;

    printf("the target space:\n");
    check(ADAM_PORTS == 2 && ADAM_ACT_PER_PORT == 18 && ADAM_TARGET_COUNT == 38,
          "two controllers of 18 actions and two console actions: 38 targets");
    check(ADAM_TARGET_SYSACT(0) == 36 && ADAM_SYSACT_COUNT == 2, "the console actions follow");

    printf("the merge:\n");
    same("idle", adamsession_buttons_held(s, 0), 0);
    check(adamsession_controller_word(s, 0) == 0x7F7F && adamsession_controller_word(s, 1) == 0x7F7F,
          "both words idle (0x7F7F)");
    adamsession_input_key(s, KP_UP, 1, 0);
    same("a key holds Up", adamsession_buttons_held(s, 0), BIT(ADAM_ACT_UP));
    check(adamsession_controller_word(s, 0) == 0x7E7F, "and the word is 0x7E7F");
    adamsession_pointer_set(s, 0, BIT(ADAM_ACT_FIRE_L));
    same("the mouse adds Fire L", adamsession_buttons_held(s, 0), BIT(ADAM_ACT_UP) | BIT(ADAM_ACT_FIRE_L));
    check(adamsession_controller_word(s, 0) == 0x3E7F, "word 0x3E7F");
    adamsession_input_key(s, KP_UP, 0, 0);
    same("a key release leaves the mouse's hold", adamsession_buttons_held(s, 0), BIT(ADAM_ACT_FIRE_L));
    adamsession_pointer_set(s, 0, 0);
    same("and the mouse lets go", adamsession_buttons_held(s, 0), 0);

    adamsession_input_key(s, KP_UP, 1, 0);
    adamsession_input_key(s, KP_DOWN, 1, 0);
    same("Up and Down together cancel", adamsession_buttons_held(s, 0), 0);
    adamsession_input_key(s, KP_LEFT, 1, 0);
    same("and Left still shows", adamsession_buttons_held(s, 0), BIT(ADAM_ACT_LEFT));
    adamsession_input_key(s, KP_RIGHT, 1, 0);
    same("Left and Right cancel too", adamsession_buttons_held(s, 0), 0);
    adamsession_input_key(s, KP_UP, 0, 0);
    same("Up lets go: Down is back", adamsession_buttons_held(s, 0), BIT(ADAM_ACT_DOWN));
    adamsession_input_key(s, KP_DOWN, 0, 0);
    adamsession_input_key(s, KP_LEFT, 0, 0);
    adamsession_input_key(s, KP_RIGHT, 0, 0);
    same("all released", adamsession_buttons_held(s, 0), 0);

    printf("the keypad strobe:\n");
    adamsession_pointer_set(s, 1, BIT(7) | BIT(3));
    same("only the lowest held key counts", adamsession_buttons_held(s, 1), BIT(3));
    check(adamsession_controller_word(s, 1) == 0x7F73, "word 0x7F73");
    adamsession_pointer_set(s, 1, BIT(ADAM_KEYPAD_KEYS - 1));
    check(adamsession_controller_word(s, 1) == 0x7F7B, "# is 0x7F7B");
    adamsession_pointer_set(s, 1, 0);
    same("port 1 idle", adamsession_buttons_held(s, 0), 0);

    printf("release_all and a window press:\n");
    adamsession_input_key(s, KP_UP, 1, 0);
    adamsession_pointer_set(s, 0, BIT(ADAM_ACT_FIRE_R));
    adamsession_release_all(s);
    same("release_all lets go of the keyboard alone", adamsession_buttons_held(s, 0), BIT(ADAM_ACT_FIRE_R));
    check(adamsession_input_key(s, KP_UP, 0, 0) == 0, "a release after release_all is not consumed");
    adamsession_pointer_set(s, 0, 0);
    adamsession_press(s, ADAM_TARGET_PORT(1, ADAM_ACT_LEFT), 1);
    same("adamsession_press holds a target", adamsession_buttons_held(s, 1), BIT(ADAM_ACT_LEFT));
    adamsession_press(s, ADAM_TARGET_PORT(1, ADAM_ACT_LEFT), 0);
    same("and lets it go", adamsession_buttons_held(s, 1), 0);

    printf("Ctrl + digit:\n");
    check(adamsession_input_key(s, '5', 1, 1) == 1, "Ctrl+5 is consumed");
    same("and dials 5 on controller 1", adamsession_buttons_held(s, 0), BIT(5));
    check(adamsession_input_key(s, '5', 0, 0) == 1, "its release (Ctrl already up) is consumed");
    same("and lets it go", adamsession_buttons_held(s, 0), 0);
    check(adamsession_input_key(s, '5', 1, 0) == 0, "a plain 5 types on the ADAM's keyboard");
    check(adamsession_input_key(s, '5', 0, 0) == 0, "and its release too");

    printf("a release clears what the press asserted:\n");
    adamsession_input_key(s, KP_UP, 1, 0);
    adamsession_binding_set_key(s, ADAM_TARGET_PORT(0, ADAM_ACT_UP), 0, NULL, 0);
    adamsession_input_key(s, KP_UP, 0, 0);
    same("the binding changed meanwhile: Up still let go", adamsession_buttons_held(s, 0), 0);
    adamsession_bindings_reset(s);

    printf("system actions:\n");
    adamsession_binding_set_key(s, ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_GAME), ADAMSESSION_KEYSYM_F9, NULL, 0);
    check(adamsession_input_key(s, ADAMSESSION_KEYSYM_F9, 1, 0) == 1, "a console key is consumed");
    check(adamsession_input_key(s, ADAMSESSION_KEYSYM_F9, 1, 0) == 1, "auto-repeat too");
    check(adamsession_input_key(s, ADAMSESSION_KEYSYM_F9, 0, 0) == 1, "and its release");
    same("it holds nothing", adamsession_buttons_held(s, 0), 0);
    adamsession_bindings_reset(s);
    adamsession_sysaction_post(s, ADAM_SYSACT_RESET_GAME);
    adamsession_sysaction_post(s, ADAM_SYSACT_RESET_COMPUTER);
    adamsession_sysaction_post(s, ADAM_SYSACT_RESET_COMPUTER);
    check(adamsession_sysaction_take(s, &act) == 1 && act == ADAM_SYSACT_RESET_COMPUTER,
          "take returns the lowest pending action");
    check(adamsession_sysaction_take(s, &act) == 1 && act == ADAM_SYSACT_RESET_GAME, "then the next");
    check(adamsession_sysaction_take(s, &act) == 0, "posts of one action coalesce");
    adamsession_sysaction_post(s, 99);
    check(adamsession_sysaction_take(s, &act) == 0, "a bad action is ignored");

    printf("persistence:\n");
    adamsession_binding_set_key(s, ADAM_TARGET_PORT(1, ADAM_ACT_FIRE_L), 'x', NULL, 0);
    check(strstr(adamsession_get_str(s, "bindings", ""), "k:120") != NULL,
          "a changed key is stored in the packed \"bindings\" setting");
    adamsession_free(s);
    {
        /* the stored form is the one the old (0.15.0) releases wrote: a file
         * written then loads as it is, nothing reset */
        adamsession *old;
        char cfg2[512], data2[560];
        adamsession_paths p2;
        FILE *ini;
        char path[640];

        test_tmpdir(cfg2, sizeof cfg2, "ctl-old");
        snprintf(data2, sizeof data2, "%s-d", cfg2);
        test_mkdir(data2);
        snprintf(path, sizeof path, "%s/settings.ini", cfg2);
        ini = fopen(path, "w");
        if (ini) {
            /* port 2's Fire R on 'x' (target 18+17), port 1's Up cleared */
            fprintf(ini, "bindings=%d.k:120 %d.k:0\n", ADAM_TARGET_PORT(1, ADAM_ACT_FIRE_R),
                    ADAM_TARGET_PORT(0, ADAM_ACT_UP));
            fclose(ini);
        }
        memset(&p2, 0, sizeof p2);
        p2.config_dir = cfg2;
        p2.data_dir = data2;
        p2.fujinet_lib = "";
        old = adamsession_new(&p2);
        check(old != NULL, "a session opens over an old settings file");
        if (old) {
            check(adamsession_target_for_key(old, 'x') == ADAM_TARGET_PORT(1, ADAM_ACT_FIRE_R),
                  "its key binding is applied");
            check(adamsession_binding_get(old, ADAM_TARGET_PORT(0, ADAM_ACT_UP)).keysym == 0,
                  "its cleared binding stays cleared");
            check(adamsession_binding_get(old, ADAM_TARGET_PORT(0, ADAM_ACT_FIRE_L)).keysym
                  == ADAMSESSION_KEYSYM_KP_0,
                  "and the rest keep the defaults");
            adamsession_free(old);
        }
    }

    printf("controllers_test: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
