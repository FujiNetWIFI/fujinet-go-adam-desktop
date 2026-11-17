/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as ColecoVision hand
 * controllers, on their own polling thread.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK/Qt/AppKit/Win32, not SDL, and only the gamepad
 * subsystem is initialised here -- never SDL video. Devices are added and
 * removed as they appear on the system: SDL posts the JOYSTICK_ and
 * GAMEPAD_ add/remove pairs and both are taken, with idempotent open/close,
 * so neither kind of device can be missed. A device SDL has no gamepad
 * mapping for (a plain HID adapter) is opened as a raw joystick instead:
 * its buttons and hats are bindable in the raw bands, just unnamed.
 *
 * What a pad drives is one hand controller (the first pad is controller 1,
 * the second controller 2). Every button goes through the bindings table: by
 * default the D-pad steers, South is the left fire, East the right fire, West
 * and North are keypad 1 and 2 and the shoulders are * and #; any button may
 * be bound to any keypad key, direction, fire button or console action
 * instead. The left analog stick is always the controller's stick: an
 * eight-way switch with hysteresis (in at half deflection, out at 0.35).
 * Hat 0 of a raw joystick is the stick too. The ways of holding an input -- a
 * pad button, the analog stick, the keyboard -- are separate sources in the
 * session, so letting go of one never releases another. Ported from the
 * Channel F sibling's gamepad_sdl.c.
 *
 * Pads are assigned to ports in connection order unless assigned explicitly.
 * An explicit assignment is remembered (settings key "pad_assign", see
 * pad_assign.h): a pad that is unplugged and plugged back in, or is there on
 * the next run, gets it back -- matched by its SDL GUID, else by its name.
 * Two identical pads share one entry; the first to connect gets it. Every
 * connect/disconnect is reported to the session for the frontend's toast.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gamepad_sdl.h"
#include "pad_assign.h"
#include "session_internal.h"

#define MAX_PADS ADAM_MAX_PADS
#define MAX_HATS 4

#define TRIGGER_ON  0.5f

typedef struct {
    SDL_JoystickID devid;
    int owner;                /* the session's source number for this pad: stable
                               * while it is connected, whatever its slot */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit port, -1 = automatic */
    int by_name;              /* assign was restored from a name-only match
                               * (the entry's GUID differs): an exact-GUID
                               * pad may take it over */
    int port;                 /* the port it drove on the last poll, or -1 */
    int stick_dir;            /* left stick's four-way state (hysteresis) */
    uint8_t sent[ADAM_ACT_PER_PORT]; /* actions the analog stick / hat hold */
    /* What each button's press asserted (target + 1, 0 = nothing), so its
     * release lets go of exactly that even if the binding changed meanwhile,
     * and a pad that goes away drops what it held. */
    int16_t btn_target[256];
    SDL_GUID guid;
    char guidstr[ADAM_PAD_GUID_MAX];   /* guid as hex, for the pad_assign table */
    char name[ADAM_PAD_NAME_MAX];
    int trigger_down[2];
    Uint8 last_hat[MAX_HATS];
} pad_slot;

typedef struct {
    struct adamsession *session;
    pthread_t thread;
    pthread_mutex_t lock;
    int running;
    atomic_int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    int quiet_scan;           /* thread only: the first scan of a restarted
                               * session re-finds pads without announcing them */
    /* Map-mode capture */
    int cap_active, cap_have, cap_result;
    /* the remembered explicit assignments (the "pad_assign" setting), for
     * pads that are away as well as those here; guarded by lock */
    adam_pad_table table;
} gamepad_state;

/* ---- pure helpers ----------------------------------------------------------- */

unsigned adam_acts_from_dir(int dir)
{
    unsigned acts = 0;

    if ((dir & ADAM_DIR_UP) && !(dir & ADAM_DIR_DOWN)) acts |= 1u << ADAM_ACT_UP;
    if ((dir & ADAM_DIR_DOWN) && !(dir & ADAM_DIR_UP)) acts |= 1u << ADAM_ACT_DOWN;
    if ((dir & ADAM_DIR_LEFT) && !(dir & ADAM_DIR_RIGHT)) acts |= 1u << ADAM_ACT_LEFT;
    if ((dir & ADAM_DIR_RIGHT) && !(dir & ADAM_DIR_LEFT)) acts |= 1u << ADAM_ACT_RIGHT;
    return acts;
}

int adam_dir_from_stick(float xval, float yval, int prev, float enter, float leave)
{
    int dir = 0;
    /* each axis independently, so diagonals are two bits and a stick that
     * is exactly horizontal never picks up a vertical bit from noise */
    if (xval <= -enter || (xval <= -leave && (prev & ADAM_DIR_LEFT))) dir |= ADAM_DIR_LEFT;
    if (xval >=  enter || (xval >=  leave && (prev & ADAM_DIR_RIGHT))) dir |= ADAM_DIR_RIGHT;
    if (yval <= -enter || (yval <= -leave && (prev & ADAM_DIR_UP))) dir |= ADAM_DIR_UP;
    if (yval >=  enter || (yval >=  leave && (prev & ADAM_DIR_DOWN))) dir |= ADAM_DIR_DOWN;
    return dir;
}

int adam_pad_for_port(const int *assign, int npads, int port)
{
    int idx, next = 0;
    for (idx = 0; idx < npads; idx++)
        if (assign[idx] == port) return idx;
    for (idx = 0; idx < npads; idx++) {
        if (assign[idx] >= 0) continue;   /* explicitly elsewhere */
        if (next == port) return idx;
        next++;
    }
    return -1;
}

int adam_pad_button_from_sdl(uint8_t button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ADAM_PAD_BTN_SOUTH;
    case SDL_GAMEPAD_BUTTON_EAST: return ADAM_PAD_BTN_EAST;
    case SDL_GAMEPAD_BUTTON_WEST: return ADAM_PAD_BTN_WEST;
    case SDL_GAMEPAD_BUTTON_NORTH: return ADAM_PAD_BTN_NORTH;
    case SDL_GAMEPAD_BUTTON_BACK: return ADAM_PAD_BTN_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE: return ADAM_PAD_BTN_GUIDE;
    case SDL_GAMEPAD_BUTTON_START: return ADAM_PAD_BTN_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ADAM_PAD_BTN_LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ADAM_PAD_BTN_RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ADAM_PAD_BTN_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ADAM_PAD_BTN_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ADAM_PAD_BTN_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ADAM_PAD_BTN_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ADAM_PAD_BTN_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ADAM_PAD_BTN_DPAD_RIGHT;
    default: return ADAM_PAD_BTN_NONE;
    }
}

int adam_sdl_button_from_pad(int button)
{
    switch (button) {
    case ADAM_PAD_BTN_SOUTH: return SDL_GAMEPAD_BUTTON_SOUTH;
    case ADAM_PAD_BTN_EAST: return SDL_GAMEPAD_BUTTON_EAST;
    case ADAM_PAD_BTN_WEST: return SDL_GAMEPAD_BUTTON_WEST;
    case ADAM_PAD_BTN_NORTH: return SDL_GAMEPAD_BUTTON_NORTH;
    case ADAM_PAD_BTN_BACK: return SDL_GAMEPAD_BUTTON_BACK;
    case ADAM_PAD_BTN_GUIDE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case ADAM_PAD_BTN_START: return SDL_GAMEPAD_BUTTON_START;
    case ADAM_PAD_BTN_LEFT_STICK: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case ADAM_PAD_BTN_RIGHT_STICK: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case ADAM_PAD_BTN_LEFT_SHOULDER: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case ADAM_PAD_BTN_RIGHT_SHOULDER: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case ADAM_PAD_BTN_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case ADAM_PAD_BTN_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case ADAM_PAD_BTN_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case ADAM_PAD_BTN_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return -1;
    }
}

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *gst, SDL_JoystickID devid)
{
    int idx;
    for (idx = 0; idx < gst->npads; idx++)
        if (gst->pads[idx].devid == devid) return idx;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *name = slot->handle ? SDL_GetGamepadName(slot->handle)
                                    : SDL_GetJoystickName(slot->joy);
    return name ? name : "Gamepad";
}

static void slot_release_all(gamepad_state *gst, pad_slot *slot)
{
    int act, btn;
    if (slot->port >= 0)
        for (act = 0; act < ADAM_ACT_PER_PORT; act++)
            if (slot->sent[act]) {
                slot->sent[act] = 0;
                session_gamepad_stick(gst->session, slot->owner, slot->port, act, 0);
            }
    for (btn = 0; btn < (int)(sizeof slot->btn_target / sizeof slot->btn_target[0]); btn++)
        if (slot->btn_target[btn]) {
            session_gamepad_press(gst->session, slot->owner, slot->btn_target[btn] - 1, 0);
            slot->btn_target[btn] = 0;
        }
}

static void slot_close(gamepad_state *gst, pad_slot *slot)
{
    slot_release_all(gst, slot);
    if (slot->handle) SDL_CloseGamepad(slot->handle);
    if (slot->joy) SDL_CloseJoystick(slot->joy);
    slot->handle = NULL;
    slot->joy = NULL;
}

static int effective_port_locked(gamepad_state *gst, int idx);

/* Gives the pad in slot idx its remembered port. An entry with the pad's own
 * GUID is the pad's: it takes it from a pad that got it only through the name
 * fallback. A pad known only by name takes the entry if no other pad carries
 * it (two identical pads, or one pad known under two GUIDs): then this one
 * stays automatic. Locked by the caller; only reads settings' copy in
 * gst->table, never writes. */
static void restore_assignment(gamepad_state *gst, int idx)
{
    pad_slot *slot = &gst->pads[idx];
    int ent = adam_pad_table_find(&gst->table, slot->guidstr, slot->name), kix;
    int exact;

    if (ent < 0 || gst->table.entry[ent].port >= ADAM_PORTS) return;
    exact = !strcmp(gst->table.entry[ent].guid, slot->guidstr);
    for (kix = 0; kix < gst->npads; kix++) {
        pad_slot *other = &gst->pads[kix];
        if (kix == idx || other->assign < 0 ||
            adam_pad_table_find(&gst->table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name) return;
        other->assign = -1;          /* the entry's owner is here: it wins */
        other->by_name = 0;
    }
    slot->assign = gst->table.entry[ent].port;
    slot->by_name = !exact;
}

/* Idempotent, and the single place the gamepad/joystick decision is made:
 * both SDL_EVENT_JOYSTICK_ADDED and SDL_EVENT_GAMEPAD_ADDED land here (SDL
 * posts both for a mapped device) and the slot_for_id guard makes the second
 * one a no-op. */
static void open_pad(gamepad_state *gst, SDL_JoystickID devid)
{
    pthread_mutex_lock(&gst->lock);
    if (gst->npads < MAX_PADS && slot_for_id(gst, devid) < 0) {
        /* Not an else-if on SDL_IsGamepad: a device SDL calls a gamepad can
         * still fail to open as one. Falling through to the raw joystick
         * keeps it usable -- every button is still bindable, just unnamed. */
        SDL_Gamepad *pad = SDL_IsGamepad(devid) ? SDL_OpenGamepad(devid) : NULL;
        SDL_Joystick *joy = pad ? NULL : SDL_OpenJoystick(devid);
        if (pad || joy) {
            pad_slot *slot = &gst->pads[gst->npads++];
            int hat, used = 0, kix;
            for (kix = 0; kix < gst->npads - 1; kix++)
                used |= 1 << gst->pads[kix].owner;
            memset(slot, 0, sizeof *slot);
            for (kix = 0; kix < MAX_PADS && (used & (1 << kix)); kix++) {}
            slot->owner = kix;
            slot->devid = devid;
            slot->handle = pad;
            slot->joy = joy;
            slot->assign = -1;
            slot->port = -1;
            for (hat = 0; hat < MAX_HATS; hat++) slot->last_hat[hat] = SDL_HAT_CENTERED;
            slot->guid = SDL_GetJoystickGUIDForID(devid);
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(slot->guid, slot->guidstr, sizeof slot->guidstr);
            restore_assignment(gst, gst->npads - 1);
            fprintf(stderr, "adam gamepad: %s connected (%s)\n", slot->name,
                    pad ? "gamepad" : "raw joystick");
            if (!gst->quiet_scan) {
                char msg[128];
                int port = effective_port_locked(gst, gst->npads - 1);
                if (port >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (controller %d)", slot->name, port + 1);
                else
                    snprintf(msg, sizeof msg, "Connected: %s", slot->name);
                gst->generation++;
                session_gamepad_event(gst->session, msg);
            }
        }
    }
    pthread_mutex_unlock(&gst->lock);
}

static void close_pad(gamepad_state *gst, SDL_JoystickID devid)
{
    int idx, kix;
    pthread_mutex_lock(&gst->lock);
    idx = slot_for_id(gst, devid);
    if (idx >= 0) {
        pad_slot *slot = &gst->pads[idx];
        char msg[128];
        fprintf(stderr, "adam gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        slot_close(gst, slot);
        session_gamepad_event(gst->session, msg);
        for (kix = idx; kix < gst->npads - 1; kix++) gst->pads[kix] = gst->pads[kix + 1];
        gst->npads--;
        gst->generation++;
    }
    pthread_mutex_unlock(&gst->lock);
}

/* ---- buttons ---------------------------------------------------------------- */

/* The port a slot drives right now. Locked by the caller. */
static int effective_port_locked(gamepad_state *gst, int idx)
{
    int assign[MAX_PADS], kix, port;
    for (kix = 0; kix < MAX_PADS; kix++) assign[kix] = -1;
    for (kix = 0; kix < gst->npads; kix++) assign[kix] = gst->pads[kix].assign;
    for (port = 0; port < ADAM_PORTS; port++)
        if (adam_pad_for_port(assign, gst->npads, port) == idx) return port;
    return -1;
}

static void dispatch_button(gamepad_state *gst, SDL_JoystickID which, int btn, int down)
{
    int idx, port, owner = 0, target = -1;
    if (btn == ADAM_PAD_BTN_NONE) return;

    pthread_mutex_lock(&gst->lock);
    if (gst->cap_active) {
        /* Map mode: every button event is swallowed while armed, not just
         * the captured one, so the press that maps a button does not also
         * fire whatever it used to do. */
        if (down && !gst->cap_have) { gst->cap_have = 1; gst->cap_result = btn; }
        pthread_mutex_unlock(&gst->lock);
        return;
    }
    idx = slot_for_id(gst, which);
    port = idx >= 0 ? effective_port_locked(gst, idx) : -1;
    if (idx >= 0 && port >= 0 && btn >= 0 && btn < 256) {
        pad_slot *slot = &gst->pads[idx];
        owner = slot->owner;
        if (down) {
            target = adamsession_target_for_button(gst->session, port, btn);
            if (target >= 0 && target < ADAM_TARGET_SYSACT(0))
                slot->btn_target[btn] = (int16_t)(target + 1);
        } else if (slot->btn_target[btn]) {
            /* what the press asserted, whatever the binding says now */
            target = slot->btn_target[btn] - 1;
            slot->btn_target[btn] = 0;
        }
    }
    pthread_mutex_unlock(&gst->lock);
    if (target < 0) return;

    /* a console action is posted, on the press only, for the UI thread */
    if (target >= ADAM_TARGET_SYSACT(0)) {
        if (down) adamsession_sysaction_post(gst->session, target - ADAM_TARGET_SYSACT(0));
        return;
    }
    session_gamepad_press(gst->session, owner, target, down);
}

static int hat_dir_from_mask(Uint8 mask)
{
    switch (mask) {
    case SDL_HAT_UP: return 0;
    case SDL_HAT_RIGHTUP: return 1;
    case SDL_HAT_RIGHT: return 2;
    case SDL_HAT_RIGHTDOWN: return 3;
    case SDL_HAT_DOWN: return 4;
    case SDL_HAT_LEFTDOWN: return 5;
    case SDL_HAT_LEFT: return 6;
    case SDL_HAT_LEFTUP: return 7;
    default: return -1;
    }
}

static void handle_hat_motion(gamepad_state *gst, SDL_JoystickID which, int hat, Uint8 mask)
{
    int idx, prev_dir, dir;
    if (hat < 0 || hat >= MAX_HATS) return;
    pthread_mutex_lock(&gst->lock);
    idx = slot_for_id(gst, which);
    if (idx < 0 || gst->pads[idx].handle) { pthread_mutex_unlock(&gst->lock); return; }
    prev_dir = hat_dir_from_mask(gst->pads[idx].last_hat[hat]);
    gst->pads[idx].last_hat[hat] = mask;
    pthread_mutex_unlock(&gst->lock);
    dir = hat_dir_from_mask(mask);
    /* hat positions are bindable buttons in their own band; hat 0's four
     * cardinal points also feed the directional role in poll_one */
    if (prev_dir >= 0)
        dispatch_button(gst, which, ADAM_PAD_HAT_BASE + hat * ADAM_PAD_HAT_DIRS + prev_dir, 0);
    if (dir >= 0)
        dispatch_button(gst, which, ADAM_PAD_HAT_BASE + hat * ADAM_PAD_HAT_DIRS + dir, 1);
}

/* After a session restart the new machine holds nothing, but a pad button
 * may still be physically held: assert it again, as its press would have. */
static void reassert_held(gamepad_state *gst)
{
    int slot_idx;
    for (slot_idx = 0; slot_idx < MAX_PADS; slot_idx++) {
        int held[ADAM_PAD_BTN_RAW_BASE + 64 + MAX_HATS * ADAM_PAD_HAT_DIRS];
        int nheld = 0, btn, hat, count;
        SDL_JoystickID devid;
        pad_slot *slot;

        pthread_mutex_lock(&gst->lock);
        if (slot_idx >= gst->npads) { pthread_mutex_unlock(&gst->lock); break; }
        slot = &gst->pads[slot_idx];
        devid = slot->devid;
        /* the port it drives from now on, so the first poll sees no move
         * (a move releases whatever the pad holds) */
        slot->port = effective_port_locked(gst, slot_idx);
        if (slot->handle) {
            for (btn = 0; btn < SDL_GAMEPAD_BUTTON_COUNT; btn++)
                if (SDL_GetGamepadButton(slot->handle, (SDL_GamepadButton)btn))
                    held[nheld++] = adam_pad_button_from_sdl((SDL_GamepadButton)btn);
        } else if (slot->joy) {
            count = SDL_GetNumJoystickButtons(slot->joy);
            for (btn = 0; btn < count && btn < 64; btn++)
                if (SDL_GetJoystickButton(slot->joy, btn))
                    held[nheld++] = ADAM_PAD_BTN_RAW_BASE + btn;
            count = SDL_GetNumJoystickHats(slot->joy);
            for (hat = 0; hat < count && hat < MAX_HATS; hat++) {
                Uint8 mask = SDL_GetJoystickHat(slot->joy, hat);
                int dir = hat_dir_from_mask(mask);
                slot->last_hat[hat] = mask;
                if (dir >= 0)
                    held[nheld++] = ADAM_PAD_HAT_BASE + hat * ADAM_PAD_HAT_DIRS + dir;
            }
        }
        pthread_mutex_unlock(&gst->lock);
        for (btn = 0; btn < nheld; btn++)
            dispatch_button(gst, devid, held[btn], 1);
    }
}

/* ---- the stick --------------------------------------------------------------- */

static float axis_norm(Sint16 val)
{
    return val < 0 ? (float)val / 32768.0f : (float)val / 32767.0f;
}

/* The left stick: a mapped gamepad's, or axes 0 and 1 of a raw joystick. */
static void slot_stick(const pad_slot *slot, float *xval, float *yval)
{
    if (slot->handle) {
        *xval = axis_norm(SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTX));
        *yval = axis_norm(SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTY));
    } else {
        *xval = SDL_GetNumJoystickAxes(slot->joy) > 0 ? axis_norm(SDL_GetJoystickAxis(slot->joy, 0)) : 0.f;
        *yval = SDL_GetNumJoystickAxes(slot->joy) > 1 ? axis_norm(SDL_GetJoystickAxis(slot->joy, 1)) : 0.f;
    }
}

/* The raw joystick's hat 0 as directions (a mapped gamepad's D-pad is a
 * button, bound like any other). */
static int slot_hat_dir(const pad_slot *slot)
{
    int dir = 0;
    if (!slot->handle && slot->joy && SDL_GetNumJoystickHats(slot->joy) > 0) {
        Uint8 mask = SDL_GetJoystickHat(slot->joy, 0);
        if (mask & SDL_HAT_UP) dir |= ADAM_DIR_UP;
        if (mask & SDL_HAT_DOWN) dir |= ADAM_DIR_DOWN;
        if (mask & SDL_HAT_LEFT) dir |= ADAM_DIR_LEFT;
        if (mask & SDL_HAT_RIGHT) dir |= ADAM_DIR_RIGHT;
    }
    return dir;
}

/* A trigger crossing its threshold. Dispatched by poll_sticks once it has
 * let go of the lock: dispatch_button takes it too, and it is not recursive. */
typedef struct {
    SDL_JoystickID devid;
    int button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

static void poll_one(gamepad_state *gst, pad_slot *slot, int port,
                     trigger_edge *edges, int *nedges)
{
    uint8_t want[ADAM_ACT_PER_PORT];
    unsigned acts;
    int dir, act;
    float stickx, sticky;

    memset(want, 0, sizeof want);
    if (port != slot->port) {
        /* moved to another port (or none): release what it held there */
        slot_release_all(gst, slot);
        slot->port = port;
        slot->stick_dir = 0;
        if (port < 0) return;
    }

    /* the left stick (and a raw joystick's hat 0) is the controller's stick */
    dir = slot_hat_dir(slot);
    if (slot->handle || slot->joy) {
        slot_stick(slot, &stickx, &sticky);
        slot->stick_dir = adam_dir_from_stick(stickx, sticky, slot->stick_dir,
                                                ADAM_STICK_ENTER, ADAM_STICK_EXIT);
        dir |= slot->stick_dir;
    }
    acts = adam_acts_from_dir(dir);

    for (act = 0; act < ADAM_ACT_PER_PORT; act++)
        want[act] = (uint8_t)((acts >> act) & 1u);
    for (act = 0; act < ADAM_ACT_PER_PORT; act++) {
        if (want[act] != slot->sent[act]) {
            slot->sent[act] = want[act];
            session_gamepad_stick(gst->session, slot->owner, port, act, want[act]);
        }
    }

    /* the triggers, as buttons */
    if (slot->handle) {
        int trig;
        for (trig = 0; trig < 2; trig++) {
            float level = axis_norm(SDL_GetGamepadAxis(slot->handle,
                trig ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            int down = level >= TRIGGER_ON;
            if (down != slot->trigger_down[trig]) {
                slot->trigger_down[trig] = down;
                if (*nedges < MAX_EDGES) {
                    edges[*nedges].devid = slot->devid;
                    edges[*nedges].button = trig ? ADAM_PAD_BTN_RIGHT_TRIGGER : ADAM_PAD_BTN_LEFT_TRIGGER;
                    edges[*nedges].down = down;
                    (*nedges)++;
                }
            }
        }
    }
}

static void poll_sticks(gamepad_state *gst)
{
    int idx, nedges = 0;
    trigger_edge edges[MAX_EDGES];
    pthread_mutex_lock(&gst->lock);
    for (idx = 0; idx < gst->npads; idx++)
        poll_one(gst, &gst->pads[idx], effective_port_locked(gst, idx), edges, &nedges);
    pthread_mutex_unlock(&gst->lock);
    for (idx = 0; idx < nedges; idx++)
        dispatch_button(gst, edges[idx].devid, edges[idx].button, edges[idx].down);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *gst = arg;
    int count = 0, idx;
    /* Every joystick, not just the ones SDL has a gamepad mapping for:
     * SDL_GetGamepads would silently omit a plain HID adapter entirely. */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (idx = 0; idx < count; idx++) open_pad(gst, ids[idx]);
        SDL_free(ids);
    }
    if (gst->quiet_scan) {
        gst->quiet_scan = 0;
        reassert_held(gst);
    }

    while (!atomic_load(&gst->stop_requested)) {
        SDL_Event evt;
        int npads;

        pthread_mutex_lock(&gst->lock);
        npads = gst->npads;
        pthread_mutex_unlock(&gst->lock);
        while (SDL_PollEvent(&evt)) {
            switch (evt.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(gst, evt.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(gst, evt.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(gst, evt.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(gst, evt.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                dispatch_button(gst, evt.gbutton.which,
                                adam_pad_button_from_sdl(evt.gbutton.button),
                                evt.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also emits these for the same press it
                 * reported as a GAMEPAD_BUTTON event; the slot's kind decides */
                int gamepad, slot_idx;
                pthread_mutex_lock(&gst->lock);
                slot_idx = slot_for_id(gst, evt.jbutton.which);
                gamepad = slot_idx >= 0 && gst->pads[slot_idx].handle != NULL;
                pthread_mutex_unlock(&gst->lock);
                if (gamepad || evt.jbutton.button >= 64) break;
                dispatch_button(gst, evt.jbutton.which,
                                ADAM_PAD_BTN_RAW_BASE + evt.jbutton.button,
                                evt.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                handle_hat_motion(gst, evt.jhat.which, evt.jhat.hat, evt.jhat.value);
                break;
            default:
                break;
            }
        }
        poll_sticks(gst);
        /* ~60 Hz while a pad is attached (the machine's own sampling rate).
         * With none there are only hotplug events to notice, which can wait
         * half a second. (Not SDL_WaitEventTimeout: SDL polls inside it at
         * about 1 kHz.) */
        SDL_Delay(npads > 0 ? 16 : 500);
    }

    pthread_mutex_lock(&gst->lock);
    for (idx = 0; idx < gst->npads; idx++) slot_close(gst, &gst->pads[idx]);
    gst->npads = 0;
    pthread_mutex_unlock(&gst->lock);
    return NULL;
}

int gamepad_start(struct adamsession *sess)
{
    gamepad_state *gst;
    pthread_attr_t attr;
    if (sess->gamepad) return 0;
    /* The app owns its signals; SDL must not intercept SIGINT/SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* Bluetooth pads over hidapi and the joystick subsystem together. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(sess, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    gst = calloc(1, sizeof *gst);
    if (!gst) { SDL_QuitSubSystem(SDL_INIT_GAMEPAD); return -1; }
    gst->session = sess;
    /* the generation goes on from where the last run of the thread left it
     * (a Preferences restart), so a frontend's baseline never goes stale */
    gst->generation = sess->pad_gen_base;
    gst->quiet_scan = sess->pad_restarted;
    /* the remembered assignments, once: garbage or no key is an empty table */
    adam_pad_table_decode(&gst->table, adamsession_get_str(sess, "pad_assign", ""),
                            ADAM_PORTS);
    pthread_mutex_init(&gst->lock, NULL);
    /* An explicit stack: the platform default is 512 KB on macOS, and SDL's
     * HID enumeration runs on this thread. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&gst->thread, &attr, thread_main, gst) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(sess, "gamepad thread could not start");
        pthread_mutex_destroy(&gst->lock);
        free(gst);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    gst->running = 1;
    sess->gamepad = gst;
    return 0;
}

void gamepad_stop(struct adamsession *sess)
{
    gamepad_state *gst = sess->gamepad;
    if (!gst) return;
    atomic_store(&gst->stop_requested, 1);
    pthread_join(gst->thread, NULL);
    sess->pad_gen_base = gst->generation;
    sess->pad_restarted = 1;
    pthread_mutex_destroy(&gst->lock);
    sess->gamepad = NULL;
    free(gst);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---- public API ------------------------------------------------------------- */

int adamsession_gamepad_count(adamsession *sess)
{
    gamepad_state *gst = sess->gamepad;
    int num;
    if (!gst) return 0;
    pthread_mutex_lock(&gst->lock);
    num = gst->npads;
    pthread_mutex_unlock(&gst->lock);
    return num;
}

int adamsession_gamepad_name(adamsession *sess, int idx, char *dst, int dstsz)
{
    gamepad_state *gst = sess->gamepad;
    int num = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!gst) return 0;
    pthread_mutex_lock(&gst->lock);
    if (idx >= 0 && idx < gst->npads)
        num = snprintf(dst, (size_t)dstsz, "%s", slot_name(&gst->pads[idx]));
    pthread_mutex_unlock(&gst->lock);
    return num;
}

void adamsession_gamepad_assign(adamsession *sess, int idx, int port)
{
    gamepad_state *gst = sess->gamepad;
    char text[ADAM_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;
    if (!gst) return;
    pthread_mutex_lock(&gst->lock);
    if (idx >= 0 && idx < gst->npads) {
        pad_slot *slot = &gst->pads[idx];
        if (port >= 0 && port < ADAM_PORTS) {
            slot->assign = port;
            slot->by_name = 0;
            changed = adam_pad_table_set(&gst->table, slot->guidstr, slot->name, port) > 0;
        } else {
            const char *kguid[MAX_PADS], *kname[MAX_PADS];
            int kix, nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (kix = 0; kix < gst->npads; kix++)
                if (kix != idx && gst->pads[kix].assign >= 0) {
                    kguid[nkeep] = gst->pads[kix].guidstr;
                    kname[nkeep++] = gst->pads[kix].name;
                }
            changed = adam_pad_table_remove(&gst->table, slot->guidstr, slot->name,
                                              nkeep, kguid, kname);
        }
        if (changed && adam_pad_table_encode(&gst->table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&gst->lock);
    /* the settings file is written here, on the caller's thread, with the
     * pad lock let go: the pad thread must never wait on a file write */
    if (changed)
        adamsession_set_str(sess, "pad_assign", text);
}

int adamsession_gamepad_assignment(adamsession *sess, int idx)
{
    gamepad_state *gst = sess->gamepad;
    int result = -1;
    if (!gst) return -1;
    pthread_mutex_lock(&gst->lock);
    if (idx >= 0 && idx < gst->npads) result = gst->pads[idx].assign;
    pthread_mutex_unlock(&gst->lock);
    return result;
}

int adamsession_gamepad_effective_port(adamsession *sess, int idx)
{
    gamepad_state *gst = sess->gamepad;
    int result = -1;
    if (!gst) return -1;
    pthread_mutex_lock(&gst->lock);
    if (idx >= 0 && idx < gst->npads) result = effective_port_locked(gst, idx);
    pthread_mutex_unlock(&gst->lock);
    return result;
}

unsigned adamsession_gamepad_generation(adamsession *sess)
{
    gamepad_state *gst = sess->gamepad;
    unsigned result;
    if (!gst) return sess->pad_gen_base;
    pthread_mutex_lock(&gst->lock);
    result = gst->generation;
    pthread_mutex_unlock(&gst->lock);
    return result;
}

void adamsession_gamepad_capture_begin(adamsession *sess)
{
    gamepad_state *gst = sess->gamepad;
    if (!gst) return;
    pthread_mutex_lock(&gst->lock);
    gst->cap_active = 1; gst->cap_have = 0; gst->cap_result = ADAM_PAD_BTN_NONE;
    pthread_mutex_unlock(&gst->lock);
}

void adamsession_gamepad_capture_cancel(adamsession *sess)
{
    gamepad_state *gst = sess->gamepad;
    if (!gst) return;
    pthread_mutex_lock(&gst->lock);
    gst->cap_active = 0; gst->cap_have = 0;
    pthread_mutex_unlock(&gst->lock);
}

int adamsession_gamepad_capture_poll(adamsession *sess, int *button)
{
    gamepad_state *gst = sess->gamepad;
    int got = 0;
    if (!gst) return 0;
    pthread_mutex_lock(&gst->lock);
    if (gst->cap_active && gst->cap_have) {
        if (button) *button = gst->cap_result;
        gst->cap_active = 0; gst->cap_have = 0;
        got = 1;
    }
    pthread_mutex_unlock(&gst->lock);
    return got;
}
