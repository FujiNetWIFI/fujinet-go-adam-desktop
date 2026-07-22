/*
 * End-to-end test of the gamepad backend using an SDL3 virtual gamepad:
 * verifies hotplug pickup, the GameControllerMapper-derived button/axis
 * mapping, and the encoded controller words the backend pushes toward the
 * core -- no hardware or human required.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "session_internal.h"
#include "test_tmp.h"

static int failures;

static int wait_for(adamsession *s, uint16_t want, const char *what)
{
    /* Device sync runs at ~4Hz, state polls at ~125Hz. */
    int i;
    for (i = 0; i < 100; i++) {
        if (adamsession_gamepad_last_state(s, 0) == want)
            return 1;
        usleep(20 * 1000);
    }
    fprintf(stderr, "FAIL %s: last_state %04X want %04X\n", what,
            adamsession_gamepad_last_state(s, 0), want);
    failures++;
    return 0;
}

int main(void)
{
    char tmpl[512];
    char *tmp = adam_make_tempdir(tmpl, sizeof(tmpl), "adampad-test-");
    adamsession_paths paths;
    adamsession *s;
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID vid;
    SDL_Joystick *vj;
    int i, baseline, vidx = -1;

    if (!tmp) return 1;
    memset(&paths, 0, sizeof(paths));
    paths.config_dir = tmp;
    paths.data_dir = tmp;
    paths.fujinet_lib = "";

    s = adamsession_new(&paths);
    if (!s) return 1;
    if (gamepad_start(s) != 0) {
        fprintf(stderr, "gamepad_map: SDL gamepad init failed (headless "
                        "CI?); skipping\n");
        return 77;
    }

    /* Real controllers may already be attached; the virtual pad is found
     * by name and pinned to port 0, everything else parked on port 1.
     * Let the backend's first device scan (about 4Hz) find whatever real
     * hardware exists before taking the baseline. */
    usleep(700 * 1000);
    baseline = adamsession_gamepad_count(s);

    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "adamtest virtual pad";
    vid = SDL_AttachVirtualJoystick(&desc);
    if (!vid) {
        fprintf(stderr, "gamepad_map: virtual joystick unavailable: %s; "
                        "skipping\n", SDL_GetError());
        gamepad_stop(s);
        return 77;
    }
    vj = SDL_OpenJoystick(vid);
    if (!vj) return 1;

    /* Wait for the backend's device sync to open it. */
    for (i = 0; i < 100 && adamsession_gamepad_count(s) <= baseline; i++)
        usleep(20 * 1000);
    for (i = 0; i < adamsession_gamepad_count(s); i++) {
        char name[64];
        adamsession_gamepad_name(s, i, name, sizeof(name));
        printf("gamepad_map: pad %d = \"%s\"\n", i, name);
        if (strcmp(name, "adamtest virtual pad") == 0)
            vidx = i;
        else
            adamsession_gamepad_assign(s, i, 1); /* keep port 0 clear */
    }
    if (vidx < 0) {
        fprintf(stderr, "gamepad_map: backend never saw the virtual pad\n");
        return 1;
    }
    adamsession_gamepad_assign(s, vidx, 0);

    /* A (south) -> left fire. */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, true);
    wait_for(s, 0x3F7F, "south=fireL");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_SOUTH, false);
    wait_for(s, 0x7F7F, "release");

    /* B (east) -> right fire. */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, true);
    wait_for(s, 0x7F3F, "east=fireR");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_EAST, false);
    wait_for(s, 0x7F7F, "release");

    /* D-pad. */
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, true);
    wait_for(s, 0x7E7F, "dpad up");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_UP, false);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    wait_for(s, 0x777F, "dpad left");
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_DPAD_LEFT, false);
    wait_for(s, 0x7F7F, "release");

    /* Left stick past the direction threshold: right+down + fire combo
     * (right 0x0200, down 0x0400, fireL 0x4000 all cleared from 0x7F7F). */
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 20000);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_WEST, true);
    wait_for(s, 0x397F, "stick right+down + west=fireL");
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(vj, SDL_GAMEPAD_AXIS_LEFTY, 0);
    SDL_SetJoystickVirtualButton(vj, SDL_GAMEPAD_BUTTON_WEST, false);
    wait_for(s, 0x7F7F, "release all");

    /* Hot-unplug: the count returns to what it was before the attach. */
    SDL_CloseJoystick(vj);
    SDL_DetachVirtualJoystick(vid);
    for (i = 0; i < 100 && adamsession_gamepad_count(s) != baseline; i++)
        usleep(20 * 1000);
    if (adamsession_gamepad_count(s) != baseline) {
        fprintf(stderr, "gamepad_map: backend kept the detached pad "
                        "(count %d, baseline %d)\n",
                adamsession_gamepad_count(s), baseline);
        failures++;
    }

    gamepad_stop(s);
    adamsession_free(s);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("gamepad_map: hotplug + mapping + encoding ok\n");
    return 0;
}
