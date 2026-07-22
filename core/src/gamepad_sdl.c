/*
 * adamsession gamepad backend: SDL3 gamepad subsystem on a dedicated
 * polling thread. Hotplug is automatic (SDL's udev monitor runs inside
 * SDL_UpdateGamepads); the mapping mirrors the Android GameControllerMapper:
 * left stick with d-pad fallback, 0.3 deadzone / 0.35 direction threshold,
 * A/X -> left fire, B/Y -> right fire. Pads are assigned to controller
 * ports in connect order (first -> port 0) unless overridden.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_internal.h"

#define MAX_PADS 4
#define DEADZONE 0.3f
#define DIR_THRESHOLD 0.35f

typedef struct {
    SDL_Gamepad *pad;
    SDL_JoystickID id;
    int assigned_port; /* -1 = automatic (connect order) */
    uint16_t last_state;
} pad_slot;

typedef struct {
    pthread_t thread;
    volatile int run;
    pthread_mutex_t mtx;
    pad_slot pads[MAX_PADS];
    int count;
    uint16_t last_pushed[2]; /* per controller port, for introspection */
} gamepad_state;

static float axis_norm(Sint16 v)
{
    float f = (float)v / 32767.0f;
    if (f < -1.0f) f = -1.0f;
    return (f > -DEADZONE && f < DEADZONE) ? 0.0f : f;
}

static uint16_t read_pad(SDL_Gamepad *pad)
{
    float ax = axis_norm(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    float ay = axis_norm(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    int up = ay <= -DIR_THRESHOLD ||
             SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    int down = ay >= DIR_THRESHOLD ||
               SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    int left = ax <= -DIR_THRESHOLD ||
               SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    int right = ax >= DIR_THRESHOLD ||
                SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    int fire_l = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
                 SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST);
    int fire_r = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ||
                 SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
    return adam_controller_encode(up, down, left, right, fire_l, fire_r, -1);
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
            SDL_CloseGamepad(g->pads[i].pad);
            memmove(&g->pads[i], &g->pads[i + 1],
                    (size_t)(g->count - i - 1) * sizeof(pad_slot));
            g->count--;
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
                g->count++;
            }
        }
    }
    pthread_mutex_unlock(&g->mtx);
    SDL_free(ids);
    (void)s;
}

static void *gamepad_thread_main(void *arg)
{
    adamsession *s = arg;
    gamepad_state *g = s->gamepad;
    int tick = 0;

    pthread_setname_np(pthread_self(), "adam-gamepad");
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
            uint16_t state = read_pad(g->pads[i].pad);
            int port = g->pads[i].assigned_port >= 0
                           ? g->pads[i].assigned_port
                           : i;
            if (state != g->pads[i].last_state && port < 2) {
                g->pads[i].last_state = state;
                g->last_pushed[port] = state;
                adamsession_joystick_raw(s, port, state);
            }
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
