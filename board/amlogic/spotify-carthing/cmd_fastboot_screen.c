// SPDX-License-Identifier: GPL-2.0+
/*
 * `fastboot_with_screen` — fastboot with the on-panel FASTBOOT splash.
 *
 * Mirrors the bootmenu's K_CMD dispatch (splash -> run_command -> reset)
 * without the menu wrapper. carthing_boot_route() runs this when the SoC
 * reports BOOT_DEVICE_USB, so a RAM-loaded dev iteration lands the host in a
 * fastboot session with the panel saying so.
 */

#include <command.h>
#include <linux/delay.h>

#include "menu_ui.h"

static int do_fastboot_with_screen(struct cmd_tbl *cmdtp, int flag,
				   int argc, char *const argv[])
{
	int ret;

	menu_ui_begin();
	draw_mode("FASTBOOT");

	printf("\nRunning: fastboot 0\n");
	ret = run_command("fastboot 0", 0);
	if (ret)
		printf("\n(fastboot returned %d)\n", ret);

	printf("\nResetting in 1s (USB gadget cleanup)...\n");
	mdelay(1000);
	run_command("reset", 0);
	return 0;	/* not reached */
}

U_BOOT_CMD(
	fastboot_with_screen, 1, 1, do_fastboot_with_screen,
	"Enter fastboot with the on-panel FASTBOOT splash",
	"\n"
	"  Paints the same centred FASTBOOT title the bootmenu uses,\n"
	"  runs `fastboot 0`, then resets the device on exit."
);
