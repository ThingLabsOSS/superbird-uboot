// SPDX-License-Identifier: GPL-2.0+
/*
 * Drawing + input primitives shared by the on-panel screens.
 *
 * Everything renders via the vidconsole position_cursor API and plain markers
 * rather than ANSI escapes: neither cursor addressing nor reverse-video
 * survives the rotated console driver.
 */

#include <button.h>
#include <dm.h>
#include <dm/uclass.h>
#include <linux/string.h>
#include <video.h>
#include <video_console.h>

#include "menu_ui.h"

/* Latched by menu_ui_begin(); NULL means "no panel", and drawing no-ops. */
static struct udevice *vc;

void menu_ui_begin(void)
{
	struct udevice *vdev;

	/* Costs the ~700 ms ST7701S init if misc_init_r skipped it under
	 * quick_boot=1; a cheap re-probe otherwise. */
	(void)uclass_first_device_err(UCLASS_VIDEO, &vdev);

	if (uclass_first_device_err(UCLASS_VIDEO_CONSOLE, &vc))
		vc = NULL;
}

/*
 * Write straight to the vidconsole, bypassing the stdout multiplexer — so
 * drawing never depends on env stdout containing "vidconsole", and general
 * u-boot output (command echo, busy spinners) never bleeds onto the panel.
 */
void vc_puts(const char *s)
{
	if (vc)
		vidconsole_put_string(vc, s);
}

void vc_at(int row, int col)
{
	if (vc)
		vidconsole_position_cursor(vc, col, row);
}

void vc_clear(void)
{
	if (vc)
		vidconsole_clear_and_reset(vc);
}

void vc_sync(void)
{
	if (vc)
		video_sync(dev_get_parent(vc), false);
}

int center_col(int width)
{
	int c = (VC_COLS - width) / 2;

	return c < 0 ? 0 : c;
}

void draw_mode(const char *title)
{
	const char *exit_hint = "Press to exit ->";

	if (!vc)
		return;

	vc_clear();

	vc_at(VC_ROWS / 2 - 1, center_col(strlen(title)));
	vc_puts(title);

	vc_at(VC_ROWS - 3, VC_COLS - strlen(exit_hint));
	vc_puts(exit_hint);

	vc_sync();
}

int btn_init(struct btn_edge *b, const char *label)
{
	int ret;

	b->label = label;
	b->prev = 0;
	ret = button_get_by_label(label, &b->dev);
	if (ret) {
		printf("bootmenu: button '%s' not found (err=%d)\n", label, ret);
		return ret;
	}
	return 0;
}

int btn_edge(struct btn_edge *b)
{
	int now = (button_get_state(b->dev) == BUTTON_ON);
	int edge = now && !b->prev;

	b->prev = now;
	return edge;
}

int menu_btns_init(struct menu_btns *b)
{
	return btn_init(&b->up,   "preset1") ||
	       btn_init(&b->dn,   "preset4") ||
	       btn_init(&b->sel,  "select")  ||
	       btn_init(&b->back, "back");
}

void menu_btns_drain(struct menu_btns *b)
{
	(void)btn_edge(&b->up);
	(void)btn_edge(&b->dn);
	(void)btn_edge(&b->sel);
	(void)btn_edge(&b->back);
}
