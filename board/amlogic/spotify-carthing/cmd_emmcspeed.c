// SPDX-License-Identifier: GPL-2.0+
/*
 * eMMC bus-speed control for the Car Thing.
 *
 * Field units bootloop because the eMMC is marginal at DDR52: reading
 * the GPT is a couple of sectors and survives, loading a 22 MB kernel
 * does not, so the partition scan looks fine right before the boot
 * fails. Running the bus slowly fixes those units — confirmed on one
 * that previously never booted.
 *
 * But slow is only the right answer for *booting*. Flashing over
 * fastboot or UMS wants the bandwidth, and there a host is attached, a
 * failure is visible, and a retry costs nothing. So rather than slowing
 * the bus everywhere, pin it slow for the boot path and lift the pin
 * when a host session starts.
 *
 * The mechanism is u-boot's own: mmc->user_speed_mode, honoured by
 * mmc_start_init(), which masks host_caps down to that single mode plus
 * legacy/1-bit. Two consequences worth knowing:
 *
 *  - the mode has to be enabled in the DT to be selectable, so the DT
 *    keeps its full DDR52/HS200 capabilities and we restrict at
 *    runtime. Deleting the caps from the DT (an earlier approach here)
 *    would make fast permanently unreachable, which is exactly the
 *    flexibility we want;
 *  - it needs CONFIG_MMC_SPEED_MODE_SET, otherwise the field is
 *    ignored and everything silently runs fast. carthing_mmc_set_mode()
 *    refuses rather than pretending in that case.
 */

#include <blk.h>
#include <command.h>
#include <dm.h>
#include <mmc.h>
#include <vsprintf.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "carthing_mmc.h"

/* Whether the boot-time pin is in effect, so fast_for_host() knows what
 * to fall back to and doesn't churn the card when nothing was pinned. */
static bool mmc_pinned_slow;

int carthing_mmc_set_mode(enum bus_mode mode, bool reinit)
{
	struct mmc *mmc;
	int ret;

	if (!IS_ENABLED(CONFIG_MMC_SPEED_MODE_SET)) {
		printf("emmcspeed: CONFIG_MMC_SPEED_MODE_SET is off — "
		       "speed pinning does nothing in this build\n");
		return -ENOSYS;
	}

	mmc = find_mmc_device(CARTHING_MMC_DEV);
	if (!mmc) {
		printf("emmcspeed: mmc %d not found\n", CARTHING_MMC_DEV);
		return -ENODEV;
	}

	mmc->user_speed_mode = mode;
	mmc_pinned_slow = (mode != MMC_MODES_END);

	if (!reinit)
		return 0;

	/* Force the card back through init so the new cap mask is applied,
	 * and drop cached blocks read at the old speed. */
	mmc->has_init = 0;
	ret = mmc_init(mmc);
	if (ret) {
		printf("emmcspeed: re-init failed (%d)\n", ret);
		return ret;
	}
	if (IS_ENABLED(CONFIG_BLOCK_CACHE)) {
		struct blk_desc *bd = mmc_get_blk_desc(mmc);

		blkcache_invalidate(bd->uclass_id, bd->devnum);
	}

	printf("eMMC now in %s\n", carthing_mmc_current_mode());
	return 0;
}

void carthing_mmc_fast_for_host(void)
{
	if (!IS_ENABLED(CONFIG_MMC_SPEED_MODE_SET) || !mmc_pinned_slow)
		return;

	printf("Host session: lifting the eMMC speed pin\n");
	if (carthing_mmc_set_mode(MMC_MODES_END, true)) {
		/* Marginal card: better slow-but-working than wedged
		 * halfway through someone's flash. */
		printf("Falling back to the boot speed mode\n");
		carthing_mmc_set_mode(CONFIG_CARTHING_MMC_BOOT_MODE, true);
	}
}

const char *carthing_mmc_current_mode(void)
{
	struct mmc *mmc = find_mmc_device(CARTHING_MMC_DEV);

	if (!mmc)
		return "<no mmc>";
	if (!mmc->has_init)
		return "<not initialised>";
	return mmc_mode_name(mmc->selected_mode);
}

static int do_emmcspeed(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	enum bus_mode mode;

	if (argc < 2) {
		printf("eMMC is in %s\n", carthing_mmc_current_mode());
		return 0;
	}

	if (!strcmp(argv[1], "fast")) {
		mode = MMC_MODES_END;
	} else if (!strcmp(argv[1], "slow")) {
		mode = CONFIG_CARTHING_MMC_BOOT_MODE;
	} else {
		char *end;

		mode = (enum bus_mode)dectoul(argv[1], &end);
		if (*end || mode >= MMC_MODES_END) {
			printf("emmcspeed: expected fast, slow, or a bus_mode index < %d\n",
			       MMC_MODES_END);
			return CMD_RET_USAGE;
		}
	}

	return carthing_mmc_set_mode(mode, true) ? CMD_RET_FAILURE : 0;
}

U_BOOT_CMD(
	emmcspeed, 2, 0, do_emmcspeed,
	"show or change the eMMC bus speed mode",
	"\n"
	"  emmcspeed          show the mode the card negotiated\n"
	"  emmcspeed slow     pin to the conservative boot mode\n"
	"  emmcspeed fast     un-pin; renegotiate the best mode the DT allows\n"
	"  emmcspeed <n>      pin to enum bus_mode index n (1=MMC_HS 26MHz,\n"
	"                     3=MMC_HS_52, 4=MMC_DDR_52, 10=MMC_HS_200)\n"
	"\n"
	"  Changing speed re-initialises the card. Boots are pinned slow\n"
	"  because marginal eMMCs fail on sustained reads; fastboot and UMS\n"
	"  lift the pin automatically so flashing keeps full bandwidth."
);
