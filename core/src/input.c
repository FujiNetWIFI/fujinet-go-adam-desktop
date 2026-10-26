/*
 * adamsession controllers: merges the keyboard, the keypad window and every
 * gamepad into the two ColecoVision controller words, and translates keys
 * through the bindings table (bindings.c).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <string.h>

#include "bindings.h"
#include "session_internal.h"

static void src_apply(adam_src_state *st, int act, int down)
{
    switch (act) {
    case ADAM_ACT_UP:     if (down) st->dirs |= ADAM_DIR_UP;    else st->dirs &= (uint8_t)~ADAM_DIR_UP;    return;
    case ADAM_ACT_DOWN:   if (down) st->dirs |= ADAM_DIR_DOWN;  else st->dirs &= (uint8_t)~ADAM_DIR_DOWN;  return;
    case ADAM_ACT_LEFT:   if (down) st->dirs |= ADAM_DIR_LEFT;  else st->dirs &= (uint8_t)~ADAM_DIR_LEFT;  return;
    case ADAM_ACT_RIGHT:  if (down) st->dirs |= ADAM_DIR_RIGHT; else st->dirs &= (uint8_t)~ADAM_DIR_RIGHT; return;
    case ADAM_ACT_FIRE_L: if (down) st->fires |= 1; else st->fires &= (uint8_t)~1u; return;
    case ADAM_ACT_FIRE_R: if (down) st->fires |= 2; else st->fires &= (uint8_t)~2u; return;
    default: break;
    }
    if (act < 0 || act >= ADAM_KEYPAD_KEYS) return;
    if (down) {
        st->keys |= (uint16_t)(1u << act);
        st->klast = (int8_t)act;
    } else {
        st->keys &= (uint16_t)~(1u << act);
        if (st->klast == act) {
            int k;
            st->klast = -1;
            for (k = 0; k < ADAM_KEYPAD_KEYS; k++)
                if (st->keys & (1u << k)) { st->klast = (int8_t)k; break; }
        }
    }
}

static void push_locked(adamsession *s, int port)
{
    int dirs = 0, fires = 0, keypad = -1, i;
    for (i = 0; i < ADAM_SRC_COUNT; i++) {
        const adam_src_state *st = &s->src[port][i];
        dirs |= st->dirs;
        fires |= st->fires;
        if (keypad < 0 && st->klast >= 0) keypad = st->klast;
    }
    adamsession_joystick_raw(s, port,
        adam_controller_encode(dirs & ADAM_DIR_UP, dirs & ADAM_DIR_DOWN,
                               dirs & ADAM_DIR_LEFT, dirs & ADAM_DIR_RIGHT,
                               fires & 1, fires & 2, keypad));
}

void input_init(adamsession *s)
{
    int p, i;
    pthread_mutex_init(&s->input_mtx, NULL);
    for (p = 0; p < 2; p++)
        for (i = 0; i < ADAM_SRC_COUNT; i++)
            s->src[p][i].klast = -1;
    bindings_init(s);
}

void input_release_everything(adamsession *s)
{
    int p, i;
    pthread_mutex_lock(&s->input_mtx);
    for (p = 0; p < 2; p++) {
        for (i = 0; i < ADAM_SRC_COUNT; i++) {
            memset(&s->src[p][i], 0, sizeof s->src[p][i]);
            s->src[p][i].klast = -1;
        }
        push_locked(s, p);
    }
    pthread_mutex_unlock(&s->input_mtx);
    memset(s->held_keysym, 0, sizeof s->held_keysym);
    memset(s->chord_keysym, 0, sizeof s->chord_keysym);
}

void input_apply(adamsession *s, int src, int target, int down)
{
    int port = target / ADAM_ACT_PER_PORT;
    if (target < 0 || port > 1) return;
    pthread_mutex_lock(&s->input_mtx);
    src_apply(&s->src[port][src], target % ADAM_ACT_PER_PORT, down);
    push_locked(s, port);
    pthread_mutex_unlock(&s->input_mtx);
}

void input_pad_set(adamsession *s, int slot, int port, int dirs, int fires,
                   uint16_t keys)
{
    int p, k;
    adam_src_state st;

    if (slot < 0 || slot >= ADAM_MAX_PADS || port < 0 || port > 1) return;
    memset(&st, 0, sizeof st);
    st.dirs = (uint8_t)dirs;
    st.fires = (uint8_t)fires;
    st.keys = keys;
    st.klast = -1;
    for (k = 0; k < ADAM_KEYPAD_KEYS; k++)
        if (keys & (1u << k)) { st.klast = (int8_t)k; break; }

    pthread_mutex_lock(&s->input_mtx);
    for (p = 0; p < 2; p++) {
        adam_src_state *cur = &s->src[p][ADAM_SRC_PAD + slot];
        adam_src_state next;
        if (p == port) {
            next = st;
        } else {
            memset(&next, 0, sizeof next);
            next.klast = -1;
        }
        if (memcmp(cur, &next, sizeof next) != 0) {
            *cur = next;
            push_locked(s, p);
        }
    }
    pthread_mutex_unlock(&s->input_mtx);
}

void input_pad_clear(adamsession *s, int slot)
{
    input_pad_set(s, slot, 0, 0, 0, 0);
}

/* ---- public ----------------------------------------------------------------- */

void adamsession_sysaction(adamsession *s, int sysact)
{
    switch (sysact) {
    case ADAM_SYSACT_RESET_COMPUTER: adamsession_reset(s, 0); break;
    case ADAM_SYSACT_RESET_GAME:     adamsession_reset(s, 1); break;
    default: break;
    }
}

void adamsession_press(adamsession *s, int target, int down)
{
    if (!s || target < 0 || target >= ADAM_TARGET_COUNT) return;
    if (target >= ADAM_TARGET_SYSACT(0)) {
        if (down) adamsession_sysaction(s, target - ADAM_TARGET_SYSACT(0));
        return;
    }
    input_apply(s, ADAM_SRC_PANEL, target, down);
}

uint32_t adamsession_buttons_held(adamsession *s, int port)
{
    uint32_t bits = 0;
    int i;
    if (!s || port < 0 || port > 1) return 0;
    pthread_mutex_lock(&s->input_mtx);
    for (i = 0; i < ADAM_SRC_COUNT; i++) {
        const adam_src_state *st = &s->src[port][i];
        bits |= st->keys;
        if (st->dirs & ADAM_DIR_UP)    bits |= 1u << ADAM_ACT_UP;
        if (st->dirs & ADAM_DIR_DOWN)  bits |= 1u << ADAM_ACT_DOWN;
        if (st->dirs & ADAM_DIR_LEFT)  bits |= 1u << ADAM_ACT_LEFT;
        if (st->dirs & ADAM_DIR_RIGHT) bits |= 1u << ADAM_ACT_RIGHT;
        if (st->fires & 1)             bits |= 1u << ADAM_ACT_FIRE_L;
        if (st->fires & 2)             bits |= 1u << ADAM_ACT_FIRE_R;
    }
    pthread_mutex_unlock(&s->input_mtx);
    return bits;
}

int adamsession_input_key(adamsession *s, uint32_t keysym, int down,
                          int ctrl_down)
{
    uint32_t k = bindings_fold(keysym);
    int targets[ADAM_TARGET_COUNT], n, i, hit = 0;

    if (!s || !k) return 0;

    if (down) {
        n = bindings_targets_for_key(k, targets);
        for (i = 0; i < n; i++) {
            int t = targets[i];
            hit = 1;
            if (s->held_keysym[t] == k) continue; /* auto-repeat */
            s->held_keysym[t] = k;
            if (t >= ADAM_TARGET_SYSACT(0))
                adamsession_sysaction(s, t - ADAM_TARGET_SYSACT(0));
            else
                input_apply(s, ADAM_SRC_KEYBOARD, t, 1);
        }
        if (!n && ctrl_down) {
            int digit = -1;
            if (k >= '0' && k <= '9') digit = (int)(k - '0');
            else if (k >= ADAMSESSION_KEYSYM_KP_0 && k <= ADAMSESSION_KEYSYM_KP_9)
                digit = (int)(k - ADAMSESSION_KEYSYM_KP_0);
            if (digit >= 0) {
                hit = 1;
                if (s->chord_keysym[digit] != k) {
                    s->chord_keysym[digit] = k;
                    input_apply(s, ADAM_SRC_KEYBOARD, ADAM_TARGET_PORT(0, digit), 1);
                }
            }
        }
        return hit;
    }

    /* A release clears exactly what the press asserted, even if the binding
     * or the Ctrl state changed in between. */
    for (i = 0; i < ADAM_TARGET_COUNT; i++) {
        if (s->held_keysym[i] != k) continue;
        s->held_keysym[i] = 0;
        hit = 1;
        if (i < ADAM_TARGET_SYSACT(0))
            input_apply(s, ADAM_SRC_KEYBOARD, i, 0);
    }
    for (i = 0; i < 10; i++) {
        if (s->chord_keysym[i] != k) continue;
        s->chord_keysym[i] = 0;
        hit = 1;
        input_apply(s, ADAM_SRC_KEYBOARD, ADAM_TARGET_PORT(0, i), 0);
    }
    return hit;
}

void adamsession_release_all(adamsession *s)
{
    int p, i;
    if (!s) return;
    pthread_mutex_lock(&s->input_mtx);
    for (p = 0; p < 2; p++) {
        memset(&s->src[p][ADAM_SRC_KEYBOARD], 0, sizeof s->src[p][ADAM_SRC_KEYBOARD]);
        s->src[p][ADAM_SRC_KEYBOARD].klast = -1;
        push_locked(s, p);
    }
    pthread_mutex_unlock(&s->input_mtx);
    for (i = 0; i < ADAM_TARGET_COUNT; i++) s->held_keysym[i] = 0;
    memset(s->chord_keysym, 0, sizeof s->chord_keysym);
}
