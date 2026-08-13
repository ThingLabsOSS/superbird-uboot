/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Shared drawing + input primitives for the on-panel screens
 * (cmd_bootmenu.c and the menu_*.c sub-screens).
 */

#ifndef _SPOTIFY_CARTHING_MENU_UI_H
#define _SPOTIFY_CARTHING_MENU_UI_H

struct udevice;

/* Logical (post-rotation) panel size with the 16x32 font:
 * 800/16 = 50 cols, 480/32 = 15 rows. */
#define VC_COLS	50
#define VC_ROWS	15

/*
 * Bring up the panel and latch the vidconsole device. Idempotent, and safe to
 * call when misc_init_r already probed video — but NOT optional, since
 * quick_boot=1 leaves the panel uninitialised and no screen can draw without
 * it. Drawing degrades to a no-op if the vidconsole is missing.
 */
void menu_ui_begin(void);

void vc_puts(const char *s);
void vc_at(int row, int col);
void vc_clear(void);

/*
 * Push the framebuffer. Needed before any wait loop: vidconsole otherwise
 * flushes only on newlines or via the cyclic idle worker, and the poll loops
 * we enter never yield long enough for the cyclic to fire.
 */
void vc_sync(void);

/* Leftmost column that centres `width` chars. Never negative. */
int center_col(int width);

/*
 * Centred title plus a right-aligned "Press to exit ->" hint, positioned to
 * line up with the physical back button in landscape. Used for modes that
 * otherwise sit blocked on the USB gadget with a blank screen.
 */
void draw_mode(const char *title);

/* Edge-triggered button, so a held button fires once. */
struct btn_edge {
	const char *label;
	struct udevice *dev;
	int prev;
};

int btn_init(struct btn_edge *b, const char *label);
int btn_edge(struct btn_edge *b);

/* The four buttons every screen navigates with. */
struct menu_btns {
	struct btn_edge up;	/* preset1 */
	struct btn_edge dn;	/* preset4 */
	struct btn_edge sel;	/* wheel press */
	struct btn_edge back;
};

/* Returns 0 on success, non-zero if any button is missing from the DT. */
int menu_btns_init(struct menu_btns *b);

/* Discard held state, so a press that entered a screen can't immediately act
 * inside it. Call on entry to and exit from every sub-screen. */
void menu_btns_drain(struct menu_btns *b);

#endif /* _SPOTIFY_CARTHING_MENU_UI_H */
