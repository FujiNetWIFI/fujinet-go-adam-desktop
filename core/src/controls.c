/*
 * controls -- the on-screen controllers: one layout for every toolkit, its hit
 * test, the stick's gesture maths and the keycap every keypad key has.
 *
 * The ColecoVision has two hand controllers, side by side here (Controller 1
 * on the left). Each is a black case with a short stick on a disc at the top,
 * a fire button down either side, and below it the 12-key phone keypad in the
 * frame of the overlay card slot:
 *
 *   the STICK  a disc: UP (away from the player) at the top, RIGHT on the
 *              right. The cap is the inner half of the disc; round it, eight
 *              DIRECTION wedges.
 *   FIRE       the left and the right side button, tall and narrow.
 *   OVERLAY    the card frame behind the keypad (drawn, never pressed).
 *   KEY        1 2 3 / 4 5 6 / 7 8 9 / * 0 #, the phone's order.
 *
 * Units: a keypad key's width is 1.0. The extents are the controls' bounding
 * box; a toolkit adds its own margin. Wedges are annular sectors, so their
 * rectangles are only bounding boxes: a toolkit paints a wedge as the sector
 * of the STICK disc whose direction is its act_mask, between half the disc's
 * radius and all of it, and uses adamsession_controls_hit (exact shapes)
 * for hit testing. No colours here: the same layout serves the ADAM app, whose
 * controllers are the same ones in cream.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stddef.h>

#include "adamsession.h"

#define PI_F 3.14159265358979f

/* ---- geometry (layout units) ------------------------------------------------ */

#define KEY_W     1.0f
#define KEY_H     0.8f
#define KEY_GAP   0.15f
#define KEY_PITCH_X (KEY_W + KEY_GAP)
#define KEY_PITCH_Y (KEY_H + KEY_GAP)
#define BLOCK_W   5.0f
#define BLOCK_GAP 0.6f
#define CAPTION_H 0.6f
#define DISC_D    3.0f
#define DISC_R    (DISC_D / 2)
#define DISC_CX   2.5f
#define DISC_CY   2.5f
#define FIRE_W    0.7f
#define FIRE_H    1.8f
#define FIRE_Y    1.6f
#define CARD_X    0.55f
#define CARD_Y    4.25f
#define CARD_W    3.9f
#define CARD_H    4.3f
#define KEYS_X    0.85f
#define KEYS_Y    4.6f
#define MARGIN_Y  0.2f

#define PER_CTL   25                           /* parts per controller */
#define TOTAL     (ADAM_PORTS * PER_CTL)

static adam_control g_layout[TOTAL];
/* the exact shape of the round parts, parallel to g_layout */
typedef struct {
    float centx, centy;          /* the disc's centre */
    float rinner, router;          /* the ring (wedges); a STICK is a disc of router */
    int sector;            /* wedges: 0..7, 0 = RIGHT, counter-clockwise on screen */
} shape;
static shape g_shape[TOTAL];
static float g_w, g_h;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* The eight sectors in order round the disc from RIGHT, counter-clockwise as
 * drawn (so UP, at the top, is 2). */
static const unsigned k_sector_acts[8] = {
    1u << ADAM_ACT_RIGHT,
    (1u << ADAM_ACT_UP) | (1u << ADAM_ACT_RIGHT),
    1u << ADAM_ACT_UP,
    (1u << ADAM_ACT_UP) | (1u << ADAM_ACT_LEFT),
    1u << ADAM_ACT_LEFT,
    (1u << ADAM_ACT_DOWN) | (1u << ADAM_ACT_LEFT),
    1u << ADAM_ACT_DOWN,
    (1u << ADAM_ACT_DOWN) | (1u << ADAM_ACT_RIGHT),
};
static const char *const k_sector_names[8] = {
    "Right", "Up right", "Up", "Up left", "Left", "Down left", "Down", "Down right",
};

/* The keycap table: the keypad's keys in the phone's order, row by row. */
typedef struct {
    int act;               /* ADAM_ACT_KEYPAD + the key's number */
    const char *label;
} keycap;
static const keycap k_keycaps[ADAM_KEYPAD_KEYS] = {
    { ADAM_ACT_KEYPAD + 1, "1" }, { ADAM_ACT_KEYPAD + 2, "2" }, { ADAM_ACT_KEYPAD + 3, "3" },
    { ADAM_ACT_KEYPAD + 4, "4" }, { ADAM_ACT_KEYPAD + 5, "5" }, { ADAM_ACT_KEYPAD + 6, "6" },
    { ADAM_ACT_KEYPAD + 7, "7" }, { ADAM_ACT_KEYPAD + 8, "8" }, { ADAM_ACT_KEYPAD + 9, "9" },
    { ADAM_ACT_KEYPAD + 10, "*" }, { ADAM_ACT_KEYPAD + 0, "0" }, { ADAM_ACT_KEYPAD + 11, "#" },
};

static int lowest_bit(unsigned mask)
{
    int bit = 0;
    while (mask && !(mask & 1u)) { mask >>= 1; bit++; }
    return bit;
}

static void add(int *count, int kind, const char *label, int port, int target,
                unsigned acts, float xpos, float ypos, float width, float height)
{
    adam_control *ctl = &g_layout[*count];
    ctl->kind = kind;
    ctl->label = label;
    ctl->legend = NULL;
    ctl->port = port;
    ctl->target = target;
    ctl->act_mask = acts;
    ctl->xpos = xpos; ctl->ypos = ypos; ctl->width = width; ctl->height = height;
    g_shape[*count].sector = -1;
    (*count)++;
}

/* The bounding box of the ring sector `sector` round (centx, centy). */
static void sector_box(float centx, float centy, float rad0, float rad1, int sector,
                       float *xpos, float *ypos, float *width, float *height)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    const float start = ((float)sector * 45.0f - 22.5f) * PI_F / 180.0f;
    int step, edge;

    for (step = 0; step <= 8; step++) {
        const float ang = start + (float)step * (45.0f * PI_F / 180.0f) / 8.0f;
        for (edge = 0; edge < 2; edge++) {
            const float rad = edge ? rad1 : rad0;
            const float posx = centx + rad * cosf(ang), posy = centy - rad * sinf(ang);
            if (posx < minx) minx = posx;
            if (posx > maxx) maxx = posx;
            if (posy < miny) miny = posy;
            if (posy > maxy) maxy = posy;
        }
    }
    *xpos = minx; *ypos = miny; *width = maxx - minx; *height = maxy - miny;
}

static void controller(int *count, int port, float xorg, const char *caption)
{
    const float centx = xorg + DISC_CX, centy = DISC_CY;
    const int base = ADAM_TARGET_PORT(port, 0);
    int sector, idx;

    add(count, ADAM_CTL_CAPTION, caption, port, -1, 0, xorg, 0.0f, BLOCK_W, CAPTION_H);
    add(count, ADAM_CTL_OVERLAY, "Overlay", port, -1, 0,
        xorg + CARD_X, CARD_Y, CARD_W, CARD_H);

    add(count, ADAM_CTL_STICK, "Stick", port, -1, 0,
        centx - DISC_R, centy - DISC_R, DISC_D, DISC_D);
    g_shape[*count - 1] = (shape){ centx, centy, 0.0f, DISC_R, -1 };

    for (sector = 0; sector < 8; sector++) {
        const unsigned acts = k_sector_acts[sector];
        float xpos, ypos, width, height;
        sector_box(centx, centy, DISC_R / 2, DISC_R, sector, &xpos, &ypos, &width, &height);
        add(count, ADAM_CTL_DIRECTION, k_sector_names[sector], port,
            /* a diagonal drives two actions: no single target */
            (acts & (acts - 1)) ? -1 : base + lowest_bit(acts), acts,
            xpos, ypos, width, height);
        g_shape[*count - 1] = (shape){ centx, centy, DISC_R / 2, DISC_R, sector };
    }

    add(count, ADAM_CTL_FIRE, "Left Fire", port, base + ADAM_ACT_FIRE_L,
        1u << ADAM_ACT_FIRE_L, xorg, FIRE_Y, FIRE_W, FIRE_H);
    add(count, ADAM_CTL_FIRE, "Right Fire", port, base + ADAM_ACT_FIRE_R,
        1u << ADAM_ACT_FIRE_R, xorg + BLOCK_W - FIRE_W, FIRE_Y, FIRE_W, FIRE_H);

    for (idx = 0; idx < ADAM_KEYPAD_KEYS; idx++)
        add(count, ADAM_CTL_KEY, k_keycaps[idx].label, port, base + k_keycaps[idx].act,
            1u << k_keycaps[idx].act,
            xorg + KEYS_X + (float)(idx % 3) * KEY_PITCH_X,
            KEYS_Y + (float)(idx / 3) * KEY_PITCH_Y, KEY_W, KEY_H);
}

static void build(void)
{
    int count = 0;

    controller(&count, 0, 0.0f, "Controller 1");
    controller(&count, 1, BLOCK_W + BLOCK_GAP, "Controller 2");

    g_w = ADAM_PORTS * BLOCK_W + BLOCK_GAP;
    g_h = CARD_Y + CARD_H + MARGIN_Y;
}

static void ensure_built(void)
{
    pthread_once(&g_once, build);
}

const adam_control *adamsession_controls_layout(int *count, float *width,
                                                    float *height)
{
    ensure_built();
    if (count) *count = TOTAL;
    if (width) *width = g_w;
    if (height) *height = g_h;
    return g_layout;
}

/* The sector (0 = RIGHT, counter-clockwise on screen) a vector lies in. */
static int sector_of(float delx, float dely)
{
    float deg = atan2f(-dely, delx) * 180.0f / PI_F;      /* +y is toward the player */
    int sector;

    if (deg < 0) deg += 360.0f;
    sector = (int)floorf((deg + 22.5f) / 45.0f);
    return sector & 7;
}

int adamsession_controls_hit(float xpos, float ypos)
{
    int idx;

    ensure_built();
    for (idx = TOTAL - 1; idx >= 0; idx--) {
        const adam_control *ctl = &g_layout[idx];
        const shape *shp = &g_shape[idx];

        if (ctl->kind == ADAM_CTL_CAPTION || ctl->kind == ADAM_CTL_OVERLAY)
            continue;
        if (ctl->kind == ADAM_CTL_STICK || ctl->kind == ADAM_CTL_DIRECTION) {
            const float delx = xpos - shp->centx, dely = ypos - shp->centy;
            const float dist = sqrtf(delx * delx + dely * dely);

            if (dist > shp->router || dist < shp->rinner)
                continue;
            if (ctl->kind == ADAM_CTL_DIRECTION && sector_of(delx, dely) != shp->sector)
                continue;
            return idx;
        }
        if (xpos >= ctl->xpos && xpos < ctl->xpos + ctl->width
            && ypos >= ctl->ypos && ypos < ctl->ypos + ctl->height)
            return idx;
    }
    return -1;
}

unsigned adamsession_stick_from_vector(float delx, float dely, float radius)
{
    if (radius <= 0.0f)
        return 0;
    if (sqrtf(delx * delx + dely * dely) < radius * 0.25f)
        return 0;
    return k_sector_acts[sector_of(delx, dely)];
}

const char *adamsession_keypad_label(int act)
{
    int idx;
    for (idx = 0; idx < ADAM_KEYPAD_KEYS; idx++)
        if (k_keycaps[idx].act == act)
            return k_keycaps[idx].label;
    return NULL;
}
