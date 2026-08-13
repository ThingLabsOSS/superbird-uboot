/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _SPOTIFY_CARTHING_MENU_HWINFO_H
#define _SPOTIFY_CARTHING_MENU_HWINFO_H

struct menu_btns;

/*
 * Six-page hardware inventory. Owns its input loop; returns when back is
 * pressed (or, from inside the cert-hex modal, when back leaves the modal
 * first). Rotate cycles pages, press refreshes the current page.
 */
void menu_hwinfo_run(struct menu_btns *b);

#endif /* _SPOTIFY_CARTHING_MENU_HWINFO_H */
