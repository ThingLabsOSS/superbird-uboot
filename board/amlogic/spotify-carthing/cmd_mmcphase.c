// SPDX-License-Identifier: GPL-2.0+
/*
 * `mmcphase` — sweep the eMMC host clock phase / delay lines.
 *
 * Why this exists: field units bootloop because u-boot can't read the
 * eMMC reliably at DDR52, while the same units are stable under Linux
 * at full speed. The difference is calibration. Our
 * meson_mmc_config_clock() programs core phase and tx phase and leaves
 * RX phase and both delay lines at zero, so data is sampled at one
 * fixed point chosen against whatever eMMC part happened to be on the
 * bench. The vendor driver tunes rxclk_rx_phase / rxclk_rx_delay per
 * unit; Linux tunes HS200. We do neither.
 *
 * That predicts a correlation with the flash part, and the field data
 * agrees: two bootlooping units both carry eMMC "004GA0" where a
 * healthy one carries "S40004".
 *
 * This command makes the window measurable. Set a phase, run a big
 * read, see whether it survives. `mmcphase scan` automates the search
 * over RX phase and delay, which is the axis that matters for reads.
 */

#include <blk.h>
#include <command.h>
#include <dm.h>
#include <mmc.h>
#include <vsprintf.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "../../../drivers/mmc/meson_gx_mmc.h"
#include "carthing_mmc.h"

/* Read enough to exercise sustained transfer rather than a lucky
 * sector or two — the failure mode is specifically that small reads
 * succeed and kernel-sized ones don't. 4 MiB at 512 B/block. */
#define PHASE_TEST_BLOCKS	8192
#define PHASE_TEST_ADDR		0x08000000UL

static const char *phase_name(u8 v)
{
	static const char * const names[] = { "0", "90", "180", "270" };

	return v == MESON_PHASE_DEFAULT ? "default" :
	       (v < 4 ? names[v] : "?");
}

static void phase_show(void)
{
	struct meson_mmc_phase p;

	meson_mmc_get_phase(&p);
	printf("core=%s tx=%s rx=%s tx_delay=%u rx_delay=%u (delay steps = 200ps)\n",
	       phase_name(p.core), phase_name(p.tx), phase_name(p.rx),
	       p.tx_delay, p.rx_delay);
	printf("bus mode: %s\n", carthing_mmc_current_mode());
}

/*
 * Apply a phase and re-read. Returns 0 if the read survived.
 *
 * The card is re-initialised because set_ios (where the phase is
 * applied) only runs as part of init; poking the register without it
 * would leave the controller and our idea of the phase disagreeing.
 */
static int phase_try(const struct meson_mmc_phase *p, bool quiet)
{
	struct mmc *mmc = find_mmc_device(CARTHING_MMC_DEV);
	struct blk_desc *bd;
	ulong n;

	if (!mmc)
		return -ENODEV;

	meson_mmc_set_phase(p);

	mmc->has_init = 0;
	if (mmc_init(mmc)) {
		if (!quiet)
			printf("  init failed\n");
		return -EIO;
	}

	bd = mmc_get_blk_desc(mmc);
	if (IS_ENABLED(CONFIG_BLOCK_CACHE))
		blkcache_invalidate(bd->uclass_id, bd->devnum);

	n = blk_dread(bd, 0, PHASE_TEST_BLOCKS, (void *)PHASE_TEST_ADDR);
	if (n != PHASE_TEST_BLOCKS) {
		if (!quiet)
			printf("  read failed (%lu/%u blocks)\n",
			       n, PHASE_TEST_BLOCKS);
		return -EIO;
	}

	return 0;
}

static int do_mmcphase(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	struct meson_mmc_phase p;

	if (argc < 2) {
		phase_show();
		return 0;
	}

	if (!strcmp(argv[1], "reset")) {
		p.core = MESON_PHASE_DEFAULT;
		p.tx = MESON_PHASE_DEFAULT;
		p.rx = MESON_PHASE_DEFAULT;
		p.tx_delay = 0;
		p.rx_delay = 0;
		return phase_try(&p, false) ? CMD_RET_FAILURE : 0;
	}

	if (!strcmp(argv[1], "test")) {
		meson_mmc_get_phase(&p);
		printf("testing current phase: ");
		phase_show();
		if (phase_try(&p, false)) {
			printf("FAIL\n");
			return CMD_RET_FAILURE;
		}
		printf("OK (%d MiB read clean)\n", PHASE_TEST_BLOCKS / 2048);
		return 0;
	}

	/*
	 * `scan` walks RX phase x RX delay at the current bus speed and
	 * reports every combination that reads cleanly. Run it at full
	 * speed (`emmcspeed fast`) on a failing unit — the set of
	 * survivors is the timing window, and its centre is the value
	 * worth defaulting to.
	 */
	if (!strcmp(argv[1], "scan")) {
		int rx, delay, ok = 0;

		printf("scanning rx phase x rx delay at current speed...\n");
		for (rx = 0; rx < 4; rx++) {
			for (delay = 0; delay < 64; delay += 4) {
				meson_mmc_get_phase(&p);
				p.rx = rx;
				p.rx_delay = delay;
				if (!phase_try(&p, true)) {
					printf("  PASS rx=%-3s rx_delay=%2d (%d ps)\n",
					       phase_name(rx), delay,
					       delay * 200);
					ok++;
				}
			}
		}
		printf("%d passing combinations\n", ok);
		if (!ok)
			printf("none passed — try a slower mode first\n");
		return ok ? 0 : CMD_RET_FAILURE;
	}

	if (argc < 4)
		return CMD_RET_USAGE;

	meson_mmc_get_phase(&p);
	p.core = (u8)dectoul(argv[1], NULL);
	p.tx = (u8)dectoul(argv[2], NULL);
	p.rx = (u8)dectoul(argv[3], NULL);
	if (argc > 4)
		p.rx_delay = (u8)dectoul(argv[4], NULL);
	if (argc > 5)
		p.tx_delay = (u8)dectoul(argv[5], NULL);

	if (p.core > 3 || p.tx > 3 || p.rx > 3 ||
	    p.rx_delay > 63 || p.tx_delay > 63) {
		printf("phases are 0-3 (0/90/180/270), delays 0-63\n");
		return CMD_RET_USAGE;
	}

	if (phase_try(&p, false))
		return CMD_RET_FAILURE;

	printf("OK: ");
	phase_show();
	return 0;
}

U_BOOT_CMD(
	mmcphase, 6, 0, do_mmcphase,
	"show/sweep the eMMC host clock phase and delay lines",
	"\n"
	"  mmcphase                       show the current settings\n"
	"  mmcphase test                  re-read 4 MiB at the current settings\n"
	"  mmcphase scan                  find every rx phase/delay that reads clean\n"
	"  mmcphase reset                 back to the driver defaults\n"
	"  mmcphase <core> <tx> <rx> [rx_delay] [tx_delay]\n"
	"                                 phases 0-3 = 0/90/180/270 degrees,\n"
	"                                 delays 0-63 in 200 ps steps\n"
	"\n"
	"  Each change re-initialises the card and reads 4 MiB to prove it.\n"
	"  Upstream leaves rx phase and both delays at zero, so reads are\n"
	"  sampled at one fixed point that suits some eMMC parts and not\n"
	"  others — run `emmcspeed fast` then `mmcphase scan` on a unit that\n"
	"  fails at full speed to measure its actual window."
);
