/*
 * gamepad_map_test -- the gamepad layer end to end with SDL3 virtual
 * joysticks: hot-plug pickup, the default mapping onto the hand controller
 * (the D-pad and the left stick steer, South and East fire, West and North
 * are keypad 1 and 2, the shoulders * and #), a button rebound to a keypad
 * key and to the console actions, a trigger past half travel (which must not
 * take the pad lock twice), map-mode capture, a raw HID adapter (an
 * unmapped joystick: raw buttons, a hat that steers and is bindable), the
 * sources not stepping on each other, and hot-unplug releasing everything the
 * pad held -- with no hardware, no human and no machine. Then the pad's port
 * assignment: written to the settings ("pad_assign") when made, restored when
 * a new session (a restart) finds the pad again, found through a stale GUID
 * by the name, kept through garbage, removed by Automatic, and the toasts'
 * text and generation.
 *
 * What the pad holds is read back through adamsession_buttons_held, which
 * reports the same per-source bits the machine is driven from. Every wait
 * polls with a five second ceiling, so a slow runner is merely slow. SKIPs
 * (77) where SDL has no virtual joysticks or real pads leave no port free.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session_internal.h"
#include "test_tmpdir.h"

#define PAD_NAME "adamtest virtual pad"
#define RAW_NAME "adamtest raw adapter"

#define ACT_BIT(act) (1u << (act))
#define KEYPAD_BIT(num) (1u << (ADAM_ACT_KEYPAD + (num)))
#define PAD_BTN(name) SDL_GAMEPAD_BUTTON_##name

static int failures;

static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

static void sleep_ms(int msec)
{
    struct timespec stamp = { msec / 1000, (long)(msec % 1000) * 1000000L };
    nanosleep(&stamp, NULL);
}

/* Waits for the controller to hold exactly `want`. The pad thread syncs
 * devices a few times a second and polls state faster; give it five. */
static void expect(adamsession *sess, int port, unsigned want, const char *what)
{
    unsigned held = 0;
    int tries;
    for (tries = 0; tries < 250; tries++) {
        held = adamsession_buttons_held(sess, port);
        if (held == want) {
            printf("ok: %s\n", what);
            return;
        }
        sleep_ms(20);
    }
    printf("FAIL: %s (held %05X want %05X)\n", what, held, want);
    failures++;
}

static SDL_JoystickID attach_pad(const char *name, int raw)
{
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    if (raw) {
        desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
        desc.naxes = 2;
        desc.nbuttons = 8;
        desc.nhats = 1;
    } else {
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    }
    desc.name = name;
    return SDL_AttachVirtualJoystick(&desc);
}

/* The pad called `name` in the session, or -1. */
static int pad_slot_now(adamsession *sess, const char *name)
{
    int idx;
    for (idx = 0; idx < adamsession_gamepad_count(sess); idx++) {
        char got[64];
        adamsession_gamepad_name(sess, idx, got, sizeof got);
        if (strcmp(got, name) == 0)
            return idx;
    }
    return -1;
}

/* The slot once the pad thread has found the pad (polled, up to five
 * seconds: a slow runner is slow to enumerate), or -1. */
static int wait_for_pad(adamsession *sess, const char *name)
{
    int idx = pad_slot_now(sess, name), tries;
    for (tries = 0; tries < 250 && idx < 0; tries++) {
        sleep_ms(20);
        idx = pad_slot_now(sess, name);
    }
    return idx;
}

static void wait_gone(adamsession *sess, const char *name)
{
    int tries;
    for (tries = 0; tries < 250 && pad_slot_now(sess, name) >= 0; tries++)
        sleep_ms(20);
}

/* The ADAM's defaults bind only the D-pad and the two fire buttons; the rest
 * of this test's "defaults" (West/North = keypad 1/2, the shoulders = * and
 * #, W for controller 2's Up) are bound here so the same checks run. */
static void pad_defaults(adamsession *sess)
{
    int port;
    adamsession_bindings_reset(sess);
    for (port = 0; port < ADAM_PORTS; port++) {
        adamsession_binding_set_button(sess, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + 1), ADAM_PAD_BTN_WEST, NULL, 0);
        adamsession_binding_set_button(sess, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + 2), ADAM_PAD_BTN_NORTH, NULL, 0);
        adamsession_binding_set_button(sess, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + 10), ADAM_PAD_BTN_LEFT_SHOULDER, NULL, 0);
        adamsession_binding_set_button(sess, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + 11), ADAM_PAD_BTN_RIGHT_SHOULDER, NULL, 0);
    }
    adamsession_binding_set_key(sess, ADAM_TARGET_PORT(1, ADAM_ACT_UP), 'w', NULL, 0);
}

static adamsession *session_up(const adamsession_paths *paths)
{
    adamsession *sess = adamsession_new(paths);
    if (!sess)
        return NULL;
    if (gamepad_start(sess) != 0) {
        printf("SKIP: the gamepad layer would not start: %s\n", adamsession_last_error(sess));
        adamsession_free(sess);
        return NULL;
    }
    pad_defaults(sess);
    return sess;
}

static int wait_sysaction(adamsession *sess, int want)
{
    int tries, act = -1;
    for (tries = 0; tries < 250; tries++) {
        if (adamsession_sysaction_take(sess, &act))
            return act == want;
        sleep_ms(20);
    }
    return 0;
}

/* The pad_assign setting with the pad's entry in it? */
static int has_entry(adamsession *sess, const char *guid, int port)
{
    char want[160];
    snprintf(want, sizeof want, "%s|%s=%d", guid, PAD_NAME, port);
    return strstr(adamsession_get_str(sess, "pad_assign", ""), want) != NULL;
}

/* A pad's port assignment survives a restart. The virtual pad stays attached
 * across the sessions (SDL is held open by the caller). */
static void persistence(const adamsession_paths *paths)
{
    SDL_JoystickID vid = attach_pad(PAD_NAME, 0);
    adamsession *sess;
    char guid[64], text[160];
    int idx, want, auto_port, tries;
    unsigned gen_before;

    if (!vid) {
        printf("FAIL: the virtual pad would not attach again: %s\n", SDL_GetError());
        failures++;
        return;
    }
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(vid), guid, sizeof guid);

    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0, "persistence: the pad is picked up");
    if (idx < 0) { adamsession_free(sess); SDL_DetachVirtualJoystick(vid); return; }
    check(adamsession_gamepad_assignment(sess, idx) == -1, "a pad nobody assigned is automatic");
    check(strstr(adamsession_get_str(sess, "pad_assign", ""), guid) == NULL, "and has no entry");
    auto_port = adamsession_gamepad_effective_port(sess, idx);
    want = auto_port == 0 ? 1 : 0;

    adamsession_gamepad_last_event(sess, text, sizeof text);
    check(strstr(text, "Connected: " PAD_NAME) == text, "the toast says the pad connected");
    check(auto_port < 0 || strstr(text, auto_port ? "(controller 2)" : "(controller 1)") != NULL,
          "and which controller it drives");

    adamsession_gamepad_assign(sess, idx, want);
    check(adamsession_gamepad_assignment(sess, idx) == want
          && adamsession_gamepad_effective_port(sess, idx) == want, "assigning drives the other port");
    check(has_entry(sess, guid, want), "and writes <guid>|<name>=<port> to pad_assign");

    /* the file itself, not just the in-memory store */
    {
        char path[600], line[16384];
        int found = 0;
        FILE *file;
        snprintf(path, sizeof path, "%s/settings.ini", paths->config_dir);
        file = fopen(path, "r");
        if (file) {
            while (fgets(line, sizeof line, file))
                if (!strncmp(line, "pad_assign=", 11) && strstr(line, guid))
                    found = 1;
            fclose(file);
        }
        check(found, "the settings file on disk holds it");
    }

    /* restart: a new session on the same settings */
    adamsession_free(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0, "restart: the pad is picked up again");
    if (idx >= 0) {
        check(adamsession_gamepad_assignment(sess, idx) == want, "the assignment is restored on device add");
        check(adamsession_gamepad_effective_port(sess, idx) == want, "and the pad drives that port");
    }

    /* a stale GUID (another USB port): the name finds the entry. Rewrite the
     * stored GUID, restart. */
    snprintf(text, sizeof text, "ffffffffffffffffffffffffffffffff|%s=%d", PAD_NAME, want);
    adamsession_set_str(sess, "pad_assign", text);
    adamsession_free(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0 && adamsession_gamepad_assignment(sess, idx) == want,
          "a different GUID with the same name gets the assignment too");

    /* Automatic removes the entry, and the next run starts automatic */
    adamsession_gamepad_assign(sess, idx, want);   /* back under its own GUID */
    check(has_entry(sess, guid, want), "assigned under the pad's own GUID again");
    adamsession_gamepad_assign(sess, idx, -1);
    check(adamsession_gamepad_assignment(sess, idx) == -1, "Automatic: no explicit port");
    check(strstr(adamsession_get_str(sess, "pad_assign", ""), PAD_NAME) == NULL,
          "and the entry is gone from pad_assign");
    adamsession_free(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0 && adamsession_gamepad_assignment(sess, idx) == -1
          && adamsession_gamepad_effective_port(sess, idx) == auto_port, "after a restart it is automatic again");

    /* garbage in the setting is ignored, silently */
    adamsession_set_str(sess, "pad_assign", ";;|=;zz|x=9;%%%;" "00000000000000000000000000000000|" PAD_NAME "=7");
    adamsession_free(sess);
    sess = session_up(paths);
    if (!sess) { failures++; SDL_DetachVirtualJoystick(vid); return; }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0 && adamsession_gamepad_assignment(sess, idx) == -1,
          "a malformed or out-of-range pad_assign leaves the pad automatic");
    adamsession_gamepad_assign(sess, idx, want);
    check(has_entry(sess, guid, want), "and a new assignment still writes");
    adamsession_gamepad_assign(sess, idx, -1);

    /* a hot replug within a run restores it too, and is announced */
    adamsession_gamepad_assign(sess, idx, want);
    gen_before = adamsession_gamepad_generation(sess);
    SDL_DetachVirtualJoystick(vid);
    wait_gone(sess, PAD_NAME);
    check(pad_slot_now(sess, PAD_NAME) < 0, "unplugged: the pad is gone");
    check(adamsession_gamepad_generation(sess) != gen_before, "the generation moved");
    adamsession_gamepad_last_event(sess, text, sizeof text);
    check(!strcmp(text, "Disconnected: " PAD_NAME), "and the toast says the pad disconnected");
    vid = attach_pad(PAD_NAME, 0);
    idx = vid ? wait_for_pad(sess, PAD_NAME) : -1;
    check(idx >= 0 && adamsession_gamepad_assignment(sess, idx) == want, "unplugged and plugged back in: the port comes back");
    adamsession_gamepad_last_event(sess, text, sizeof text);
    check(strstr(text, want ? "(controller 2)" : "(controller 1)") != NULL, "and the toast names that controller");
    adamsession_gamepad_assign(sess, idx, -1);

    /* a restart of the session (Preferences, a cartridge load) finds the same
     * pad again: no new toast, the generation where it was, and a button held
     * across it is asserted again in the new machine */
    {
        SDL_Joystick *joy = SDL_OpenJoystick(vid);
        char before[160];
        int port = idx >= 0 ? adamsession_gamepad_effective_port(sess, idx) : -1;

        gen_before = adamsession_gamepad_generation(sess);
        adamsession_gamepad_last_event(sess, before, sizeof before);
        check(joy != NULL && port >= 0, "restart: the pad is open and drives a controller");
        if (joy && port >= 0) {
            pad_defaults(sess);      /* the defaults, whatever ran before */
            /* the pad thread has seen the port the pad drives now (a move
             * would release a button pressed just before it): the stick
             * right goes through its poll */
            SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 30000);
            expect(sess, port, ACT_BIT(ADAM_ACT_RIGHT), "restart: the pad thread is polling");
            SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
            expect(sess, port, 0, "restart: and the stick lets go");
            SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), true);
            expect(sess, port, ACT_BIT(ADAM_ACT_FIRE_L), "restart: South is held");
            gamepad_stop(sess);
            check(adamsession_gamepad_generation(sess) == gen_before, "stopped: the generation is where it was");
            adamsession_release_all(sess);     /* the new machine holds nothing */
            gamepad_start(sess);
            idx = wait_for_pad(sess, PAD_NAME);
            check(idx >= 0, "restarted: the pad is found again");
            expect(sess, port, ACT_BIT(ADAM_ACT_FIRE_L), "restarted: the held button is asserted again");
            adamsession_gamepad_last_event(sess, text, sizeof text);
            check(adamsession_gamepad_generation(sess) == gen_before && !strcmp(text, before),
                  "restarted: no new generation and no new toast");
            SDL_SetJoystickVirtualButton(joy, PAD_BTN(SOUTH), false);
            expect(sess, port, 0, "restarted: and the button still lets go");
        }
        if (joy)
            SDL_CloseJoystick(joy);
    }
    adamsession_free(sess);
    if (vid)
        SDL_DetachVirtualJoystick(vid);
}

/* A plain HID adapter: no gamepad mapping, so raw buttons, a hat and axes. */
static void raw_adapter(adamsession *sess)
{
    SDL_JoystickID vid = attach_pad(RAW_NAME, 1);
    SDL_Joystick *joy;
    int idx, port;
    char text[96];

    if (!vid) {
        printf("SKIP: no raw virtual joystick here: %s\n", SDL_GetError());
        return;
    }
    joy = SDL_OpenJoystick(vid);
    idx = joy ? wait_for_pad(sess, RAW_NAME) : -1;
    check(idx >= 0, "a raw adapter is opened as a joystick, not missed");
    port = idx >= 0 ? adamsession_gamepad_effective_port(sess, idx) : -1;
    if (port < 0) {
        check(0, "the raw adapter drives a controller");
        if (joy) SDL_CloseJoystick(joy);
        SDL_DetachVirtualJoystick(vid);
        return;
    }
    check(!SDL_IsGamepad(vid), "(and SDL has no gamepad mapping for it)");
    adamsession_gamepad_last_event(sess, text, sizeof text);
    check(strstr(text, "Connected: " RAW_NAME) == text, "the toast names it");

    /* raw buttons are the band 64.., unbound until mapped */
    SDL_SetJoystickVirtualButton(joy, 3, true);
    sleep_ms(150);
    expect(sess, port, 0, "an unmapped raw button does nothing");
    SDL_SetJoystickVirtualButton(joy, 3, false);
    adamsession_binding_set_button(sess, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + 9),
                                     ADAM_PAD_BTN_RAW_BASE + 3, NULL, 0);
    SDL_SetJoystickVirtualButton(joy, 3, true);
    expect(sess, port, KEYPAD_BIT(9), "raw button 3, bound to keypad 9, presses it");
    SDL_SetJoystickVirtualButton(joy, 3, false);
    expect(sess, port, 0, "released");

    /* hat 0 steers; its positions are also bindable buttons in the band 192.. */
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_UP);
    expect(sess, port, ACT_BIT(ADAM_ACT_UP), "hat 0 up steers up");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_RIGHTDOWN);
    expect(sess, port, ACT_BIT(ADAM_ACT_RIGHT) | ACT_BIT(ADAM_ACT_DOWN), "and a diagonal is two directions");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    expect(sess, port, 0, "centred: nothing");
    adamsession_binding_set_button(sess, ADAM_TARGET_PORT(port, ADAM_ACT_FIRE_R),
                                     ADAM_PAD_HAT_BASE + 2, NULL, 0);   /* hat 0, right */
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_RIGHT);
    expect(sess, port, ACT_BIT(ADAM_ACT_RIGHT) | ACT_BIT(ADAM_ACT_FIRE_R),
           "hat right steers right and, bound in the hat band, fires");
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    expect(sess, port, 0, "centred: both let go");

    /* axes 0 and 1 are its stick */
    SDL_SetJoystickVirtualAxis(joy, 0, -30000);
    SDL_SetJoystickVirtualAxis(joy, 1, -30000);
    expect(sess, port, ACT_BIT(ADAM_ACT_LEFT) | ACT_BIT(ADAM_ACT_UP), "axes 0 and 1 are the stick");
    SDL_SetJoystickVirtualAxis(joy, 0, 0);
    SDL_SetJoystickVirtualAxis(joy, 1, 0);
    expect(sess, port, 0, "centred");

    /* map mode takes a raw button and a hat position by number */
    {
        int btn = -1, got = 0, tries;
        adamsession_gamepad_capture_begin(sess);
        SDL_SetJoystickVirtualButton(joy, 5, true);
        for (tries = 0; tries < 250 && !got; tries++) {
            got = adamsession_gamepad_capture_poll(sess, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == ADAM_PAD_BTN_RAW_BASE + 5, "map mode captures raw button 5 as 69");
        SDL_SetJoystickVirtualButton(joy, 5, false);
        adamsession_gamepad_capture_begin(sess);
        SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_LEFT);
        for (got = 0, tries = 0; tries < 250 && !got; tries++) {
            got = adamsession_gamepad_capture_poll(sess, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == ADAM_PAD_HAT_BASE + 6, "and hat 0 left as 198");
        SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_CENTERED);
    }
    expect(sess, port, 0, "capturing pressed nothing");

    /* unplugged while held */
    SDL_SetJoystickVirtualButton(joy, 3, true);
    SDL_SetJoystickVirtualHat(joy, 0, SDL_HAT_UP);
    expect(sess, port, KEYPAD_BIT(9) | ACT_BIT(ADAM_ACT_UP), "held before the unplug");
    SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(vid);
    wait_gone(sess, RAW_NAME);
    expect(sess, port, 0, "the raw adapter's holds are released on unplug");
    pad_defaults(sess);
}

int main(void)
{
    char cfg[512], data[512];
    adamsession_paths paths;
    adamsession *sess;
    SDL_JoystickID vid;
    SDL_Joystick *vjoy;
    int idx, port, tries, act = -1;

    /* held open for the whole run: a session stopping must not take the
     * virtual pads with it (the persistence tests restart sessions) */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        printf("SKIP: SDL gamepad subsystem: %s\n", SDL_GetError());
        return 77;
    }
    test_tmpdir(cfg, sizeof cfg, "gmcfg");
    test_tmpdir(data, sizeof data, "gmdat");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";      /* the pad only, and no machine */

    /* the pad is there before the layer starts: the start-up scan must find
     * it (the hot-plug events are covered by the replug in persistence) */
    vid = attach_pad(PAD_NAME, 0);
    if (!vid) {
        printf("SKIP: no virtual joysticks here: %s\n", SDL_GetError());
        return 77;
    }
    vjoy = SDL_OpenJoystick(vid);
    if (!vjoy)
        return 1;
    sess = session_up(&paths);
    if (!sess) {
        SDL_DetachVirtualJoystick(vid);
        return 77;
    }
    idx = wait_for_pad(sess, PAD_NAME);
    check(idx >= 0, "the virtual pad was picked up");
    port = idx >= 0 ? adamsession_gamepad_effective_port(sess, idx) : -1;
    if (port < 0 || port > 1) {
        printf("%s: the virtual pad got no port (%d; real pads attached?)\n", idx < 0 ? "FAIL" : "SKIP", port);
        SDL_CloseJoystick(vjoy);
        SDL_DetachVirtualJoystick(vid);
        adamsession_free(sess);
        return idx < 0 ? 1 : 77;
    }
    printf("ok: the virtual pad drives controller %d\n", port + 1);
    check(adamsession_gamepad_generation(sess) >= 1, "a pad connecting bumped the generation");
    sleep_ms(300);   /* the pad thread takes its baseline of the buttons a poll after opening */

    /* the defaults */
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(SOUTH), true);
    expect(sess, port, ACT_BIT(ADAM_ACT_FIRE_L), "South is the left fire");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(EAST), true);
    expect(sess, port, ACT_BIT(ADAM_ACT_FIRE_L) | ACT_BIT(ADAM_ACT_FIRE_R), "East is the right fire");
    check(adamsession_controller_word(sess, port) == 0x3F3F, "and the machine is handed both (0x3F3F)");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(SOUTH), false);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(EAST), false);
    expect(sess, port, 0, "released");

    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(WEST), true);
    expect(sess, port, KEYPAD_BIT(1), "West is keypad 1");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(NORTH), true);
    expect(sess, port, KEYPAD_BIT(1), "North (keypad 2) is held too, but the console reads the lowest key");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(WEST), false);
    expect(sess, port, KEYPAD_BIT(2), "and keypad 2 is there once 1 lets go");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(NORTH), false);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(LEFT_SHOULDER), true);
    expect(sess, port, KEYPAD_BIT(10), "the left shoulder is *");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(LEFT_SHOULDER), false);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(RIGHT_SHOULDER), true);
    expect(sess, port, KEYPAD_BIT(11), "the right shoulder is #");
    check(adamsession_controller_word(sess, port) == 0x7F7B, "(word 0x7F7B)");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(RIGHT_SHOULDER), false);
    expect(sess, port, 0, "released");

    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_UP), true);
    expect(sess, port, ACT_BIT(ADAM_ACT_UP), "D-pad up is up");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_LEFT), true);
    expect(sess, port, ACT_BIT(ADAM_ACT_UP) | ACT_BIT(ADAM_ACT_LEFT), "up and left together are a diagonal");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_DOWN), true);
    expect(sess, port, ACT_BIT(ADAM_ACT_LEFT), "up and down together cancel");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_UP), false);
    expect(sess, port, ACT_BIT(ADAM_ACT_LEFT) | ACT_BIT(ADAM_ACT_DOWN), "and down is back once up lets go");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_DOWN), false);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_LEFT), false);
    expect(sess, port, 0, "released");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_RIGHT), true);
    expect(sess, port, ACT_BIT(ADAM_ACT_RIGHT), "D-pad right is right");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_RIGHT), false);
    expect(sess, port, 0, "released");

    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(GUIDE), true);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(START), true);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(LEFT_STICK), true);
    sleep_ms(200);
    expect(sess, port, 0, "Guide, Start and the stick clicks are unbound");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(GUIDE), false);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(START), false);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(LEFT_STICK), false);

    /* the left stick is an eight-way switch: half deflection engages */
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, -32768);
    expect(sess, port, ACT_BIT(ADAM_ACT_RIGHT) | ACT_BIT(ADAM_ACT_UP), "the stick right and up is a diagonal");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_LEFT), true);
    expect(sess, port, ACT_BIT(ADAM_ACT_UP), "the D-pad is its own source: left cancels the stick's right");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_LEFT), false);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, 0);
    expect(sess, port, 0, "the stick centred: nothing held");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 12000);   /* 0.37: under half */
    sleep_ms(150);
    expect(sess, port, 0, "a third of the way is not enough");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 17000);   /* 0.52 */
    expect(sess, port, ACT_BIT(ADAM_ACT_RIGHT), "past half it is");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 13000);   /* 0.40: between exit and enter */
    sleep_ms(150);
    expect(sess, port, ACT_BIT(ADAM_ACT_RIGHT), "and it holds until it falls below 0.35");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 9000);    /* 0.27 */
    expect(sess, port, 0, "then lets go");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    /* the right stick is nothing here */
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHTX, 30000);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHTY, -30000);
    sleep_ms(150);
    expect(sess, port, 0, "the right stick does nothing");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHTX, 0);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHTY, 0);

    /* the sources do not step on each other: a key and a pad on the same action */
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(SOUTH), true);
    expect(sess, port, ACT_BIT(ADAM_ACT_FIRE_L), "South fires");
    adamsession_pointer_set(sess, port, ACT_BIT(ADAM_ACT_FIRE_L));
    adamsession_input_key(sess, port ? 'w' : ADAMSESSION_KEYSYM_KP_8, 1, 0);
    expect(sess, port, ACT_BIT(ADAM_ACT_FIRE_L) | ACT_BIT(ADAM_ACT_UP), "a key adds to what the pad holds");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(SOUTH), false);
    sleep_ms(150);
    expect(sess, port, ACT_BIT(ADAM_ACT_FIRE_L) | ACT_BIT(ADAM_ACT_UP), "the pad letting go leaves the mouse's fire and the key's up");
    adamsession_release_all(sess);
    expect(sess, port, ACT_BIT(ADAM_ACT_FIRE_L), "release_all lets go of the keyboard's alone");
    adamsession_pointer_set(sess, port, 0);
    expect(sess, port, 0, "and the mouse's");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_UP), true);
    adamsession_input_key(sess, port ? 'w' : ADAMSESSION_KEYSYM_KP_8, 1, 0);
    adamsession_input_key(sess, port ? 'w' : ADAMSESSION_KEYSYM_KP_8, 0, 0);
    sleep_ms(100);
    expect(sess, port, ACT_BIT(ADAM_ACT_UP), "releasing a key does not drop a direction the pad still holds");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_UP), false);
    expect(sess, port, 0, "released");

    /* a button rebound to a keypad key; the old binding stops */
    adamsession_binding_set_button(sess, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + 5),
                                     ADAM_PAD_BTN_EAST, NULL, 0);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(EAST), true);
    expect(sess, port, KEYPAD_BIT(5), "East, rebound to keypad 5, presses it and no longer fires");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(EAST), false);
    expect(sess, port, 0, "released");
    pad_defaults(sess);

    /* a pad button on a console action posts it, once, on the press */
    adamsession_binding_set_button(sess, ADAM_TARGET_SYSACT(ADAM_SYSACT_RESET_GAME),
                                     ADAM_PAD_BTN_START, NULL, 0);
    while (adamsession_sysaction_take(sess, &act)) {}
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(START), true);
    check(wait_sysaction(sess, ADAM_SYSACT_RESET_GAME), "Start, bound to Reset Game, posts it");
    sleep_ms(150);
    check(adamsession_sysaction_take(sess, &act) == 0, "once: holding the button posts nothing more");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(START), false);
    sleep_ms(150);
    check(adamsession_sysaction_take(sess, &act) == 0 && adamsession_buttons_held(sess, port) == 0,
          "and the release posts nothing, nor holds anything");
    pad_defaults(sess);

    /* A trigger past half travel once risked the gamepad thread taking its
     * own lock twice; a hang here is a ctest timeout. */
    adamsession_binding_set_button(sess, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + 7),
                                     ADAM_PAD_BTN_LEFT_TRIGGER, NULL, 0);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    expect(sess, port, KEYPAD_BIT(7), "the left trigger presses its binding");
    check(adamsession_gamepad_generation(sess) >= 1 && adamsession_gamepad_effective_port(sess, idx) == port,
          "the pad lock is still free with a trigger held");
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    expect(sess, port, 0, "the left trigger released");
    adamsession_gamepad_capture_begin(sess);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    {
        int btn = -1, got = 0;
        for (tries = 0; tries < 250 && !got; tries++) {
            got = adamsession_gamepad_capture_poll(sess, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == ADAM_PAD_BTN_RIGHT_TRIGGER, "map mode captures a trigger");
    }
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    pad_defaults(sess);

    /* map mode swallows the press: it maps, it does not fire */
    adamsession_gamepad_capture_begin(sess);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(SOUTH), true);
    {
        int btn = -1, got = 0;
        for (tries = 0; tries < 250 && !got; tries++) {
            got = adamsession_gamepad_capture_poll(sess, &btn);
            if (!got) sleep_ms(20);
        }
        check(got && btn == ADAM_PAD_BTN_SOUTH, "map mode captures South");
    }
    expect(sess, port, 0, "and South did not fire while it was captured");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(SOUTH), false);
    /* (another button: SDL folds a release and a press that arrive between
     * two polls into no change at all) */
    adamsession_gamepad_capture_begin(sess);
    adamsession_gamepad_capture_cancel(sess);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(EAST), true);
    expect(sess, port, ACT_BIT(ADAM_ACT_FIRE_R), "after a cancel the pad acts again");
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(EAST), false);
    expect(sess, port, 0, "released");

    /* hot-unplug while held: the stick, a button and the sticks are
     * released, not left holding forever */
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(DPAD_RIGHT), true);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(SOUTH), true);
    SDL_SetJoystickVirtualButton(vjoy, PAD_BTN(WEST), true);
    SDL_SetJoystickVirtualAxis(vjoy, SDL_GAMEPAD_AXIS_LEFTY, -30000);
    expect(sess, port, ACT_BIT(ADAM_ACT_RIGHT) | ACT_BIT(ADAM_ACT_FIRE_L) | ACT_BIT(ADAM_ACT_UP) | KEYPAD_BIT(1),
           "held before the unplug");
    check(adamsession_controller_word(sess, port) == 0x3C71, "and the machine was handed it (0x3C71)");
    SDL_CloseJoystick(vjoy);
    SDL_DetachVirtualJoystick(vid);
    wait_gone(sess, PAD_NAME);
    check(pad_slot_now(sess, PAD_NAME) < 0, "the unplugged pad is gone");
    expect(sess, port, 0, "and its directions and buttons are released");
    check(adamsession_controller_word(sess, port) == 0x7F7F, "the machine is handed idle");

    raw_adapter(sess);

    adamsession_free(sess);

    persistence(&paths);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
