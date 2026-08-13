// SPDX-License-Identifier: GPL-2.0+
/*
 * Settings sub-menu: brightness and boot-time display init.
 *
 * "Init display at boot" is stored in env `quick_boot` — legacy name, kept so
 * existing setups don't break, and its polarity is INVERTED (display-init=Yes
 * means quick_boot unset or 0). Tradeoff: superbird-docs/uboot/boot-flow.md.
 */

#include <command.h>
#include <console.h>
#include <env.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <vsprintf.h>

#include "menu_settings.h"
#include "menu_ui.h"
#include "wheel.h"

/* Short names double as `brightness` env values, so the cycle position
 * round-trips across reboots. */
static const char * const brightness_names[] = { "Low", "Medium", "High" };
static const char * const brightness_short[] = { "low", "med", "high" };
static const char * const brightness_cmds[]  = {
	"setbright low", "setbright med", "setbright high",
};
#define NBRIGHTNESS	((int)(sizeof(brightness_names) / sizeof(brightness_names[0])))

/* Persists across menu invocations so the cycle resumes where it left off. */
static int brightness_idx = 1;	/* Medium */

void menu_settings_sync_brightness(void)
{
	const char *v = env_get("brightness");
	int i;

	brightness_idx = 1;
	if (!v)
		return;
	for (i = 0; i < NBRIGHTNESS; i++) {
		if (!strcmp(v, brightness_short[i])) {
			brightness_idx = i;
			return;
		}
	}
}

enum settings_item {
	SET_BRIGHTNESS,
	SET_DISPLAY_INIT,
};
#define NSETTINGS 2

static bool display_init_enabled(void)
{
	return env_get_yesno("quick_boot") != 1;
}

static void draw_settings(int sel)
{
	const char *title = "SETTINGS";
	const char *sep   = "----------------";
	const char *hint  = "knob to nav  press=change  back=exit";
	char buf[VC_COLS + 1];

	vc_clear();

	vc_at(2, center_col(strlen(title)));
	vc_puts(title);
	vc_at(3, center_col(strlen(sep)));
	vc_puts(sep);

	snprintf(buf, sizeof(buf), "Brightness: %s",
		 brightness_names[brightness_idx]);
	vc_at(6, center_col(strlen(buf) + 4));
	vc_puts(sel == SET_BRIGHTNESS ? "> " : "  ");
	vc_puts(buf);

	snprintf(buf, sizeof(buf), "Init display at boot: %s",
		 display_init_enabled() ? "Yes" : "No");
	vc_at(7, center_col(strlen(buf) + 4));
	vc_puts(sel == SET_DISPLAY_INIT ? "> " : "  ");
	vc_puts(buf);

	/* Spell out the "No" consequence — it is not guessable from the label. */
	{
		const char *help1 = "\"No\" boots ~700 ms faster but the OS";
		const char *help2 = "that follows must init the display.";

		vc_at(10, center_col(strlen(help1)));
		vc_puts(help1);
		vc_at(11, center_col(strlen(help2)));
		vc_puts(help2);
	}

	vc_at(VC_ROWS - 1, center_col(strlen(hint)));
	vc_puts(hint);

	vc_sync();
}

static void cycle_brightness(void)
{
	brightness_idx = (brightness_idx + 1) % NBRIGHTNESS;
	run_command(brightness_cmds[brightness_idx], 0);
	env_set("brightness", brightness_short[brightness_idx]);
	env_save();
}

static void toggle_display_init(void)
{
	env_set("quick_boot", display_init_enabled() ? "1" : "0");
	env_save();
}

void menu_settings_run(struct menu_btns *b)
{
	int sel = 0;

	menu_btns_drain(b);
	draw_settings(sel);

	while (1) {
		int dt;

		mdelay(5);

		dt = wheel_poll_detents();
		if (dt != 0) {
			sel = ((sel + dt) % NSETTINGS + NSETTINGS) % NSETTINGS;
			draw_settings(sel);
			continue;
		}
		if (btn_edge(&b->up)) {
			sel = (sel - 1 + NSETTINGS) % NSETTINGS;
			draw_settings(sel);
			continue;
		}
		if (btn_edge(&b->dn)) {
			sel = (sel + 1) % NSETTINGS;
			draw_settings(sel);
			continue;
		}
		if (btn_edge(&b->sel)) {
			if (sel == SET_BRIGHTNESS)
				cycle_brightness();
			else
				toggle_display_init();
			draw_settings(sel);
			continue;
		}
		if (btn_edge(&b->back) || ctrlc())
			return;
	}
}
