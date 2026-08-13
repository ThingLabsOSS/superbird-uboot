// SPDX-License-Identifier: GPL-2.0+
/*
 * On-panel boot menu. Navigate with preset1/preset4 or the wheel, select with
 * the wheel press, cancel with back.
 *
 * To add an item, append to items[]. K_CMD items just run action_cmd; the
 * other kinds dispatch to a sub-screen in menu_*.c, each of which owns its own
 * input loop and returns here when the user backs out.
 */

#include <command.h>
#include <console.h>
#include <env.h>
#include <linux/delay.h>
#include <linux/string.h>

#include "menu_charger.h"
#include "menu_hwinfo.h"
#include "menu_settings.h"
#include "menu_ui.h"
#include "wheel.h"

enum item_kind {
	K_CMD,		/* run action_cmd, optionally reset_after */
	K_SETTINGS,
	K_CHARGER,
	K_HWINFO,
};

struct menu_item {
	enum item_kind kind;
	const char *label;
	const char *action_cmd;
	/* Painted centred while action_cmd runs, so the user can see which
	 * mode the device is in — these commands otherwise sit blocked on the
	 * USB gadget with a blank screen. */
	const char *panel_title;
	/* u-boot's gadget stack doesn't fully tear down between sessions, so a
	 * second ums/fastboot wedges the controller. Resetting is the pragmatic
	 * fix until the gadget shutdown path is cleaned up upstream. */
	int reset_after;
};

static const struct menu_item items[] = {
	{ K_CMD,        "Fastboot",         "fastboot 0",  "FASTBOOT",         1 },
	{ K_CMD,        "Target Disk Mode", "ums 0 mmc 0", "TARGET DISK MODE", 1 },
	{ K_SETTINGS,   "Settings",         NULL,          NULL,               0 },
	{ K_CHARGER,    "Charger Info",     NULL,          NULL,               0 },
	{ K_HWINFO,     "Hardware Details", NULL,          NULL,               0 },
};

#define NITEMS	((int)(sizeof(items) / sizeof(items[0])))

/*
 * Column for a centred menu label. The "> " cursor occupies the two columns to
 * its left, and is not counted — it must not pull the label off centre.
 *
 * An odd-length label can't sit exactly centred on a 50-column screen (50 - 13
 * is odd), so the spare column has to fall on one side. Bias it to the LEFT
 * margin, i.e. round the start column up: "Hardware Info" then begins on the
 * same column as "Charger Info" above it rather than hanging one column
 * further left, which is what reads as misaligned.
 */
static int item_label_col(const char *label)
{
	int col = (VC_COLS - (int)strlen(label) + 1) / 2;

	/* Keep room for the cursor even if a label ever gets very long. */
	return col < 2 ? 2 : col;
}

static void draw(int sel)
{
	const char *title = "BOOT MENU";
	const char *sep   = "----------------";
	const char *hint  = "1/4 or knob to nav  knob-press=enter  back=exit";
	int i;

	vc_clear();

	vc_at(2, center_col(strlen(title)));
	vc_puts(title);
	vc_at(3, center_col(strlen(sep)));
	vc_puts(sep);

	for (i = 0; i < NITEMS; i++) {
		int col = item_label_col(items[i].label);

		vc_at(5 + i, col - 2);
		vc_puts((i == sel) ? "> " : "  ");
		vc_puts(items[i].label);
	}

	vc_at(VC_ROWS - 1, center_col(strlen(hint)));
	vc_puts(hint);
}

/*
 * One-shot "mask-ROM USB attempt failed" screen — buttons 1+4 were held but
 * the SoC fell through to eMMC, usually a charge-only USB cable.
 */
static void draw_maskrom_failed_screen(void)
{
	const char *title = "MASK-ROM USB RECOVERY FAILED";
	const char *sep = "============================";

	vc_clear();

	vc_at(1, center_col(strlen(title)));
	vc_puts(title);
	vc_at(2, center_col(strlen(sep)));
	vc_puts(sep);

	vc_at(4, 1);
	vc_puts("You tried to enter recovery mode by holding");
	vc_at(5, 1);
	vc_puts("buttons 1 and 4 at reset, but the SoC could");
	vc_at(6, 1);
	vc_puts("not enumerate over USB.");

	vc_at(8, 1);
	vc_puts("Common causes: a charge-only or damaged USB");
	vc_at(9, 1);
	vc_puts("cable, or a faulty USB port on the host.");

	vc_at(11, 1);
	vc_puts("Try a different cable AND/OR port.");

	vc_at(VC_ROWS - 1, VC_COLS - 16);
	vc_puts("Press to dismiss");

	vc_sync();
}

static void wait_any_button(struct menu_btns *b)
{
	while (1) {
		mdelay(5);
		if (btn_edge(&b->up) || btn_edge(&b->dn) ||
		    btn_edge(&b->sel) || btn_edge(&b->back) || ctrlc())
			return;
	}
}

/* Run a K_CMD item's command behind its mode splash. */
static void run_cmd_item(const struct menu_item *item)
{
	int ret;

	if (item->panel_title)
		draw_mode(item->panel_title);
	else
		vc_clear();

	printf("\nRunning: %s\n", item->action_cmd);
	ret = run_command(item->action_cmd, 0);
	if (ret)
		printf("\n(command returned %d)\n", ret);

	if (item->reset_after) {
		printf("\nResetting in 1s (USB gadget cleanup)...\n");
		mdelay(1000);
		run_command("reset", 0);
	}
}

static int do_bootmenu(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	struct menu_btns btns;
	int sel = 0;

	menu_ui_begin();
	menu_settings_sync_brightness();

	if (menu_btns_init(&btns))
		return CMD_RET_FAILURE;
	menu_btns_drain(&btns);

	if (env_get_yesno("maskrom_failed") == 1) {
		draw_maskrom_failed_screen();
		wait_any_button(&btns);
	}

	draw(sel);

	while (1) {
		int dt;

		/* 200 Hz: fast enough to never miss an encoder transition at
		 * human spin rates, tight enough that presses feel instant. */
		mdelay(5);

		dt = wheel_poll_detents();
		if (dt != 0) {
			/* CW moves the selection down. The + NITEMS keeps the
			 * modulo positive when wrapping past 0 upward. */
			sel = ((sel + dt) % NITEMS + NITEMS) % NITEMS;
			draw(sel);
			continue;
		}

		if (btn_edge(&btns.up)) {
			sel = (sel - 1 + NITEMS) % NITEMS;
			draw(sel);
		} else if (btn_edge(&btns.dn)) {
			sel = (sel + 1) % NITEMS;
			draw(sel);
		} else if (btn_edge(&btns.sel)) {
			switch (items[sel].kind) {
			case K_SETTINGS:
				menu_settings_run(&btns);
				break;
			case K_CHARGER:
				menu_charger_run(&btns);
				break;
			case K_HWINFO:
				menu_hwinfo_run(&btns);
				break;
			case K_CMD:
				run_cmd_item(&items[sel]);
				break;
			}
			draw(sel);
			menu_btns_drain(&btns);
		} else if (btn_edge(&btns.back)) {
			vc_clear();
			return 0;
		}

		if (ctrlc()) {
			vc_clear();
			return 0;
		}
	}
}

U_BOOT_CMD(
	bootmenu, 1, 1, do_bootmenu,
	"Car Thing on-panel boot menu",
	"\n"
	"  Renders a scrollable menu on the panel. Navigate with\n"
	"  preset1 (up) / preset4 (down), select with the wheel press,\n"
	"  cancel with back."
);
