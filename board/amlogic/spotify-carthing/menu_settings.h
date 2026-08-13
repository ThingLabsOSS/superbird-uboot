/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _SPOTIFY_CARTHING_MENU_SETTINGS_H
#define _SPOTIFY_CARTHING_MENU_SETTINGS_H

struct menu_btns;

/* Re-sync the brightness cycle position from the `brightness` env var. */
void menu_settings_sync_brightness(void);

/* Owns its input loop; returns when back is pressed. */
void menu_settings_run(struct menu_btns *b);

#endif /* _SPOTIFY_CARTHING_MENU_SETTINGS_H */
