/*
 * input -- every way a hand controller gets held, and the one word per
 * controller the machine sees.
 *
 * The inputs are held by sources (session_internal.h SRC_*: the keyboard, the
 * Controllers window's mouse, and for each gamepad its buttons and its stick),
 * one bit per source in an array per action, so letting go of one source
 * never releases another's. After every change the result is encoded and
 * handed to the machine (adamsession_joystick_raw), which samples it once a
 * frame.
 *
 * A direction pair held both ways (UP and DOWN, LEFT and RIGHT) holds
 * neither, as a stick's switches cannot close both. The keypad strobe carries
 * ONE key (adamcore's keypad_code is an index), so when several are held the
 * lowest wins; adamsession_buttons_held reports exactly what the console
 * sees. Nothing here needs the machine to be running: with no core the push
 * is a no-op and the held state stays readable, which is what lets the tests
 * run on a CI box with no BIOS.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "bindings.h"
#include "session_internal.h"

#define IDLE_WORD 0x7F7F

void input_init(struct adamsession *sess)
{
    int port;
    pthread_mutex_init(&sess->input_mtx, NULL);
    pthread_mutex_init(&sess->sysact_mtx, NULL);
    pthread_mutex_init(&sess->pad_event_mtx, NULL);
    for (port = 0; port < ADAM_PORTS; port++)
        sess->host_word[port] = IDLE_WORD;
    bindings_init(sess);
}

/* ---- what the console sees ---------------------------------------------------- */

/* input_mtx held. */
static unsigned held_locked(const struct adamsession *sess, int port)
{
    unsigned mask = 0;
    int act;

    for (act = 0; act < ADAM_ACT_PER_PORT; act++)
        if (sess->pad_src[port][act])
            mask |= 1u << act;
    if ((mask & (1u << ADAM_ACT_UP)) && (mask & (1u << ADAM_ACT_DOWN)))
        mask &= ~((1u << ADAM_ACT_UP) | (1u << ADAM_ACT_DOWN));
    if ((mask & (1u << ADAM_ACT_LEFT)) && (mask & (1u << ADAM_ACT_RIGHT)))
        mask &= ~((1u << ADAM_ACT_LEFT) | (1u << ADAM_ACT_RIGHT));
    /* the keypad strobe holds one key: the lowest wins */
    for (act = 0; act < ADAM_KEYPAD_KEYS; act++)
        if (mask & (1u << act)) {
            mask &= ~((1u << ADAM_KEYPAD_KEYS) - 1u);
            mask |= 1u << act;
            break;
        }
    return mask;
}

static void push_port_locked(struct adamsession *sess, int port)
{
    const unsigned held = held_locked(sess, port);
    int keypad = -1, act;

    for (act = 0; act < ADAM_KEYPAD_KEYS; act++)
        if (held & (1u << act))
            keypad = act;
    sess->host_word[port] = adam_controller_encode(
        (held >> ADAM_ACT_UP) & 1u, (held >> ADAM_ACT_DOWN) & 1u,
        (held >> ADAM_ACT_LEFT) & 1u, (held >> ADAM_ACT_RIGHT) & 1u,
        (held >> ADAM_ACT_FIRE_L) & 1u, (held >> ADAM_ACT_FIRE_R) & 1u,
        keypad);
    adamsession_joystick_raw(sess, port, sess->host_word[port]);
}

static void push_all_locked(struct adamsession *sess)
{
    int port;
    for (port = 0; port < ADAM_PORTS; port++)
        push_port_locked(sess, port);
}

/* A stop: nothing is held any more, and the machine (if still there) is
 * told. Keys, chords, the mouse and the pads' bits all go. */
void input_release_everything(struct adamsession *sess)
{
    pthread_mutex_lock(&sess->input_mtx);
    memset(sess->pad_src, 0, sizeof sess->pad_src);
    memset(sess->held_keysym, 0, sizeof sess->held_keysym);
    memset(sess->chord_keysym, 0, sizeof sess->chord_keysym);
    push_all_locked(sess);
    pthread_mutex_unlock(&sess->input_mtx);
}

/* A new core starts idle: hand it whatever is held. */
void input_resend(struct adamsession *sess)
{
    pthread_mutex_lock(&sess->input_mtx);
    push_all_locked(sess);
    pthread_mutex_unlock(&sess->input_mtx);
}

static void set_src(uint32_t *word, uint32_t src, int down)
{
    if (down)
        *word |= src;
    else
        *word &= ~src;
}

/* One source of one controller target; system actions are not held inputs and
 * are left to the caller. input_mtx held. */
static void source_press_locked(struct adamsession *sess, int target,
                                uint32_t src, int down)
{
    const int port = target / ADAM_ACT_PER_PORT;
    set_src(&sess->pad_src[port][target % ADAM_ACT_PER_PORT], src, down);
    push_port_locked(sess, port);
}

/* ---- the public input API ------------------------------------------------------- */

void adamsession_press(adamsession *sess, int target, int down)
{
    if (!sess || target < 0 || target >= ADAM_TARGET_COUNT)
        return;
    if (target >= ADAM_TARGET_SYSACT(0)) {
        if (down)
            adamsession_sysaction(sess, target - ADAM_TARGET_SYSACT(0));
        return;
    }
    pthread_mutex_lock(&sess->input_mtx);
    source_press_locked(sess, target, SRC_KEYBOARD, down);
    pthread_mutex_unlock(&sess->input_mtx);
}

static void key_hold(struct adamsession *sess, int target, uint32_t pressed)
{
    if (target < 0 || target >= ADAM_TARGET_SYSACT(0))
        return;
    pthread_mutex_lock(&sess->input_mtx);
    if (sess->held_keysym[target] != pressed) {   /* not an auto-repeat */
        sess->held_keysym[target] = pressed;
        source_press_locked(sess, target, SRC_KEYBOARD, 1);
    }
    pthread_mutex_unlock(&sess->input_mtx);
}

static int key_release(struct adamsession *sess, uint32_t pressed)
{
    int target, count = 0;

    if (!pressed)          /* 0 is "nothing held" in held_keysym, not a key */
        return 0;
    pthread_mutex_lock(&sess->input_mtx);
    for (target = 0; target < ADAM_TARGET_COUNT; target++)
        if (sess->held_keysym[target] == pressed) {
            sess->held_keysym[target] = 0;
            if (target < ADAM_TARGET_SYSACT(0))
                source_press_locked(sess, target, SRC_KEYBOARD, 0);
            count++;
        }
    pthread_mutex_unlock(&sess->input_mtx);
    return count;
}

void adamsession_sysaction_post(adamsession *sess, int sysact)
{
    if (sysact < 0 || sysact >= ADAM_SYSACT_COUNT)
        return;
    pthread_mutex_lock(&sess->sysact_mtx);
    sess->sysact_pending |= 1u << sysact;
    pthread_mutex_unlock(&sess->sysact_mtx);
}

int adamsession_sysaction_take(adamsession *sess, int *sysact)
{
    int found = 0, idx;

    pthread_mutex_lock(&sess->sysact_mtx);
    for (idx = 0; idx < ADAM_SYSACT_COUNT; idx++) {
        if (sess->sysact_pending & (1u << idx)) {
            sess->sysact_pending &= ~(1u << idx);
            if (sysact)
                *sysact = idx;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&sess->sysact_mtx);
    return found;
}

unsigned adamsession_buttons_held(adamsession *sess, int port)
{
    unsigned held;
    if (!sess || port < 0 || port >= ADAM_PORTS)
        return 0;
    pthread_mutex_lock(&sess->input_mtx);
    held = held_locked(sess, port);
    pthread_mutex_unlock(&sess->input_mtx);
    return held;
}

uint16_t adamsession_controller_word(adamsession *sess, int port)
{
    uint16_t word = IDLE_WORD;
    if (!sess || port < 0 || port >= ADAM_PORTS)
        return word;
    pthread_mutex_lock(&sess->input_mtx);
    word = sess->host_word[port];
    pthread_mutex_unlock(&sess->input_mtx);
    return word;
}

void adamsession_pointer_set(adamsession *sess, int port, unsigned mask)
{
    int act;
    if (!sess || port < 0 || port >= ADAM_PORTS)
        return;
    pthread_mutex_lock(&sess->input_mtx);
    for (act = 0; act < ADAM_ACT_PER_PORT; act++)
        set_src(&sess->pad_src[port][act], SRC_POINTER, (mask >> act) & 1u);
    push_port_locked(sess, port);
    pthread_mutex_unlock(&sess->input_mtx);
}

/* Everything the keyboard holds, let go (focus loss): what a key's press
 * asserted, whichever way it was asserted. A gamepad's own holds are left to
 * the gamepad, the mouse's to adamsession_pointer_set. */
void adamsession_release_all(adamsession *sess)
{
    int port, act;
    if (!sess)
        return;
    pthread_mutex_lock(&sess->input_mtx);
    memset(sess->held_keysym, 0, sizeof sess->held_keysym);
    memset(sess->chord_keysym, 0, sizeof sess->chord_keysym);
    for (port = 0; port < ADAM_PORTS; port++) {
        for (act = 0; act < ADAM_ACT_PER_PORT; act++)
            sess->pad_src[port][act] &= ~SRC_KEYBOARD;
        push_port_locked(sess, port);
    }
    pthread_mutex_unlock(&sess->input_mtx);
}

/* The gamepad thread's entry points: the same routing, from the pad's own
 * source bits, never touching the settings or the machine's state. */
void session_gamepad_press(struct adamsession *sess, int owner, int target, int down)
{
    if (owner < 0 || owner >= ADAM_MAX_PADS || target < 0
        || target >= ADAM_TARGET_SYSACT(0))
        return;
    pthread_mutex_lock(&sess->input_mtx);
    source_press_locked(sess, target, SRC_PAD(owner), down);
    pthread_mutex_unlock(&sess->input_mtx);
}

void session_gamepad_stick(struct adamsession *sess, int owner, int port, int act, int down)
{
    if (owner < 0 || owner >= ADAM_MAX_PADS || port < 0 || port >= ADAM_PORTS
        || act < 0 || act >= ADAM_ACT_PER_PORT)
        return;
    pthread_mutex_lock(&sess->input_mtx);
    source_press_locked(sess, ADAM_TARGET_PORT(port, act), SRC_STICK(owner), down);
    pthread_mutex_unlock(&sess->input_mtx);
}

/* ---- system actions, and the keyboard's entry point ----------------------------- */

void adamsession_sysaction(adamsession *sess, int sysact)
{
    switch (sysact) {
    case ADAM_SYSACT_RESET_COMPUTER: adamsession_reset(sess, 0); break;
    case ADAM_SYSACT_RESET_GAME:     adamsession_reset(sess, 1); break;
    default: break;
    }
}

/* Ctrl + a digit dials that keypad digit on controller 1, when the digit is
 * not itself bound. Remembered by the key, like a bound key's hold. */
static void chord_hold(struct adamsession *sess, int digit, uint32_t pressed)
{
    pthread_mutex_lock(&sess->input_mtx);
    if (sess->chord_keysym[digit] != pressed) {
        sess->chord_keysym[digit] = pressed;
        source_press_locked(sess, ADAM_TARGET_PORT(0, digit), SRC_KEYBOARD, 1);
    }
    pthread_mutex_unlock(&sess->input_mtx);
}

static int chord_release(struct adamsession *sess, uint32_t pressed)
{
    int digit, count = 0;
    pthread_mutex_lock(&sess->input_mtx);
    for (digit = 0; digit < 10; digit++)
        if (sess->chord_keysym[digit] == pressed) {
            sess->chord_keysym[digit] = 0;
            source_press_locked(sess, ADAM_TARGET_PORT(0, digit), SRC_KEYBOARD, 0);
            count++;
        }
    pthread_mutex_unlock(&sess->input_mtx);
    return count;
}

int adamsession_input_key(adamsession *sess, uint32_t keysym, int down,
                          int ctrl_down)
{
    const uint32_t pressed = bindings_fold(keysym);
    int targets[ADAM_TARGET_COUNT], count, idx, hit = 0;

    if (!sess || !pressed)
        return 0;

    if (!down) {
        /* a release clears exactly what the press asserted, even if the
         * binding or the Ctrl state changed in between */
        hit = key_release(sess, pressed);
        hit += chord_release(sess, pressed);
        return hit > 0;
    }

    count = bindings_targets_for_key(pressed, targets, ADAM_TARGET_COUNT);
    for (idx = 0; idx < count; idx++) {
        const int tgt = targets[idx];
        hit = 1;
        if (tgt >= ADAM_TARGET_SYSACT(0)) {
            /* acts on the leading edge only: held_keysym remembers the press
             * so the release is consumed and auto-repeat does not repeat it */
            int fresh;
            pthread_mutex_lock(&sess->input_mtx);
            fresh = sess->held_keysym[tgt] != pressed;
            sess->held_keysym[tgt] = pressed;
            pthread_mutex_unlock(&sess->input_mtx);
            if (fresh)
                adamsession_sysaction(sess, tgt - ADAM_TARGET_SYSACT(0));
        } else {
            key_hold(sess, tgt, pressed);
        }
    }
    if (!count && ctrl_down) {
        int digit = -1;
        if (pressed >= '0' && pressed <= '9')
            digit = (int)(pressed - '0');
        else if (pressed >= ADAMSESSION_KEYSYM_KP_0 && pressed <= ADAMSESSION_KEYSYM_KP_9)
            digit = (int)(pressed - ADAMSESSION_KEYSYM_KP_0);
        if (digit >= 0) {
            hit = 1;
            chord_hold(sess, digit, pressed);
        }
    }
    return hit;
}

uint16_t adamsession_gamepad_last_state(adamsession *sess, int port)
{
    return adamsession_controller_word(sess, port);
}
