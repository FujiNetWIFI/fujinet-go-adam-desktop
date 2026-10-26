/*
 * adamsession gamepad backend: SDL3 gamepad subsystem on a dedicated
 * polling thread. Hotplug is automatic (SDL's udev monitor runs inside
 * SDL_UpdateGamepads). The left stick (0.3 deadzone / 0.35 direction
 * threshold) and D-pad always steer; every other button drives whatever
 * the bindings table says (default: A = left fire, B = right fire). Pads
 * are assigned to controller ports in connect order (first -> port 0)
 * unless overridden. Each pad is its own input source (input.c), merged
 * with the keyboard and the keypad window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bindings.h"
#include "compat.h"
#include "session_internal.h"

#define MAX_PADS ADAM_MAX_PADS
#define DEADZONE 0.3f
#define DIR_THRESHOLD 0.35f

typedef struct {
    SDL_Gamepad *pad;
    SDL_JoystickID id;
    int assigned_port; /* -1 = automatic (connect order) */
    uint16_t last_state; /* the pad's own controller word, change detection */
    uint32_t last_buttons; /* adam_pad_button bitmask, capture edge detection */
} pad_slot;

typedef struct {
    pthread_t thread;
    volatile int run;
    pthread_mutex_t mtx;
    pad_slot pads[MAX_PADS];
    int count;
    uint16_t last_pushed[2]; /* per controller port, for introspection */
    /* Map-mode capture */
    volatile int capture_armed;
    volatile int captured; /* adam_pad_button, NONE until a press */
} gamepad_state;

static float axis_norm(Sint16 v)
{
    float f = (float)v / 32767.0f;
    if (f < -1.0f) f = -1.0f;
    return (f > -DEADZONE && f < DEADZONE) ? 0.0f : f;
}

static const SDL_GamepadButton sdl_button[ADAM_PAD_BTN_COUNT] = {
    [ADAM_PAD_BTN_SOUTH] = SDL_GAMEPAD_BUTTON_SOUTH,
    [ADAM_PAD_BTN_EAST] = SDL_GAMEPAD_BUTTON_EAST,
    [ADAM_PAD_BTN_WEST] = SDL_GAMEPAD_BUTTON_WEST,
    [ADAM_PAD_BTN_NORTH] = SDL_GAMEPAD_BUTTON_NORTH,
    [ADAM_PAD_BTN_BACK] = SDL_GAMEPAD_BUTTON_BACK,
    [ADAM_PAD_BTN_GUIDE] = SDL_GAMEPAD_BUTTON_GUIDE,
    [ADAM_PAD_BTN_START] = SDL_GAMEPAD_BUTTON_START,
    [ADAM_PAD_BTN_LEFT_STICK] = SDL_GAMEPAD_BUTTON_LEFT_STICK,
    [ADAM_PAD_BTN_RIGHT_STICK] = SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    [ADAM_PAD_BTN_LEFT_SHOULDER] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    [ADAM_PAD_BTN_RIGHT_SHOULDER] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    [ADAM_PAD_BTN_DPAD_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP,
    [ADAM_PAD_BTN_DPAD_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    [ADAM_PAD_BTN_DPAD_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    [ADAM_PAD_BTN_DPAD_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};

static int button_down(SDL_Gamepad *pad, int button)
{
    if (button == ADAM_PAD_BTN_LEFT_TRIGGER)
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000;
    if (button == ADAM_PAD_BTN_RIGHT_TRIGGER)
        return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    if (button < 0 || button >= ADAM_PAD_BTN_COUNT) return 0;
    return SDL_GetGamepadButton(pad, sdl_button[button]) ? 1 : 0;
}

/* Reads a pad into its steering, its fire buttons and the keypad keys the
 * bindings put on it, for the port it drives. */
static void read_pad(SDL_Gamepad *pad, int port, int *dirs, int *fires,
                     uint16_t *keys, uint32_t *buttons)
{
    float ax = axis_norm(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    float ay = axis_norm(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    int act, b;

    *dirs = 0; *fires = 0; *keys = 0; *buttons = 0;
    if (ay <= -DIR_THRESHOLD || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))
        *dirs |= ADAM_DIR_UP;
    if (ay >= DIR_THRESHOLD || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
        *dirs |= ADAM_DIR_DOWN;
    if (ax <= -DIR_THRESHOLD || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
        *dirs |= ADAM_DIR_LEFT;
    if (ax >= DIR_THRESHOLD || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
        *dirs |= ADAM_DIR_RIGHT;

    for (b = 0; b < ADAM_PAD_BTN_COUNT; b++)
        if (button_down(pad, b)) *buttons |= 1u << b;

    for (act = 0; act < ADAM_ACT_PER_PORT; act++) {
        int bound = bindings_button_of(ADAM_TARGET_PORT(port, act));
        if (bound == ADAM_PAD_BTN_NONE || !(*buttons & (1u << bound))) continue;
        if (act < ADAM_KEYPAD_KEYS) *keys |= (uint16_t)(1u << act);
        else if (act == ADAM_ACT_UP) *dirs |= ADAM_DIR_UP;
        else if (act == ADAM_ACT_DOWN) *dirs |= ADAM_DIR_DOWN;
        else if (act == ADAM_ACT_LEFT) *dirs |= ADAM_DIR_LEFT;
        else if (act == ADAM_ACT_RIGHT) *dirs |= ADAM_DIR_RIGHT;
        else if (act == ADAM_ACT_FIRE_L) *fires |= 1;
        else if (act == ADAM_ACT_FIRE_R) *fires |= 2;
    }
}

static void sync_devices(adamsession *s, gamepad_state *g)
{
    int i, n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);

    pthread_mutex_lock(&g->mtx);
    /* Drop pads that vanished. */
    for (i = 0; i < g->count;) {
        int present = 0, j;
        for (j = 0; j < n; j++)
            if (ids && ids[j] == g->pads[i].id) present = 1;
        if (!present) {
            int j2;
            SDL_CloseGamepad(g->pads[i].pad);
            memmove(&g->pads[i], &g->pads[i + 1],
                    (size_t)(g->count - i - 1) * sizeof(pad_slot));
            g->count--;
            /* the slots above shifted down: drop their holds, the next
             * poll re-asserts what is really held */
            for (j2 = i; j2 < MAX_PADS; j2++)
                input_pad_clear(s, j2);
        } else {
            i++;
        }
    }
    /* Open pads that appeared. */
    for (i = 0; ids && i < n && g->count < MAX_PADS; i++) {
        int j, known = 0;
        for (j = 0; j < g->count; j++)
            if (g->pads[j].id == ids[i]) known = 1;
        if (!known) {
            SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
            if (pad) {
                g->pads[g->count].pad = pad;
                g->pads[g->count].id = ids[i];
                g->pads[g->count].assigned_port = -1;
                g->pads[g->count].last_state = 0x7F7F;
                g->pads[g->count].last_buttons = 0;
                g->count++;
            }
        }
    }
    pthread_mutex_unlock(&g->mtx);
    SDL_free(ids);
}

static void *gamepad_thread_main(void *arg)
{
    adamsession *s = arg;
    gamepad_state *g = s->gamepad;
    int tick = 0;

    adam_thread_setname("adam-gamepad");
    while (g->run) {
        int i;
        SDL_UpdateGamepads();
        /* Device add/remove is rare; re-enumerate at ~4Hz, poll state at
         * ~125Hz. Flush the (unused) event queue so it can't grow. */
        if (tick++ % 32 == 0)
            sync_devices(s, g);
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

        pthread_mutex_lock(&g->mtx);
        for (i = 0; i < g->count; i++) {
            int port = g->pads[i].assigned_port >= 0
                           ? g->pads[i].assigned_port
                           : i;
            int dirs, fires;
            uint16_t keys, state;
            uint32_t buttons, fresh;

            if (port > 1) continue;
            read_pad(g->pads[i].pad, port, &dirs, &fires, &keys, &buttons);

            /* Map mode: the next press is the answer, and is swallowed so the
             * press that maps a button does not also fire its old target. */
            fresh = buttons & ~g->pads[i].last_buttons;
            g->pads[i].last_buttons = buttons;
            if (g->capture_armed) {
                if (fresh) {
                    int b;
                    for (b = 0; b < ADAM_PAD_BTN_COUNT; b++)
                        if (fresh & (1u << b)) {
                            g->captured = b;
                            g->capture_armed = 0;
                            break;
                        }
                }
                dirs = fires = 0; keys = 0;
            }

            {
                int kp = -1, k;
                for (k = 0; k < ADAM_KEYPAD_KEYS; k++)
                    if (keys & (1u << k)) { kp = k; break; }
                state = adam_controller_encode(
                    dirs & ADAM_DIR_UP, dirs & ADAM_DIR_DOWN,
                    dirs & ADAM_DIR_LEFT, dirs & ADAM_DIR_RIGHT,
                    fires & 1, fires & 2, kp);
            }
            if (state != g->pads[i].last_state) {
                g->pads[i].last_state = state;
                g->last_pushed[port] = state;
            }
            input_pad_set(s, i, port, dirs, fires, keys);
        }
        pthread_mutex_unlock(&g->mtx);
        SDL_Delay(8);
    }
    return NULL;
}

uint16_t adamsession_gamepad_last_state(adamsession *s, int port)
{
    gamepad_state *g = s->gamepad;
    uint16_t v = 0x7F7F;
    if (!g || port < 0 || port > 1) return v;
    pthread_mutex_lock(&g->mtx);
    if (g->last_pushed[port])
        v = g->last_pushed[port];
    pthread_mutex_unlock(&g->mtx);
    return v;
}

int gamepad_start(adamsession *s)
{
    gamepad_state *g;

    if (s->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof(*g));
    if (!g) return -1;
    pthread_mutex_init(&g->mtx, NULL);
    g->captured = ADAM_PAD_BTN_NONE;
    g->run = 1;
    s->gamepad = g;
    if (pthread_create(&g->thread, NULL, gamepad_thread_main, s) != 0) {
        s->gamepad = NULL;
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    return 0;
}

void gamepad_stop(adamsession *s)
{
    gamepad_state *g = s->gamepad;
    int i;
    if (!g) return;
    g->run = 0;
    pthread_join(g->thread, NULL);
    for (i = 0; i < g->count; i++)
        SDL_CloseGamepad(g->pads[i].pad);
    pthread_mutex_destroy(&g->mtx);
    s->gamepad = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

int adamsession_gamepad_count(adamsession *s)
{
    gamepad_state *g = s->gamepad;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->mtx);
    n = g->count;
    pthread_mutex_unlock(&g->mtx);
    return n;
}

int adamsession_gamepad_name(adamsession *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s->gamepad;
    const char *name = NULL;
    if (!g || dstsz <= 0) return 0;
    pthread_mutex_lock(&g->mtx);
    if (idx >= 0 && idx < g->count)
        name = SDL_GetGamepadName(g->pads[idx].pad);
    snprintf(dst, (size_t)dstsz, "%s", name ? name : "");
    pthread_mutex_unlock(&g->mtx);
    return (int)strlen(dst);
}

void adamsession_gamepad_assign(adamsession *s, int idx, int port)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    pthread_mutex_lock(&g->mtx);
    if (idx >= 0 && idx < g->count)
        g->pads[idx].assigned_port = (port >= 0 && port < 2) ? port : -1;
    pthread_mutex_unlock(&g->mtx);
}

void adamsession_gamepad_capture_begin(adamsession *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    g->captured = ADAM_PAD_BTN_NONE;
    g->capture_armed = 1;
}

void adamsession_gamepad_capture_cancel(adamsession *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    g->capture_armed = 0;
    g->captured = ADAM_PAD_BTN_NONE;
}

int adamsession_gamepad_capture_poll(adamsession *s, int *button)
{
    gamepad_state *g = s->gamepad;
    if (!g || g->captured == ADAM_PAD_BTN_NONE) return 0;
    if (button) *button = g->captured;
    g->captured = ADAM_PAD_BTN_NONE;
    return 1;
}
