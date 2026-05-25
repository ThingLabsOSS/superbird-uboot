// SPDX-License-Identifier: GPL-2.0+
/*
 * Car Thing board revision detection.
 *
 * Spotify wires a resistor divider to SARADC channel 1; the divider
 * value picks out one of 12 revisions. Vendor decode table is in
 * include/spotify/hw_probe.h (sp_board_revision_confs[]).
 *
 * Vendor expects the raw 10-bit ADC reading (0..1023); our mainline
 * meson-saradc driver returns 12-bit (0..4095). Divide by 4 to bring
 * back into the vendor's range and look up.
 */
#include <adc.h>
#include <command.h>
#include <dm.h>
#include <dm/uclass.h>
#include <env.h>
#include <vsprintf.h>

#include "boardrev.h"

/* Vendor's resistor-divider table: 10-bit ADC value -> revision number. */
static const struct {
	int rev;
	int hw_id;
} board_rev_table[] = {
	{  1,   85 },
	{  2,  167 },
	{  3,  248 },
	{  4,  334 },
	{  5,  420 },
	{  6,  512 },
	{  7,  603 },
	{  8,  689 },
	{  9,  775 },
	{ 10,  856 },
	{ 11,  938 },
	{ 12, 1023 },
};

#define NREVS	((int)(sizeof(board_rev_table) / sizeof(board_rev_table[0])))

static int lookup_rev(int adc10)
{
	int i, best = -1, best_d = 1 << 30;

	for (i = 0; i < NREVS; i++) {
		int d = adc10 - board_rev_table[i].hw_id;
		if (d < 0)
			d = -d;
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	/* Half-step window between adjacent entries. Outside that → unknown. */
	if (best < 0 || best_d > 60)
		return 0;
	return board_rev_table[best].rev;
}

int carthing_probe_board_rev(void)
{
	struct udevice *dev;
	uint raw;
	int ret;

	ret = uclass_get_device_by_name(UCLASS_ADC, "adc@9000", &dev);
	if (ret)
		return -1;
	ret = adc_channel_single_shot("adc@9000", 1, &raw);
	if (ret)
		return -1;
	return lookup_rev((int)(raw / 4));
}

static int do_boardrev(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	int rev = carthing_probe_board_rev();
	char buf[8];

	if (rev < 0) {
		printf("boardrev: SARADC read failed\n");
		return CMD_RET_FAILURE;
	}
	if (rev == 0) {
		printf("boardrev: unknown (ADC value outside vendor's table)\n");
		env_set("board_revision", "0");
		return CMD_RET_SUCCESS;
	}
	printf("boardrev: REV_%d\n", rev);
	snprintf(buf, sizeof(buf), "%d", rev);
	if (!env_get("board_revision") ||
	    strcmp(env_get("board_revision"), buf))
		env_set("board_revision", buf);
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	boardrev, 1, 1, do_boardrev,
	"detect Car Thing hardware revision via SARADC ch 1",
	"\n"
	"  Reads SARADC channel 1 (Spotify HW-rev resistor divider) and\n"
	"  maps to one of REV_1..REV_12 per vendor's hw_probe.h table.\n"
	"  Also updates env var board_revision (without saveenv)."
);
