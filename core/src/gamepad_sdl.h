/*
 * gamepad_sdl -- private interface: the pure helpers unit-tested without
 * hardware, and the start/stop the session calls.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAM_GAMEPAD_SDL_H
#define ADAM_GAMEPAD_SDL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Direction bits a stick or D-pad resolves to. */
#define ADAM_DIR_UP    1
#define ADAM_DIR_DOWN  2
#define ADAM_DIR_LEFT  4
#define ADAM_DIR_RIGHT 8

/* An analog stick engages a direction at half deflection and lets go below
 * 0.35: wide enough that a thumb resting near the threshold cannot flutter.
 * The hand controller's stick is four switches, so these two numbers are the
 * whole of its feel. */
#define ADAM_STICK_ENTER 0.5f
#define ADAM_STICK_EXIT  0.35f

/* The left stick's (or the D-pad's) direction bits as the controller's
 * direction actions, a mask over adam_action. Opposite directions cancel,
 * so a stick pushed both ways holds neither. Pure. */
unsigned adam_acts_from_dir(int dir);

/* An analog stick as an eight-way switch with hysteresis: an axis engages
 * past `enter` and releases below `leave` (both 0..1 of full deflection), so
 * a thumb resting near the threshold cannot flutter. xval and yval are SDL's
 * (+x right, +y down); `prev` is the previous result. Pure. */
int adam_dir_from_stick(float xval, float yval, int prev, float enter, float leave);

/* Which pad drives `port`: an explicit assignment wins; among the rest,
 * connection order (first pad to port 0, second to port 1). `assign[idx]` is
 * pad idx's assignment (-1 = automatic). Returns the pad index or -1. Pure. */
int adam_pad_for_port(const int *assign, int npads, int port);

/* SDL3's SDL_GamepadButton (a uint8_t) to the public numbering, and back.
 * Explicit tables, never a cast, so an SDL reordering cannot desync them. */
int adam_pad_button_from_sdl(uint8_t button);
int adam_sdl_button_from_pad(int button);

#ifdef __cplusplus
}
#endif

#endif /* ADAM_GAMEPAD_SDL_H */
