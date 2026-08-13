// SPDX-License-Identifier: GPL-2.0+
/*
 * `setbright` / `blramp` — panel backlight control, used by the boot menu and
 * from the CLI. The PWM is INVERTED: 0 = brightest, 100 = dimmest.
 * See superbird-docs/uboot/splash-and-backlight.md.
 */

#include <command.h>
#include <backlight.h>
#include <env.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <vsprintf.h>
#include <dm.h>
#include <dm/uclass.h>

static int find_backlight(struct udevice **devp)
{
	int ret = uclass_first_device_err(UCLASS_PANEL_BACKLIGHT, devp);
	if (ret) {
		printf("setbright: no backlight uclass device (%d)\n", ret);
		return ret;
	}
	return 0;
}

static int do_setbright(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	struct udevice *bl;
	int level = -1;

	if (argc < 2)
		return CMD_RET_USAGE;

	/* Presets are named in "how bright" terms; the values are inverted. */
	if (!strcmp(argv[1], "off"))
		level = BACKLIGHT_OFF;
	else if (!strcmp(argv[1], "low"))
		level = 100;
	else if (!strcmp(argv[1], "med") || !strcmp(argv[1], "medium"))
		level = 70;
	else if (!strcmp(argv[1], "high") || !strcmp(argv[1], "max"))
		level = 0;
	else {
		long v = simple_strtol(argv[1], NULL, 10);
		if (v < 0 || v > 100) {
			printf("setbright: level must be 0..100 (or "
			       "off/low/med/high)\n");
			return CMD_RET_USAGE;
		}
		level = (int)v;
	}

	if (find_backlight(&bl))
		return CMD_RET_FAILURE;

	if (backlight_set_brightness(bl, level)) {
		printf("setbright: backlight_set_brightness(%d) failed\n",
		       level);
		return CMD_RET_FAILURE;
	}
	if (level == BACKLIGHT_OFF)
		printf("Backlight off\n");
	else
		printf("Backlight = %d%%\n", level);

	/* Store the literal argument, which apply_saved_brightness feeds back
	 * through this same parser next boot. Save only on change — otherwise
	 * the re-apply would rewrite the FAT on every reset. */
	{
		const char *cur = env_get("brightness");

		if (!cur || strcmp(cur, argv[1])) {
			env_set("brightness", argv[1]);
			env_save();
		}
	}
	return 0;
}

U_BOOT_CMD(
	setbright, 2, 1, do_setbright,
	"set panel backlight brightness",
	"<off|low|med|high|0-100>"
);

/*
 * Replay a backlight ramp live, for tuning the boot-splash ease-in without a
 * rebuild. The uclass quantizes onto the DT levels, so the ramp shows only as
 * many steps as lie between from and to.
 */
static int do_blramp(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	struct udevice *bl;
	int from, to, step_mag, delay, cur, step;

	if (argc < 3)
		return CMD_RET_USAGE;

	from = (int)simple_strtol(argv[1], NULL, 10);
	to = (int)simple_strtol(argv[2], NULL, 10);
	step_mag = (argc > 3) ? (int)simple_strtol(argv[3], NULL, 10) : 4;
	delay = (argc > 4) ? (int)simple_strtol(argv[4], NULL, 10) : 12;

	if (from < 0 || from > 100 || to < 0 || to > 100) {
		printf("blramp: from/to must be 0..100\n");
		return CMD_RET_USAGE;
	}
	if (step_mag < 1)
		step_mag = 1;
	if (delay < 0)
		delay = 0;

	if (find_backlight(&bl))
		return CMD_RET_FAILURE;

	printf("blramp: %d -> %d, step %d%%, %d ms/tick\n",
	       from, to, step_mag, delay);

	backlight_set_brightness(bl, from);
	cur = from;
	step = (to < from) ? -step_mag : step_mag;
	while (cur != to) {
		if ((step < 0 && cur + step < to) ||
		    (step > 0 && cur + step > to))
			cur = to;
		else
			cur += step;
		backlight_set_brightness(bl, cur);
		if (delay)
			mdelay(delay);
	}
	return 0;
}

U_BOOT_CMD(
	blramp, 5, 0, do_blramp,
	"ramp panel backlight between two levels (tune boot ease-in)",
	"<from> <to> [step%] [delay_ms]\n"
	"    levels 0..100 (inverted PWM: 0=brightest, 100=dimmest)"
);
