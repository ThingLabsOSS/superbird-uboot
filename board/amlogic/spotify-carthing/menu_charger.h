/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _SPOTIFY_CARTHING_MENU_CHARGER_H
#define _SPOTIFY_CARTHING_MENU_CHARGER_H

struct menu_btns;

/* Owns its input loop; returns when back is pressed. */
void menu_charger_run(struct menu_btns *b);

#endif /* _SPOTIFY_CARTHING_MENU_CHARGER_H */
