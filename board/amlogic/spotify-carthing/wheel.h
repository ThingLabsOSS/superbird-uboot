/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Board-local rotary-encoder API (cmd_wheel.c -> cmd_bootmenu.c). Deliberately
 * not in global include/ — single-board internal glue.
 */

#ifndef _SPOTIFY_CARTHING_WHEEL_H
#define _SPOTIFY_CARTHING_WHEEL_H

/*
 * Signed detents since the last call; one physical click = +/-1. Returns 0
 * until two half-steps accumulate in the same direction, which filters
 * single-edge bounce, and also if the wheel isn't wired up.
 */
int wheel_poll_detents(void);

#endif
