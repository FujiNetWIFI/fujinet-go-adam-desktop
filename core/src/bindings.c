/*
 * bindings -- the remappable key/gamepad table and every name the keypad
 * window and the settings store need.
 *
 * One flat target table (adamsession.h's ADAM_TARGET_* indices): per port the
 * keypad, stick and fire buttons, then the session's own actions. Each target
 * holds at most one keysym and one gamepad button. Rebinding STEALS: a key
 * drives exactly one target, because one keystroke doing two things is worse
 * than losing the old binding.
 *
 * Seeds its defaults lazily so the table is usable with no session behind it
 * (a unit test gets the documented default map). Persisted as one packed
 * "bindings" key holding only the entries that differ from the defaults
 * ("<target>.k:<keysym>" / "<target>.b:<button>"). Keysyms are X11's, folded
 * to lower case before lookup. Ported from fujinet-go-sms-desktop.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bindings.h"
#include "hid_keys.h"
#include "session_internal.h"

typedef struct {
    uint32_t keysym;
    int button;
} slot;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static slot s_table[ADAM_TARGET_COUNT];
static slot s_defaults[ADAM_TARGET_COUNT];
static int s_seeded;
static adamsession *s_store; /* whose settings hold "bindings" */

/* ---- defaults ------------------------------------------------------------- */

static void seed_key(int target, uint32_t keysym)
{
    s_defaults[target].keysym = keysym;
}

/* Controller 1 on the numeric keypad, the way a joystick would sit under the
 * right hand: 8/4/6/2 steer, 0 is the left fire button, Enter the right one,
 * '*' and '#' on Keypad * and /. The digit keys and every other key stay the
 * ADAM keyboard's; Ctrl + a digit still dials a keypad digit (adamsession_
 * input_key). Controller 2 and the system actions start unbound. */
static void compute_defaults_locked(void)
{
    int t;
    for (t = 0; t < ADAM_TARGET_COUNT; t++) {
        s_defaults[t].keysym = 0;
        s_defaults[t].button = ADAM_PAD_BTN_NONE;
    }

    seed_key(ADAM_TARGET_PORT(0, ADAM_ACT_UP), ADAMSESSION_KEYSYM_KP_8);
    seed_key(ADAM_TARGET_PORT(0, ADAM_ACT_DOWN), ADAMSESSION_KEYSYM_KP_2);
    seed_key(ADAM_TARGET_PORT(0, ADAM_ACT_LEFT), ADAMSESSION_KEYSYM_KP_4);
    seed_key(ADAM_TARGET_PORT(0, ADAM_ACT_RIGHT), ADAMSESSION_KEYSYM_KP_6);
    seed_key(ADAM_TARGET_PORT(0, ADAM_ACT_FIRE_L), ADAMSESSION_KEYSYM_KP_0);
    seed_key(ADAM_TARGET_PORT(0, ADAM_ACT_FIRE_R), ADAMSESSION_KEYSYM_KP_ENTER);
    seed_key(ADAM_TARGET_PORT(0, ADAM_KEYPAD_KEYS - 2), ADAMSESSION_KEYSYM_KP_MULTIPLY);
    seed_key(ADAM_TARGET_PORT(0, ADAM_KEYPAD_KEYS - 1), ADAMSESSION_KEYSYM_KP_DIVIDE);

    /* gamepads: South is the left fire button, East the right; the stick and
     * D-pad always steer (gamepad_sdl.c), so directions are not bound. */
    {
        int port;
        for (port = 0; port < 2; port++) {
            s_defaults[ADAM_TARGET_PORT(port, ADAM_ACT_FIRE_L)].button = ADAM_PAD_BTN_SOUTH;
            s_defaults[ADAM_TARGET_PORT(port, ADAM_ACT_FIRE_R)].button = ADAM_PAD_BTN_EAST;
        }
    }
}

uint32_t bindings_fold(uint32_t keysym)
{
    if (keysym >= 'A' && keysym <= 'Z') return keysym + 32;
    return keysym;
}

/* ---- persistence ---------------------------------------------------------- */

static void apply_persisted_locked(const char *packed)
{
    const char *p = packed;
    while (p && *p) {
        int target = -1; char kind = 0; long value = 0;
        const char *end = strchr(p, ' ');
        if (sscanf(p, "%d.%c:%ld", &target, &kind, &value) == 3
            && target >= 0 && target < ADAM_TARGET_COUNT) {
            if (kind == 'k') s_table[target].keysym = (uint32_t)value;
            else if (kind == 'b') s_table[target].button = (int)value;
        }
        if (!end) break;
        p = end + 1;
    }
}

static void pack_locked(char *out, size_t outsz)
{
    size_t len = 0;
    int t;
    out[0] = '\0';
    for (t = 0; t < ADAM_TARGET_COUNT; t++) {
        if (s_table[t].keysym != s_defaults[t].keysym)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.k:%u",
                                    len ? " " : "", t, s_table[t].keysym);
        if (len >= outsz) break;
        if (s_table[t].button != s_defaults[t].button)
            len += (size_t)snprintf(out + len, outsz - len, "%s%d.b:%d",
                                    len ? " " : "", t, s_table[t].button);
        if (len >= outsz) break;
    }
}

static void persist_locked(void)
{
    char packed[4096];
    if (!s_store) return;
    pack_locked(packed, sizeof packed);
    adamsession_set_str(s_store, "bindings", packed);
}

static void ensure_seeded_locked(void)
{
    if (s_seeded) return;
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
}

void bindings_init(adamsession *s)
{
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    s_store = s;
    if (s)
        apply_persisted_locked(adamsession_get_str(s, "bindings", ""));
    pthread_mutex_unlock(&s_lock);
}

/* ---- lookups -------------------------------------------------------------- */

uint32_t bindings_keysym_of(int target)
{
    uint32_t k = 0;
    if (target < 0 || target >= ADAM_TARGET_COUNT) return 0;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    k = s_table[target].keysym;
    pthread_mutex_unlock(&s_lock);
    return k;
}

int bindings_button_of(int target)
{
    int b = ADAM_PAD_BTN_NONE;
    if (target < 0 || target >= ADAM_TARGET_COUNT) return b;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    b = s_table[target].button;
    pthread_mutex_unlock(&s_lock);
    return b;
}

int bindings_targets_for_key(uint32_t keysym, int *out)
{
    int t, n = 0;
    keysym = bindings_fold(keysym);
    if (!keysym) return 0;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (t = 0; t < ADAM_TARGET_COUNT; t++)
        if (s_table[t].keysym == keysym) out[n++] = t;
    pthread_mutex_unlock(&s_lock);
    return n;
}

int adamsession_target_for_key(adamsession *s, uint32_t keysym)
{
    int t[ADAM_TARGET_COUNT];
    (void)s;
    return bindings_targets_for_key(keysym, t) ? t[0] : -1;
}

/* A pad button is scoped to the pad's port: the same button may mean
 * different things on different ports, so only that port's targets and the
 * machine-wide system actions are searched. */
int adamsession_target_for_button(adamsession *s, int port, int button)
{
    int t, found = -1;
    (void)s;
    if (button == ADAM_PAD_BTN_NONE) return -1;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    for (t = 0; t < ADAM_ACT_PER_PORT; t++)
        if (s_table[ADAM_TARGET_PORT(port, t)].button == button) {
            found = ADAM_TARGET_PORT(port, t);
            break;
        }
    if (found < 0)
        for (t = 2 * ADAM_ACT_PER_PORT; t < ADAM_TARGET_COUNT; t++)
            if (s_table[t].button == button) { found = t; break; }
    pthread_mutex_unlock(&s_lock);
    return found;
}

adam_binding adamsession_binding_get(adamsession *s, int target)
{
    adam_binding b = { 0, ADAM_PAD_BTN_NONE };
    (void)s;
    if (target < 0 || target >= ADAM_TARGET_COUNT) return b;
    b.keysym = bindings_keysym_of(target);
    b.button = bindings_button_of(target);
    return b;
}

/* An explicit rebinding is the user saying "this key does exactly this", so
 * it displaces every holder, and names them all so nothing is lost silently. */
static void describe_stolen(const int *victims, int nvictims, char *stolen,
                            int stolensz)
{
    int i, len = 0;
    if (!stolen || stolensz <= 0) return;
    stolen[0] = '\0';
    for (i = 0; i < nvictims && len < stolensz; i++)
        len += snprintf(stolen + len, (size_t)(stolensz - len), "%s%s",
                        i ? ", " : "", adam_target_name(victims[i]));
}

void adamsession_binding_set_key(adamsession *s, int target, uint32_t keysym,
                                 char *stolen, int stolensz)
{
    int victims[ADAM_TARGET_COUNT], nvictims = 0, t;
    (void)s;
    if (target < 0 || target >= ADAM_TARGET_COUNT) return;
    keysym = bindings_fold(keysym);
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (keysym) {
        for (t = 0; t < ADAM_TARGET_COUNT; t++)
            if (t != target && s_table[t].keysym == keysym) {
                s_table[t].keysym = 0;
                victims[nvictims++] = t;
            }
    }
    s_table[target].keysym = keysym;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void adamsession_binding_set_button(adamsession *s, int target, int button,
                                    char *stolen, int stolensz)
{
    int victims[ADAM_TARGET_COUNT], nvictims = 0, t;
    int port = target / ADAM_ACT_PER_PORT; /* 2 = system actions */
    (void)s;
    if (target < 0 || target >= ADAM_TARGET_COUNT) return;
    pthread_mutex_lock(&s_lock);
    ensure_seeded_locked();
    if (button != ADAM_PAD_BTN_NONE) {
        for (t = 0; t < ADAM_TARGET_COUNT; t++) {
            int tport = t / ADAM_ACT_PER_PORT;
            /* steal only within the same scope: this port, or the
             * machine-wide targets, which every pad reaches */
            if (t != target && s_table[t].button == button
                && (tport == port || tport >= 2 || port >= 2)) {
                s_table[t].button = ADAM_PAD_BTN_NONE;
                victims[nvictims++] = t;
            }
        }
    }
    s_table[target].button = button;
    persist_locked();
    pthread_mutex_unlock(&s_lock);
    describe_stolen(victims, nvictims, stolen, stolensz);
}

void adamsession_bindings_reset(adamsession *s)
{
    (void)s;
    pthread_mutex_lock(&s_lock);
    compute_defaults_locked();
    memcpy(s_table, s_defaults, sizeof s_table);
    s_seeded = 1;
    if (s_store) adamsession_set_str(s_store, "bindings", "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- names ---------------------------------------------------------------- */

static const char *const keypad_names[ADAM_KEYPAD_KEYS] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#",
};
static const char *const act_names[ADAM_ACT_PER_PORT - ADAM_KEYPAD_KEYS] = {
    "Up", "Down", "Left", "Right", "Fire L", "Fire R",
};
static const char *const sysact_names[ADAM_SYSACT_COUNT] = {
    "Reset Computer", "Reset Game",
};

const char *adam_target_short_name(int target)
{
    static char buf[16];
    if (target < 0 || target >= ADAM_TARGET_COUNT) return "";
    if (target >= 2 * ADAM_ACT_PER_PORT)
        return sysact_names[target - 2 * ADAM_ACT_PER_PORT];
    target %= ADAM_ACT_PER_PORT;
    if (target < ADAM_KEYPAD_KEYS) {
        snprintf(buf, sizeof buf, "Key %s", keypad_names[target]);
        return buf;
    }
    return act_names[target - ADAM_KEYPAD_KEYS];
}

const char *adam_target_name(int target)
{
    static char buf[64];
    if (target < 0 || target >= ADAM_TARGET_COUNT) return "";
    if (target >= 2 * ADAM_ACT_PER_PORT)
        return sysact_names[target - 2 * ADAM_ACT_PER_PORT];
    snprintf(buf, sizeof buf, "Controller %d: %s",
             target / ADAM_ACT_PER_PORT + 1, adam_target_short_name(target));
    return buf;
}

static const char *const pad_button_names[ADAM_PAD_BTN_COUNT] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick", "Right Stick",
    "Left Shoulder", "Right Shoulder", "D-pad Up", "D-pad Down", "D-pad Left",
    "D-pad Right", "Left Trigger", "Right Trigger",
};

const char *adam_pad_button_name(int button)
{
    if (button >= 0 && button < ADAM_PAD_BTN_COUNT) return pad_button_names[button];
    return "";
}

int adamsession_keysym_name(uint32_t keysym, char *dst, int dstsz)
{
    const char *name = NULL;
    if (!dst || dstsz <= 0) return 0;
    switch (keysym) {
    case 0: name = ""; break;
    case ADAMSESSION_KEYSYM_UP: name = "Up"; break;
    case ADAMSESSION_KEYSYM_DOWN: name = "Down"; break;
    case ADAMSESSION_KEYSYM_LEFT: name = "Left"; break;
    case ADAMSESSION_KEYSYM_RIGHT: name = "Right"; break;
    case ADAMSESSION_KEYSYM_ESCAPE: name = "Escape"; break;
    case ADAMSESSION_KEYSYM_RETURN: name = "Return"; break;
    case ADAMSESSION_KEYSYM_BACKSPACE: name = "Backspace"; break;
    case ADAMSESSION_KEYSYM_TAB: name = "Tab"; break;
    case ADAMSESSION_KEYSYM_SPACE: name = "Space"; break;
    case ADAMSESSION_KEYSYM_LSHIFT: name = "Left Shift"; break;
    case ADAMSESSION_KEYSYM_RSHIFT: name = "Right Shift"; break;
    case ADAMSESSION_KEYSYM_LCTRL: name = "Left Ctrl"; break;
    case ADAMSESSION_KEYSYM_RCTRL: name = "Right Ctrl"; break;
    case ADAMSESSION_KEYSYM_LALT: name = "Left Alt"; break;
    case ADAMSESSION_KEYSYM_RALT: name = "Right Alt"; break;
    case ADAMSESSION_KEYSYM_KP_ENTER: name = "Keypad Enter"; break;
    case ADAMSESSION_KEYSYM_KP_MULTIPLY: name = "Keypad *"; break;
    case ADAMSESSION_KEYSYM_KP_DIVIDE: name = "Keypad /"; break;
    case ADAMSESSION_KEYSYM_KP_PERIOD: name = "Keypad ."; break;
    case 0xffff: name = "Delete"; break;
    case 0xff63: name = "Insert"; break;
    case 0xff50: name = "Home"; break;
    case 0xff57: name = "End"; break;
    case 0xff55: name = "Page Up"; break;
    case 0xff56: name = "Page Down"; break;
    case 0xffe5: name = "Caps Lock"; break;
    default: break;
    }
    if (name) return snprintf(dst, (size_t)dstsz, "%s", name);
    if (keysym >= ADAMSESSION_KEYSYM_F1 && keysym <= ADAMSESSION_KEYSYM_F12)
        return snprintf(dst, (size_t)dstsz, "F%u", keysym - ADAMSESSION_KEYSYM_F1 + 1);
    if (keysym >= ADAMSESSION_KEYSYM_KP_0 && keysym <= ADAMSESSION_KEYSYM_KP_9)
        return snprintf(dst, (size_t)dstsz, "Keypad %u", keysym - ADAMSESSION_KEYSYM_KP_0);
    if (keysym >= 0x21 && keysym <= 0x7e) {
        char c = (char)keysym;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        return snprintf(dst, (size_t)dstsz, "%c", c);
    }
    if (keysym >= ADAMSESSION_KEYSYM_HID_BASE
        && keysym <= ADAMSESSION_KEYSYM_HID_BASE + ADAMSESSION_HID_USAGE_MAX) {
        const char *hid = adam_hid_usage_name(keysym - ADAMSESSION_KEYSYM_HID_BASE);
        if (hid) return snprintf(dst, (size_t)dstsz, "%s", hid);
        return snprintf(dst, (size_t)dstsz, "HID 0x%02X",
                        (unsigned)(keysym - ADAMSESSION_KEYSYM_HID_BASE));
    }
    return snprintf(dst, (size_t)dstsz, "Key 0x%X", keysym);
}
