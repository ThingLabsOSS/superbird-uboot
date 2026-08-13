// SPDX-License-Identifier: GPL-2.0+
/*
 * Charger info screen — decoded MAX14656 BC1.2 detector state, with a
 * knob-press manual redetect.
 */

#include <console.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <vsprintf.h>

#include "charger.h"
#include "menu_charger.h"
#include "menu_ui.h"

static void draw_charger_info(void)
{
	const char *exit_hint = "Press to exit ->";
	const char *knob_hint = "Knob press: force redetect";
	const char *title = "CHARGER";
	struct carthing_charger_info info;
	char buf[64];
	int row = 2;

	vc_clear();

	vc_at(row++, center_col(strlen(title)));
	vc_puts(title);
	row++;

	if (carthing_charger_read(&info) || !info.valid) {
		const char *err = "I2C read failed";

		vc_at(row++, center_col(strlen(err)));
		vc_puts(err);
	} else {
		const char *type = carthing_charger_type_str(info.status);

		vc_at(row++, center_col(strlen(type)));
		vc_puts(type);
		row++;

		snprintf(buf, sizeof(buf), "MAX14656 rev %d  status=0x%02x",
			 info.regs[0x00] & 0xf, info.status);
		vc_at(row++, center_col(strlen(buf)));
		vc_puts(buf);

		snprintf(buf, sizeof(buf), "VB=%d  OVP=%d  detecting=%d",
			 (info.status >> 4) & 1,
			 (info.status >> 5) & 1,
			 (info.status >> 6) & 1);
		vc_at(row++, center_col(strlen(buf)));
		vc_puts(buf);
	}

	vc_at(VC_ROWS - 5, center_col(strlen(knob_hint)));
	vc_puts(knob_hint);

	vc_at(VC_ROWS - 3, VC_COLS - strlen(exit_hint));
	vc_puts(exit_hint);

	vc_sync();
}

void menu_charger_run(struct menu_btns *b)
{
	draw_charger_info();
	menu_btns_drain(b);

	while (1) {
		mdelay(5);

		if (btn_edge(&b->back) || ctrlc())
			return;

		if (btn_edge(&b->sel)) {
			/* Writes CHG_TYP_MAN, waits for the chip to re-run
			 * BC1.2, then repaints. */
			(void)carthing_charger_redetect();
			mdelay(CARTHING_CHARGER_REDETECT_DELAY_MS);
			draw_charger_info();
		}
	}
}
