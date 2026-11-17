/*
 * controls_test -- the on-screen controllers' layout, hit test and gesture
 * maths, pure: two ColecoVision hand controllers side by side (Controller 1
 * on the left), each with a caption, a stick disc ringed by eight direction
 * wedges, a fire button on either side, and the overlay-card frame holding the
 * 12-key keypad in the phone's order. No two pressable controls overlap,
 * every one of the 36 controller targets is on exactly one control, the hit
 * test agrees with the wedges and with adamsession_stick_from_vector. No
 * machine is started.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "adamsession.h"

static int failures;
static void check(int pass, const char *what)
{
    printf("%s: %s\n", pass ? "ok" : "FAIL", what);
    if (!pass) failures++;
}

#define EPS 1e-3f
#define BIT(act) (1u << (act))

static int lowest(unsigned mask)
{
    int bit = 0;
    while (mask && !(mask & 1u)) { mask >>= 1; bit++; }
    return bit;
}

static int is_round(const adam_control *ctl)
{
    return ctl->kind == ADAM_CTL_STICK || ctl->kind == ADAM_CTL_DIRECTION;
}

/* a part the pointer can press, drawn as a plain rectangle */
static int is_plain(const adam_control *ctl)
{
    return ctl->kind == ADAM_CTL_KEY || ctl->kind == ADAM_CTL_FIRE;
}

static int overlap(const adam_control *one, const adam_control *two)
{
    return one->xpos < two->xpos + two->width - EPS && two->xpos < one->xpos + one->width - EPS
        && one->ypos < two->ypos + two->height - EPS && two->ypos < one->ypos + one->height - EPS;
}

static int inside(const adam_control *inner, const adam_control *outer)
{
    return inner->xpos >= outer->xpos - EPS && inner->ypos >= outer->ypos - EPS
        && inner->xpos + inner->width <= outer->xpos + outer->width + EPS
        && inner->ypos + inner->height <= outer->ypos + outer->height + EPS;
}

static int count_kind(const adam_control *ctl, int count, int kind, int port)
{
    int num = 0, idx;
    for (idx = 0; idx < count; idx++)
        if (ctl[idx].kind == kind && ctl[idx].port == port) num++;
    return num;
}

static int find_kind(const adam_control *ctl, int count, int kind, int port)
{
    int idx;
    for (idx = 0; idx < count; idx++)
        if (ctl[idx].kind == kind && ctl[idx].port == port) return idx;
    return -1;
}

static int find_target(const adam_control *ctl, int count, int target)
{
    int idx;
    for (idx = 0; idx < count; idx++)
        if (ctl[idx].target == target && !(ctl[idx].kind == ADAM_CTL_DIRECTION && ctl[idx].target < 0))
            return idx;
    return -1;
}

static void layout(void)
{
    int count = 0, idx, other, bad = 0, port;
    float width = 0, height = 0;
    const adam_control *ctl = adamsession_controls_layout(&count, &width, &height);

    check(ctl != NULL && count == 50, "50 controls: 25 on each controller");

    /* the rectangles: round parts are judged by shape below */
    for (idx = 0; idx < count; idx++) {
        if (ctl[idx].xpos < -EPS || ctl[idx].ypos < -EPS
            || ctl[idx].xpos + ctl[idx].width > width + EPS
            || ctl[idx].ypos + ctl[idx].height > height + EPS) {
            printf("  (%s sits outside the extent)\n", ctl[idx].label);
            bad++;
        }
        if (!ctl[idx].label || !ctl[idx].label[0]) bad++;
        for (other = idx + 1; other < count; other++)
            if (is_plain(&ctl[idx]) && is_plain(&ctl[other]) && overlap(&ctl[idx], &ctl[other])) {
                printf("  (%s overlaps %s)\n", ctl[idx].label, ctl[other].label);
                bad++;
            }
    }
    check(bad == 0, "every control has a label and sits inside the extent; no two buttons overlap");
    check(fabsf(width - 10.6f) < EPS && fabsf(height - 8.75f) < EPS, "the extent is 10.6 x 8.75 units");

    /* the round parts keep clear of every button */
    bad = 0;
    for (idx = 0; idx < count; idx++) {
        if (!is_round(&ctl[idx])) continue;
        for (other = 0; other < count; other++)
            if (is_plain(&ctl[other]) && overlap(&ctl[idx], &ctl[other])) {
                printf("  (%s overlaps %s)\n", ctl[idx].label, ctl[other].label);
                bad++;
            }
    }
    check(bad == 0, "the stick and its wedges meet no button");

    for (port = 0; port < ADAM_PORTS; port++) {
        int seen[ADAM_ACT_PER_PORT] = { 0 }, diag = 0, pass = 1;
        const int stick = find_kind(ctl, count, ADAM_CTL_STICK, port);
        const int overlay = find_kind(ctl, count, ADAM_CTL_OVERLAY, port);
        char what[160];

        snprintf(what, sizeof what,
                 "controller %d: 1 caption, 1 overlay, 1 stick, 8 wedges, 2 fire buttons, 12 keys", port + 1);
        check(stick >= 0 && overlay >= 0
              && count_kind(ctl, count, ADAM_CTL_CAPTION, port) == 1
              && count_kind(ctl, count, ADAM_CTL_OVERLAY, port) == 1
              && count_kind(ctl, count, ADAM_CTL_STICK, port) == 1
              && count_kind(ctl, count, ADAM_CTL_DIRECTION, port) == 8
              && count_kind(ctl, count, ADAM_CTL_FIRE, port) == 2
              && count_kind(ctl, count, ADAM_CTL_KEY, port) == 12, what);

        for (idx = 0; idx < count; idx++) {
            const unsigned mask = ctl[idx].act_mask;
            if (ctl[idx].port != port) continue;
            if (ctl[idx].kind == ADAM_CTL_DIRECTION) {
                if (mask & (mask - 1)) {
                    diag++;
                    if (ctl[idx].target != -1) pass = 0;
                } else {
                    if (ctl[idx].target != ADAM_TARGET_PORT(port, lowest(mask))) pass = 0;
                    seen[lowest(mask)]++;
                }
                if (!inside(&ctl[idx], &ctl[stick])) pass = 0;   /* a wedge sits in its disc's box */
            } else if (ctl[idx].kind == ADAM_CTL_FIRE || ctl[idx].kind == ADAM_CTL_KEY) {
                if ((mask & (mask - 1)) || mask == 0 || ctl[idx].target != ADAM_TARGET_PORT(port, lowest(mask))) pass = 0;
                else seen[lowest(mask)]++;
                if (ctl[idx].kind == ADAM_CTL_KEY && !inside(&ctl[idx], &ctl[overlay])) pass = 0;
            } else if (ctl[idx].kind == ADAM_CTL_STICK || ctl[idx].kind == ADAM_CTL_OVERLAY
                       || ctl[idx].kind == ADAM_CTL_CAPTION) {
                if (ctl[idx].target != -1 || mask != 0) pass = 0;
            }
        }
        for (idx = 0; idx < ADAM_ACT_PER_PORT; idx++)
            if (seen[idx] != 1) pass = 0;
        snprintf(what, sizeof what,
                 "controller %d: every action is on exactly one control, the 4 diagonals on two wedges", port + 1);
        check(pass && diag == 4, what);
    }

    /* every one of the 36 controller targets is on exactly one control */
    bad = 0;
    for (idx = 0; idx < ADAM_TARGET_SYSACT(0); idx++) {
        int hits = 0;
        for (other = 0; other < count; other++)
            if (ctl[other].target == idx) hits++;
        if (hits != 1) {
            printf("  (target %d is on %d controls)\n", idx, hits);
            bad++;
        }
    }
    check(bad == 0, "all 36 controller targets have one control each");
    check(find_target(ctl, count, ADAM_TARGET_SYSACT(0)) == -1,
          "and the console actions are not in the layout: the frontends' strip carries them");

    /* the keypad: the phone's order, 3 across and 4 down, in step */
    {
        static const int order[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11 };
        int pass = 1;
        for (port = 0; port < ADAM_PORTS; port++) {
            const int first = find_target(ctl, count, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + 1));
            for (idx = 0; idx < 12; idx++) {
                const int slot = find_target(ctl, count, ADAM_TARGET_PORT(port, ADAM_ACT_KEYPAD + order[idx]));
                if (slot < 0 || first < 0 || ctl[slot].kind != ADAM_CTL_KEY) { pass = 0; continue; }
                if (fabsf(ctl[slot].xpos - (ctl[first].xpos + (float)(idx % 3) * 1.15f)) > EPS
                    || fabsf(ctl[slot].ypos - (ctl[first].ypos + (float)(idx / 3) * 0.95f)) > EPS
                    || fabsf(ctl[slot].width - 1.0f) > EPS || fabsf(ctl[slot].height - 0.8f) > EPS)
                    pass = 0;
                if (strcmp(ctl[slot].label, adamsession_keypad_label(order[idx])) != 0) pass = 0;
            }
        }
        check(pass, "keys read 1 2 3 / 4 5 6 / 7 8 9 / * 0 # on a 1.15 x 0.95 pitch, a unit wide");
    }

    /* Controller 1 on the left; the second is the first, moved over */
    {
        const int cap0 = find_kind(ctl, count, ADAM_CTL_CAPTION, 0);
        const int cap1 = find_kind(ctl, count, ADAM_CTL_CAPTION, 1);
        int pass = cap0 >= 0 && cap1 >= 0 && !strcmp(ctl[cap0].label, "Controller 1")
                   && !strcmp(ctl[cap1].label, "Controller 2") && ctl[cap0].xpos < ctl[cap1].xpos;
        const float shift = pass ? ctl[cap1].xpos - ctl[cap0].xpos : 0;
        for (idx = 0; pass && idx < 25; idx++) {
            const adam_control *one = &ctl[idx], *two = &ctl[idx + 25];
            if (one->kind != two->kind || one->port != 0 || two->port != 1
                || fabsf(two->xpos - one->xpos - shift) > EPS || fabsf(two->ypos - one->ypos) > EPS
                || fabsf(two->width - one->width) > EPS || fabsf(two->height - one->height) > EPS
                || one->act_mask != two->act_mask)
                pass = 0;
        }
        check(pass, "Controller 1 is on the left, and Controller 2 is a mirror copy of it");
    }

    /* the fire buttons flank the stick; the keypad sits below the stick */
    {
        const int stick = find_kind(ctl, count, ADAM_CTL_STICK, 0);
        const int fire_l = find_target(ctl, count, ADAM_TARGET_PORT(0, ADAM_ACT_FIRE_L));
        const int fire_r = find_target(ctl, count, ADAM_TARGET_PORT(0, ADAM_ACT_FIRE_R));
        const int overlay = find_kind(ctl, count, ADAM_CTL_OVERLAY, 0);
        check(stick >= 0 && fire_l >= 0 && fire_r >= 0 && overlay >= 0
              && ctl[fire_l].xpos + ctl[fire_l].width <= ctl[stick].xpos + EPS
              && ctl[fire_r].xpos >= ctl[stick].xpos + ctl[stick].width - EPS
              && ctl[overlay].ypos >= ctl[stick].ypos + ctl[stick].height - EPS,
              "left fire on the left of the disc, right fire on the right, the keypad below it");
    }

    /* deterministic */
    check(adamsession_controls_layout(NULL, NULL, NULL) == ctl, "the layout is built once");
}

static void hit_test(void)
{
    int count = 0, idx, pass = 1;
    float width = 0, height = 0;
    const adam_control *ctl = adamsession_controls_layout(&count, &width, &height);

    for (idx = 0; idx < count; idx++) {
        const adam_control *one = &ctl[idx];
        float posx = one->xpos + one->width / 2, posy = one->ypos + one->height / 2;
        int hit;

        if (one->kind == ADAM_CTL_CAPTION || one->kind == ADAM_CTL_OVERLAY) {
            /* drawn, never pressed: the middle of the card is a gap between keys, or a key */
            hit = adamsession_controls_hit(posx, posy);
            if (hit >= 0 && (ctl[hit].kind == ADAM_CTL_CAPTION || ctl[hit].kind == ADAM_CTL_OVERLAY)) {
                printf("  (%s was hit)\n", one->label);
                pass = 0;
            }
            continue;
        }
        if (one->kind == ADAM_CTL_DIRECTION) {
            const adam_control *disc = &ctl[find_kind(ctl, count, ADAM_CTL_STICK, one->port)];
            /* the middle of the ring, toward the wedge */
            float discx = disc->xpos + disc->width / 2, discy = disc->ypos + disc->height / 2;
            float rad = disc->width / 2 * 0.75f, dirx = 0, diry = 0;
            if (one->act_mask & BIT(ADAM_ACT_RIGHT)) dirx += 1;
            if (one->act_mask & BIT(ADAM_ACT_LEFT)) dirx -= 1;
            if (one->act_mask & BIT(ADAM_ACT_UP)) diry -= 1;      /* up on the screen */
            if (one->act_mask & BIT(ADAM_ACT_DOWN)) diry += 1;
            posx = discx + rad * dirx / sqrtf(dirx * dirx + diry * diry);
            posy = discy + rad * diry / sqrtf(dirx * dirx + diry * diry);
        }
        hit = adamsession_controls_hit(posx, posy);
        if (hit != idx) {
            printf("  (%s: hit %d, want %d)\n", one->label, hit, idx);
            pass = 0;
        }
    }
    check(pass, "the middle of every control hits it (wedges at the ring's middle)");

    /* the corners of a key: inside is the key, one hair outside is not */
    {
        const int key = find_target(ctl, count, ADAM_TARGET_PORT(0, ADAM_ACT_KEYPAD + 5));
        check(key >= 0
              && adamsession_controls_hit(ctl[key].xpos + 0.01f, ctl[key].ypos + 0.01f) == key
              && adamsession_controls_hit(ctl[key].xpos + ctl[key].width - 0.01f,
                                            ctl[key].ypos + ctl[key].height - 0.01f) == key
              && adamsession_controls_hit(ctl[key].xpos - 0.05f, ctl[key].ypos + ctl[key].height / 2) != key
              && adamsession_controls_hit(ctl[key].xpos + ctl[key].width + 0.05f, ctl[key].ypos) != key,
              "a key is hit by its rectangle, exactly");
    }

    /* the stick: the cap, the dead zone between wedge and cap, the ring, outside the ring */
    {
        const int disc = find_kind(ctl, count, ADAM_CTL_STICK, 0);
        const float discx = ctl[disc].xpos + ctl[disc].width / 2, discy = ctl[disc].ypos + ctl[disc].height / 2;
        const float rad = ctl[disc].width / 2;
        int hit;

        check(adamsession_controls_hit(discx, discy) == disc, "the cap is the stick");
        check(adamsession_controls_hit(discx + rad * 0.4f, discy) == disc, "inside the ring, still the stick");
        hit = adamsession_controls_hit(discx + rad * 0.6f, discy);
        check(hit >= 0 && ctl[hit].kind == ADAM_CTL_DIRECTION && ctl[hit].act_mask == BIT(ADAM_ACT_RIGHT),
              "in the ring, a wedge");
        check(adamsession_controls_hit(discx + rad * 1.05f, discy) != disc
              && (adamsession_controls_hit(discx + rad * 1.05f, discy) < 0
                  || ctl[adamsession_controls_hit(discx + rad * 1.05f, discy)].kind != ADAM_CTL_DIRECTION),
              "outside the ring is not the stick, nor a wedge");
        /* a point between two wedges' centres belongs to exactly one of them */
        {
            const float ang = 22.4f * 3.14159265f / 180.0f;
            hit = adamsession_controls_hit(discx + rad * 0.8f * cosf(ang), discy - rad * 0.8f * sinf(ang));
            check(hit >= 0 && ctl[hit].act_mask == BIT(ADAM_ACT_RIGHT), "just under 22.5 degrees is still RIGHT");
            hit = adamsession_controls_hit(discx + rad * 0.8f * cosf(ang + 0.01f), discy - rad * 0.8f * sinf(ang + 0.01f));
            check(hit >= 0 && ctl[hit].act_mask == (BIT(ADAM_ACT_RIGHT) | BIT(ADAM_ACT_UP)),
                  "and just over it is UP+RIGHT");
        }
    }

    /* between and outside */
    check(adamsession_controls_hit(-1.0f, -1.0f) == -1 && adamsession_controls_hit(width + 1, height + 1) == -1,
          "a point outside the layout hits nothing");
    check(adamsession_controls_hit(5.3f, 8.0f) == -1, "the gap between the two controllers hits nothing");
    {
        const int cap = find_kind(ctl, count, ADAM_CTL_CAPTION, 0);
        check(cap >= 0 && adamsession_controls_hit(ctl[cap].xpos + 1.0f, ctl[cap].ypos + 0.3f) == -1,
              "a caption is not pressable");
        check(adamsession_controls_hit(1.2f, 4.4f) == -1, "nor is the overlay card's frame");
    }
}

static void gesture(void)
{
    /* the stick's gesture maths: nothing in the middle quarter, else 8 sectors of 45 degrees */
    check(adamsession_stick_from_vector(0, 0, 10) == 0 && adamsession_stick_from_vector(2, 0, 10) == 0
          && adamsession_stick_from_vector(1.5f, 1.5f, 10) == 0, "inside a quarter of the reach: nothing");
    check(adamsession_stick_from_vector(5, 0, 10) == BIT(ADAM_ACT_RIGHT)
          && adamsession_stick_from_vector(-5, 0, 10) == BIT(ADAM_ACT_LEFT)
          && adamsession_stick_from_vector(0, -5, 10) == BIT(ADAM_ACT_UP)
          && adamsession_stick_from_vector(0, 5, 10) == BIT(ADAM_ACT_DOWN),
          "right, left, up (up on the screen) and down (toward the player)");
    check(adamsession_stick_from_vector(5, -5, 10) == (BIT(ADAM_ACT_RIGHT) | BIT(ADAM_ACT_UP))
          && adamsession_stick_from_vector(-5, -5, 10) == (BIT(ADAM_ACT_LEFT) | BIT(ADAM_ACT_UP))
          && adamsession_stick_from_vector(5, 5, 10) == (BIT(ADAM_ACT_RIGHT) | BIT(ADAM_ACT_DOWN))
          && adamsession_stick_from_vector(-5, 5, 10) == (BIT(ADAM_ACT_LEFT) | BIT(ADAM_ACT_DOWN)),
          "the diagonals are two actions");
    /* 22.5 degrees is the boundary between right and the diagonal */
    check(adamsession_stick_from_vector(9.3f, -3.0f, 10) == BIT(ADAM_ACT_RIGHT)       /* 17.9 degrees */
          && adamsession_stick_from_vector(9.0f, -4.3f, 10) == (BIT(ADAM_ACT_RIGHT) | BIT(ADAM_ACT_UP)), /* 25.5 */
          "the sectors are 45 degrees wide, centred on the directions");
    check(adamsession_stick_from_vector(100, 0, 10) == BIT(ADAM_ACT_RIGHT)
          && adamsession_stick_from_vector(5, 0, 0) == 0,
          "past the reach still counts; no reach is nothing");

    /* the hit test agrees with the gesture maths: the wedge at an angle is the one
     * adamsession_stick_from_vector names for it */
    {
        int count = 0, port, idx, step;
        const adam_control *ctl = adamsession_controls_layout(&count, NULL, NULL);

        for (port = 0; port < ADAM_PORTS; port++) {
            int pass = 1;
            const adam_control *disc = NULL;

            for (idx = 0; idx < count; idx++)
                if (ctl[idx].kind == ADAM_CTL_STICK && ctl[idx].port == port) disc = &ctl[idx];
            for (step = 0; step < 8 && disc; step++) {
                const float ang = (float)step * 3.14159265f / 4.0f, vecx = cosf(ang), vecy = -sinf(ang);
                const unsigned want = adamsession_stick_from_vector(vecx, vecy, 1.0f);
                const float rad = disc->width / 2 * 0.75f;
                const int hit = adamsession_controls_hit(disc->xpos + disc->width / 2 + rad * vecx,
                                                           disc->ypos + disc->height / 2 + rad * vecy);

                if (hit < 0 || ctl[hit].kind != ADAM_CTL_DIRECTION || ctl[hit].port != port
                    || ctl[hit].act_mask != want || !want)
                    pass = 0;
            }
            check(disc && pass, port ? "controller 2's wedges are the stick's eight sectors"
                                     : "controller 1's wedges are the stick's eight sectors");
        }
    }
}

static void keycaps(void)
{
    static const char *const caps[12] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#" };
    int idx, pass = 1;

    for (idx = 0; idx < 12; idx++)
        if (!adamsession_keypad_label(ADAM_ACT_KEYPAD + idx)
            || strcmp(adamsession_keypad_label(ADAM_ACT_KEYPAD + idx), caps[idx]) != 0)
            pass = 0;
    check(pass, "the keycap of keypad action n is 0-9, * or #");
    check(adamsession_keypad_label(-1) == NULL && adamsession_keypad_label(ADAM_ACT_UP) == NULL
          && adamsession_keypad_label(99) == NULL, "and the other actions have none");
}

int main(void)
{
    layout();
    hit_test();
    gesture();
    keycaps();

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
