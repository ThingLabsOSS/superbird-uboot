/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Board-local rotary-encoder API. cmd_wheel.c implements; cmd_bootmenu.c
 * consumes. Not exported to global include/ since this is genuinely
 * single-board internal glue.
 */

#ifndef _SPOTIFY_CARTHING_WHEEL_H
#define _SPOTIFY_CARTHING_WHEEL_H

/*
 * Read the wheel pins once and return signed *detents* since the last
 * call (a full physical click = +/-1). Bounce on a single detent edge
 * is filtered out — the function returns 0 until two half-steps in the
 * same direction have accumulated.
 *
 * Returns 0 if the wheel hasn't been wired up (lookups failed) or if
 * no full detent has crossed since the last call.
 */
int wheel_poll_detents(void);

#endif
