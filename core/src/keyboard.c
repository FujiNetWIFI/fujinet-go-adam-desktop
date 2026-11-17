/*
 * keyboard -- the on-screen keyboard: the ADAM's 75 key caps as they are
 * printed on its full-travel board (see DESIGN-adam.md: layout read off
 * photographs of the real keyboard).
 *
 * A cap names the byte the ADAM keyboard sends (the same bytes the host
 * keyboard path sends through adam_key_from_event): ASCII for the printing
 * keys, 0x80-0x86 for ESCAPE/WP and the SmartKeys, 0x90-0x97 for the editing
 * block (+8 with SHIFT), 0xA0-0xA3 for the cursor keys and 0xA8 for HOME.
 * The keyboard reports a key once, when it goes down; it has no key-up. So a
 * cap sends on the press, and its "held" light is a short flash.
 *
 * SHIFT (both keys), CONTROL and LOCK are latches here, as there is no way to
 * hold a cap while pressing another with one mouse: SHIFT and CONTROL are
 * spent by the next ordinary key; LOCK stays until it is pressed again.
 *
 * Geometry: units are key caps (a key is 1.0 x 1.0, the gap between caps 0.1
 * already in the coordinates, so the pitch is 1.1). The key set and what each
 * key sends are exact; the positions follow the photographs and are
 * approximate.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>
#include <string.h>
#include <time.h>

#include "session_internal.h"

#define GAP   0.1f
#define PITCH (1.0f + GAP)

/* A cap `wid` pitches wide at column `col` of row `row` (both in pitches). */
#define KEY(lbl, shf, plain, shifted, col, row, wid, kind) \
    { lbl, shf, plain, shifted, (col) * PITCH, (row) * PITCH, (wid) * PITCH - GAP, 1.0f, kind }
#define TALL(lbl, plain, shifted, col, row, wid, rows, kind) \
    { lbl, NULL, plain, shifted, (col) * PITCH, (row) * PITCH, (wid) * PITCH - GAP, \
      (rows) * PITCH - GAP, kind }
#define NRM ADAM_KEYCAP_NORMAL
#define SPC ADAM_KEYCAP_SPECIAL
#define SMK ADAM_KEYCAP_SMARTKEY
#define EDT ADAM_KEYCAP_EDIT
#define MOD ADAM_KEYCAP_MODIFIER
#define LCK ADAM_KEYCAP_LOCK
#define ARW ADAM_KEYCAP_ARROW
#define SPB ADAM_KEYCAP_SPACE

/* A character key whose shifted code is the other ASCII. */
#define CH(lbl, shf, plain, shifted, col, row) \
    KEY(lbl, shf, plain, shifted, col, row, 1, NRM)
/* A letter: the legend is the capital, the byte lower case unshifted. */
#define LET(lbl, chr, col, row) \
    KEY(lbl, NULL, (chr) + 32, (chr), col, row, 1, NRM)

static const adam_keycap k_caps[] = {
    /* row 0: ESCAPE/WP, WILD CARD, the SmartKeys, UNDO, BACK SPACE, editing */
    KEY("ESCAPE/WP", NULL, 0x1B, 0x80, 0, 0, 1.5f, SPC),
    KEY("WILD CARD", NULL, 0x90, 0x98, 1.5f, 0, 1.5f, EDT),
    KEY("I",   NULL, 0x81, 0x81, 3.5f, 0, 1.5f, SMK),
    KEY("II",  NULL, 0x82, 0x82, 5.0f, 0, 1.5f, SMK),
    KEY("III", NULL, 0x83, 0x83, 6.5f, 0, 1.5f, SMK),
    KEY("IV",  NULL, 0x84, 0x84, 8.0f, 0, 1.5f, SMK),
    KEY("V",   NULL, 0x85, 0x85, 9.5f, 0, 1.5f, SMK),
    KEY("VI",  NULL, 0x86, 0x86, 11.0f, 0, 1.5f, SMK),
    KEY("UNDO", NULL, 0x91, 0x99, 13.0f, 0, 1.5f, EDT),
    KEY("BACK SPACE", NULL, 0x08, 0x08, 14.5f, 0, 1.5f, SPC),
    KEY("MOVE/COPY", NULL, 0x92, 0x9A, 16.5f, 0, 1.25f, EDT),
    KEY("STORE/GET", NULL, 0x93, 0x9B, 17.75f, 0, 1.25f, EDT),
    KEY("CLEAR", NULL, 0x96, 0x9E, 19.0f, 0, 1.25f, EDT),

    /* row 1: the number row, then INSERT PRINT DELETE */
    CH("1", "!", '1', '!', 0, 1), CH("2", "@", '2', '@', 1, 1),
    CH("3", "#", '3', '#', 2, 1), CH("4", "$", '4', '$', 3, 1),
    CH("5", "%", '5', '%', 4, 1), CH("6", "^", '6', '^', 5, 1),
    CH("7", "&", '7', '&', 6, 1), CH("8", "*", '8', '*', 7, 1),
    CH("9", "(", '9', '(', 8, 1), CH("0", ")", '0', ')', 9, 1),
    CH("-", "_", '-', '_', 10, 1), CH("=", "+", '=', '+', 11, 1),
    CH("^", "~", '^', '~', 12, 1), CH("\\", "|", '\\', '|', 13, 1),
    KEY("INSERT", NULL, 0x94, 0x9C, 16.5f, 1, 1.25f, EDT),
    KEY("PRINT",  NULL, 0x95, 0x9D, 17.75f, 1, 1.25f, EDT),
    KEY("DELETE", NULL, 0x97, 0x9F, 19.0f, 1, 1.25f, EDT),

    /* row 2: TAB, QWERTYUIOP, brackets, RETURN (tall: rows 2-3) */
    KEY("TAB", NULL, 0x09, 0x09, 0, 2, 1.5f, SPC),
    LET("Q", 'Q', 1.5f, 2), LET("W", 'W', 2.5f, 2), LET("E", 'E', 3.5f, 2), LET("R", 'R', 4.5f, 2),
    LET("T", 'T', 5.5f, 2), LET("Y", 'Y', 6.5f, 2), LET("U", 'U', 7.5f, 2), LET("I", 'I', 8.5f, 2),
    LET("O", 'O', 9.5f, 2), LET("P", 'P', 10.5f, 2),
    CH("[", "{", '[', '{', 11.5f, 2), CH("]", "}", ']', '}', 12.5f, 2),
    TALL("RETURN", 0x0D, 0x0D, 13.6f, 2, 1.5f, 2, SPC),

    /* row 3: CONTROL, ASDFGHJKL, ; ', the cursor-up key */
    KEY("CONTROL", NULL, ADAM_MOD_CONTROL, ADAM_MOD_CONTROL, 0, 3, 1.75f, MOD),
    LET("A", 'A', 1.75f, 3), LET("S", 'S', 2.75f, 3), LET("D", 'D', 3.75f, 3), LET("F", 'F', 4.75f, 3),
    LET("G", 'G', 5.75f, 3), LET("H", 'H', 6.75f, 3), LET("J", 'J', 7.75f, 3), LET("K", 'K', 8.75f, 3),
    LET("L", 'L', 9.75f, 3),
    CH(";", ":", ';', ':', 10.75f, 3), CH("'", "\"", '\'', '"', 11.75f, 3),
    KEY("\xE2\x86\x91", NULL, 0xA0, 0xA0, 17.75f, 3, 1.25f, ARW),

    /* row 4: SHIFT, ZXCVBNM, , . /, SHIFT, left HOME right */
    KEY("SHIFT", NULL, ADAM_MOD_SHIFT, ADAM_MOD_SHIFT, 0, 4, 2.25f, MOD),
    LET("Z", 'Z', 2.25f, 4), LET("X", 'X', 3.25f, 4), LET("C", 'C', 4.25f, 4), LET("V", 'V', 5.25f, 4),
    LET("B", 'B', 6.25f, 4), LET("N", 'N', 7.25f, 4), LET("M", 'M', 8.25f, 4),
    CH(",", "<", ',', '<', 9.25f, 4), CH(".", ">", '.', '>', 10.25f, 4),
    CH("/", "?", '/', '?', 11.25f, 4),
    KEY("SHIFT", NULL, ADAM_MOD_SHIFT, ADAM_MOD_SHIFT, 12.25f, 4, 2.75f, MOD),
    KEY("\xE2\x86\x90", NULL, 0xA3, 0xA3, 16.5f, 4, 1.25f, ARW),
    KEY("HOME", NULL, 0xA8, 0xA9, 17.75f, 4, 1.25f, SPC),
    KEY("\xE2\x86\x92", NULL, 0xA1, 0xA1, 19.0f, 4, 1.25f, ARW),

    /* row 5: LOCK, the space bar, cursor-down */
    KEY("LOCK", NULL, ADAM_MOD_LOCK, ADAM_MOD_LOCK, 0, 5, 1.5f, LCK),
    KEY("SPACE", NULL, ' ', ' ', 2.0f, 5, 9.0f, SPB),
    KEY("\xE2\x86\x93", NULL, 0xA2, 0xA2, 17.75f, 5, 1.25f, ARW),
};

#define CAP_COUNT ((int)(sizeof k_caps / sizeof k_caps[0]))

/* 20.25 pitches wide (the right-hand block's last column), 6 rows high. */
#define KEYS_WIDE (20.25f * PITCH - GAP)
#define KEYS_HIGH (5 * PITCH + 1.0f)

/* A table that outgrows the session's per-cap state would be a silent bug. */
typedef char caps_fit_state[(sizeof k_caps / sizeof k_caps[0] <= ADAM_KBD_CAPS_MAX) ? 1 : -1];

const adam_keycap *adamsession_keyboard_layout(int *count, float *wide, float *high)
{
    if (count) *count = CAP_COUNT;
    if (wide) *wide = KEYS_WIDE;
    if (high) *high = KEYS_HIGH;
    return k_caps;
}

/* ---- the flash ------------------------------------------------------------ */

#define FLASH_NS 120000000LL

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- what a cap sends --------------------------------------------------------- */

/* A control code for the byte a Ctrl-held key would type: letters and the
 * @ [ \ ] ^ _ row; anything else goes out as it is. */
static unsigned control_of(unsigned code)
{
    if (code >= 'a' && code <= 'z') return code - 'a' + 1;
    if (code >= 'A' && code <= 'Z') return code - 'A' + 1;
    if (code >= '@' && code <= '_') return code & 0x1F;
    return code;
}

int keyboard_press_code(adamsession *s, int index, int down)
{
    const adam_keycap *cap;
    unsigned code;
    int result = -1;

    if (!s || index < 0 || index >= CAP_COUNT)
        return -1;
    cap = &k_caps[index];

    pthread_mutex_lock(&s->input_mtx);
    s->kbd_down[index] = (uint8_t)(down != 0);
    if (!down) {
        pthread_mutex_unlock(&s->input_mtx);
        return -1;
    }
    s->kbd_flash_ns[index] = now_ns();

    if (cap->kind == ADAM_KEYCAP_MODIFIER || cap->kind == ADAM_KEYCAP_LOCK) {
        /* a latch toggles on each press */
        if (cap->code < sizeof s->kbd_latch)
            s->kbd_latch[cap->code] ^= 1u;
        pthread_mutex_unlock(&s->input_mtx);
        return -1;
    }

    code = s->kbd_latch[ADAM_MOD_SHIFT] ? cap->shift_code : cap->code;
    if (s->kbd_latch[ADAM_MOD_LOCK] && code >= 'a' && code <= 'z')
        code = code - 'a' + 'A';
    if (s->kbd_latch[ADAM_MOD_CONTROL])
        code = control_of(code);
    /* SHIFT and CONTROL are spent by the key; LOCK stays */
    s->kbd_latch[ADAM_MOD_SHIFT] = 0;
    s->kbd_latch[ADAM_MOD_CONTROL] = 0;
    result = (int)code;
    pthread_mutex_unlock(&s->input_mtx);
    return result;
}

void adamsession_keyboard_press(adamsession *s, int index, int down)
{
    const int code = keyboard_press_code(s, index, down);
    if (code >= 0)
        adamsession_key(s, (uint8_t)code);
}

int adamsession_keyboard_held(adamsession *s, int index)
{
    int held = 0;
    if (!s || index < 0 || index >= CAP_COUNT)
        return 0;
    pthread_mutex_lock(&s->input_mtx);
    if (s->kbd_down[index] || (s->kbd_flash_ns[index]
                               && now_ns() - s->kbd_flash_ns[index] < FLASH_NS))
        held |= 1;
    if ((k_caps[index].kind == ADAM_KEYCAP_MODIFIER || k_caps[index].kind == ADAM_KEYCAP_LOCK)
        && k_caps[index].code < sizeof s->kbd_latch && s->kbd_latch[k_caps[index].code])
        held |= 2;
    pthread_mutex_unlock(&s->input_mtx);
    return held;
}

void adamsession_keyboard_release(adamsession *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->input_mtx);
    memset(s->kbd_down, 0, sizeof s->kbd_down);
    memset(s->kbd_flash_ns, 0, sizeof s->kbd_flash_ns);
    memset(s->kbd_latch, 0, sizeof s->kbd_latch);
    pthread_mutex_unlock(&s->input_mtx);
}
