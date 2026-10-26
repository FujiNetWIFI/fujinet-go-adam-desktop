/*
 * Controller bindings and input merging: the default numpad map, stealing
 * and clearing, persistence, the Ctrl+digit chord, and that the keyboard,
 * the keypad window and a gamepad cannot cancel each other's holds.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "session_internal.h"
#include "test_tmp.h"

static int failures;

#define CHECK(what, cond)                                       \
    do {                                                        \
        if (!(cond)) {                                          \
            fprintf(stderr, "FAIL %s\n", what);                 \
            failures++;                                         \
        }                                                       \
    } while (0)

#define UP ADAM_TARGET_PORT(0, ADAM_ACT_UP)
#define FIRE_L ADAM_TARGET_PORT(0, ADAM_ACT_FIRE_L)
#define BIT(act) (1u << (act))

int main(void)
{
    char tmpl[512];
    char *tmp = adam_make_tempdir(tmpl, sizeof(tmpl), "adambind-test-");
    adamsession_paths paths;
    adamsession *s;
    char stolen[128], name[64];

    if (!tmp) return 1;
    memset(&paths, 0, sizeof paths);
    paths.config_dir = tmp;
    paths.data_dir = tmp;
    paths.fujinet_lib = "";
    s = adamsession_new(&paths);
    if (!s) return 1;

    /* defaults */
    CHECK("KP_8 = up", adamsession_target_for_key(s, ADAMSESSION_KEYSYM_KP_8) == UP);
    CHECK("KP_Enter = fire R", adamsession_target_for_key(s, ADAMSESSION_KEYSYM_KP_ENTER) == ADAM_TARGET_PORT(0, ADAM_ACT_FIRE_R));
    CHECK("KP_* = keypad *", adamsession_target_for_key(s, ADAMSESSION_KEYSYM_KP_MULTIPLY) == ADAM_TARGET_PORT(0, 10));
    CHECK("KP_/ = keypad #", adamsession_target_for_key(s, ADAMSESSION_KEYSYM_KP_DIVIDE) == ADAM_TARGET_PORT(0, 11));
    CHECK("top-row 8 is the keyboard's", adamsession_target_for_key(s, '8') == -1);
    CHECK("'a' is the keyboard's", adamsession_target_for_key(s, 'a') == -1);
    CHECK("pad south = fire L", adamsession_target_for_button(s, 0, ADAM_PAD_BTN_SOUTH) == FIRE_L);
    CHECK("pad south port 2", adamsession_target_for_button(s, 1, ADAM_PAD_BTN_SOUTH) == ADAM_TARGET_PORT(1, ADAM_ACT_FIRE_L));

    /* keys drive holds; auto-repeat is one press; release clears */
    CHECK("KP_8 consumed", adamsession_input_key(s, ADAMSESSION_KEYSYM_KP_8, 1, 0) == 1);
    CHECK("up held", adamsession_buttons_held(s, 0) == BIT(ADAM_ACT_UP));
    CHECK("repeat consumed", adamsession_input_key(s, ADAMSESSION_KEYSYM_KP_8, 1, 0) == 1);
    CHECK("other port idle", adamsession_buttons_held(s, 1) == 0);
    CHECK("release consumed", adamsession_input_key(s, ADAMSESSION_KEYSYM_KP_8, 0, 0) == 1);
    CHECK("up released", adamsession_buttons_held(s, 0) == 0);
    CHECK("unbound key not consumed", adamsession_input_key(s, 'a', 1, 0) == 0);
    CHECK("unbound release not consumed", adamsession_input_key(s, 'a', 0, 0) == 0);

    /* sources do not cancel each other */
    adamsession_press(s, UP, 1);
    adamsession_input_key(s, ADAMSESSION_KEYSYM_KP_8, 1, 0);
    adamsession_input_key(s, ADAMSESSION_KEYSYM_KP_8, 0, 0);
    CHECK("window hold survives a key release", adamsession_buttons_held(s, 0) == BIT(ADAM_ACT_UP));
    adamsession_press(s, UP, 0);
    CHECK("window release", adamsession_buttons_held(s, 0) == 0);

    /* keypad keys */
    adamsession_press(s, ADAM_TARGET_PORT(1, 7), 1);
    CHECK("port 2 key 7", adamsession_buttons_held(s, 1) == BIT(7));
    adamsession_press(s, ADAM_TARGET_PORT(1, 7), 0);

    /* Ctrl+digit chord: port 1 keypad digit, released without Ctrl */
    CHECK("ctrl+5 consumed", adamsession_input_key(s, '5', 1, 1) == 1);
    CHECK("ctrl+5 held", adamsession_buttons_held(s, 0) == BIT(5));
    CHECK("release without ctrl", adamsession_input_key(s, '5', 0, 0) == 1);
    CHECK("chord released", adamsession_buttons_held(s, 0) == 0);
    CHECK("plain 5 types", adamsession_input_key(s, '5', 1, 0) == 0);

    /* stealing, clearing, case folding, persistence */
    adamsession_binding_set_key(s, ADAM_TARGET_PORT(1, ADAM_ACT_UP), 'W', stolen, sizeof stolen);
    CHECK("W binds as w", adamsession_target_for_key(s, 'w') == ADAM_TARGET_PORT(1, ADAM_ACT_UP));
    adamsession_binding_set_key(s, FIRE_L, 'w', stolen, sizeof stolen);
    CHECK("steal names the old holder", strcmp(stolen, "Controller 2: Up") == 0);
    CHECK("old holder cleared", adamsession_binding_get(s, ADAM_TARGET_PORT(1, ADAM_ACT_UP)).keysym == 0);
    CHECK("old key freed", adamsession_target_for_key(s, ADAMSESSION_KEYSYM_KP_0) == -1);
    CHECK("persisted", adamsession_get_str(s, "bindings", "")[0] != '\0');
    adamsession_binding_set_key(s, FIRE_L, 0, NULL, 0);
    CHECK("clear unbinds", adamsession_binding_get(s, FIRE_L).keysym == 0);
    adamsession_binding_set_button(s, FIRE_L, ADAM_PAD_BTN_NONE, NULL, 0);
    CHECK("clear button", adamsession_binding_get(s, FIRE_L).button == ADAM_PAD_BTN_NONE);
    adamsession_bindings_reset(s);
    CHECK("reset restores defaults", adamsession_target_for_key(s, ADAMSESSION_KEYSYM_KP_0) == FIRE_L
                                     && adamsession_binding_get(s, FIRE_L).button == ADAM_PAD_BTN_SOUTH);
    CHECK("reset clears the store", adamsession_get_str(s, "bindings", "x")[0] == '\0');

    /* a rebinding made before a press releases what the press asserted */
    adamsession_input_key(s, ADAMSESSION_KEYSYM_KP_8, 1, 0);
    adamsession_binding_set_key(s, UP, 'q', NULL, 0);
    adamsession_input_key(s, ADAMSESSION_KEYSYM_KP_8, 0, 0);
    CHECK("release after rebind", adamsession_buttons_held(s, 0) == 0);
    adamsession_bindings_reset(s);

    /* system actions fire on the key and are consumed */
    adamsession_binding_set_key(s, ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_GAME), ADAMSESSION_KEYSYM_F9, NULL, 0);
    CHECK("sysaction key consumed", adamsession_input_key(s, ADAMSESSION_KEYSYM_F9, 1, 0) == 1);
    adamsession_input_key(s, ADAMSESSION_KEYSYM_F9, 0, 0);
    adamsession_bindings_reset(s);

    /* release_all */
    adamsession_input_key(s, ADAMSESSION_KEYSYM_KP_4, 1, 0);
    adamsession_press(s, ADAM_TARGET_PORT(0, 3), 1);
    adamsession_release_all(s);
    CHECK("release_all drops the keyboard only", adamsession_buttons_held(s, 0) == BIT(3));
    adamsession_press(s, ADAM_TARGET_PORT(0, 3), 0);

    /* names and key tables */
    adamsession_keysym_name('a', name, sizeof name);
    CHECK("name a", strcmp(name, "A") == 0);
    adamsession_keysym_name(ADAMSESSION_KEYSYM_KP_ENTER, name, sizeof name);
    CHECK("name KP_Enter", strcmp(name, "Keypad Enter") == 0);
    adamsession_keysym_name(ADAMSESSION_KEYSYM_F9, name, sizeof name);
    CHECK("name F9", strcmp(name, "F9") == 0);
    CHECK("evdev A", adamsession_keysym_from_evdev(30) == 'a');
    CHECK("evdev left ctrl", adamsession_keysym_from_evdev(29) == ADAMSESSION_KEYSYM_LCTRL);
    CHECK("win scan code Z", adamsession_keysym_from_win_scancode(0x2C, 0) == 'z');
    CHECK("target names", strcmp(adam_target_name(FIRE_L), "Controller 1: Fire L") == 0);
    CHECK("keypad names", strcmp(adam_target_short_name(ADAM_TARGET_PORT(1, 10)), "Key *") == 0);

    adamsession_free(s);
    if (failures) {
        fprintf(stderr, "bindings_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("bindings_test: all passed\n");
    return 0;
}
