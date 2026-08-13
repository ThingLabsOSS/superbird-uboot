// SPDX-License-Identifier: GPL-2.0+
/*
 * `probe` — one-shot automated hardware report, fetchable over fastboot.
 *
 * Diagnosing the field bootloops has meant walking each owner through a
 * different sequence of fastboot commands, transcribing the output by
 * hand, and getting it wrong at least once (a device-name typo cost a
 * round trip). This collapses that into: chainload one image, wait,
 * pull a text file.
 *
 *     fip-tool ramboot u-boot-probe.spotify.encrypt     (or booti it)
 *     ...unit runs the probe by itself and lands in fastboot...
 *     fastboot fetch probe report.txt
 *
 * Nothing is written to the unit: the report lives in RAM and is served
 * through a virtual `fastboot fetch` target (fastboot_fetch_virtual),
 * so a bootlooping device is never modified by being measured.
 *
 * Capture works by resetting the console recorder, letting the probe
 * print normally — so the panel shows progress and a UART, if anyone
 * has one, sees it live — and draining the recording into a buffer at
 * the end.
 */

#include <blk.h>
#include <command.h>
#include <console.h>
#include <dm.h>
#include <env.h>
#include <fastboot.h>
#include <malloc.h>
#include <time.h>
#include <mmc.h>
#include <version_string.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "carthing_mmc.h"

void carthing_mmc_phase_scan(void);

/* The report. Sized for the console recorder's worth of text; the
 * report runs a few KiB. */
#define PROBE_REPORT_MAX	(64 * 1024)

static char *probe_report;
static u32 probe_report_len;

/* Kernel-sized read. The whole failure mode is that small reads succeed
 * and a 22 MB kernel load doesn't, so a probe that only reads a few
 * sectors would reproduce the original misdiagnosis. */
#define BIG_READ_BLOCKS		32768		/* 16 MiB */
#define BIG_READ_ADDR		0x08000000UL

static void probe_big_read(const char *label)
{
	struct mmc *mmc = find_mmc_device(CARTHING_MMC_DEV);
	struct blk_desc *bd;
	ulong start, took, n;

	if (!mmc) {
		printf("%-12s no mmc\n", label);
		return;
	}

	bd = mmc_get_blk_desc(mmc);
	if (IS_ENABLED(CONFIG_BLOCK_CACHE))
		blkcache_invalidate(bd->uclass_id, bd->devnum);

	start = get_timer(0);
	n = blk_dread(bd, 0, BIG_READ_BLOCKS, (void *)BIG_READ_ADDR);
	took = get_timer(start);

	if (n != BIG_READ_BLOCKS) {
		printf("%-12s FAIL after %lu of %u blocks (%lu ms)\n",
		       label, n, BIG_READ_BLOCKS, took);
		return;
	}

	printf("%-12s OK  16 MiB in %lu ms (%lu KB/s)\n", label, took,
	       took ? (16384UL * 1000UL) / took : 0);
}

static void probe_speed_case(const char *label, int mode)
{
	if (carthing_mmc_set_mode(mode, true)) {
		printf("%-12s could not select mode %d\n", label, mode);
		return;
	}
	printf("%-12s mode=%s\n", label, carthing_mmc_current_mode());
	probe_big_read(label);
}

/*
 * Everything we have had to ask an owner for, in one pass. Ordered so
 * that if it dies part way the most valuable data is already printed.
 */
static void probe_collect(void)
{
	printf("===== Car Thing probe report =====\n");
	printf("%s\n\n", version_string);

	printf("-- eMMC identity --\n");
	run_command("mmc info", 0);

	printf("\n-- rails (adc) --\n");
	printf("ch4=VCCK (expect ~860 mV)  ch5=VDDEE (expect ~800 mV)\n");
	run_command("adc scan adc@9000", 0);

	printf("\n-- boot state --\n");
	run_command("debugreport", 0);

	printf("\n-- sustained read at each bus speed --\n");
	probe_speed_case("HS 26MHz:", 1);	/* MMC_HS */
	probe_speed_case("HS52 SDR:", 3);	/* MMC_HS_52 */
	probe_speed_case("DDR52:", 4);		/* MMC_DDR_52 */

	printf("\n-- rx phase x rx delay window at DDR52 --\n");
	printf("('.' = 4 MiB read clean, 'X' = failed)\n");
	if (carthing_mmc_set_mode(4, true)) {
		printf("cannot reach DDR52 — skipping the phase sweep\n");
	} else {
		carthing_mmc_phase_scan();
	}

	printf("\n-- back to the safe boot speed --\n");
	carthing_mmc_set_mode(CONFIG_CARTHING_MMC_BOOT_MODE, true);

	printf("===== end of report =====\n");
}

/*
 * Run the probe and stash the recording.
 *
 * console_record_reset_enable() is called first so the buffer holds
 * only this run, then drained line by line afterwards. Draining has to
 * happen before anything else prints, or the report picks up unrelated
 * output.
 */
void carthing_probe_run(void)
{
	char line[256];

	if (!probe_report) {
		probe_report = malloc(PROBE_REPORT_MAX);
		if (!probe_report) {
			printf("probe: out of memory\n");
			return;
		}
	}
	probe_report_len = 0;

	console_record_reset_enable();
	probe_collect();

	while (console_record_avail() > 0 &&
	       probe_report_len < PROBE_REPORT_MAX - sizeof(line)) {
		int n = console_record_readline(line, sizeof(line) - 2);

		if (n < 0)
			break;
		strcat(line, "\n");
		n = strlen(line);
		memcpy(probe_report + probe_report_len, line, n);
		probe_report_len += n;
	}

	printf("probe: %u bytes captured — pull it with:\n", probe_report_len);
	printf("  fastboot fetch probe report.txt\n");
}

/*
 * Serve the report as a virtual fetch target so the host can pull it
 * with a command it already has, without the report ever touching the
 * eMMC of the unit under test.
 */
int fastboot_fetch_virtual(const char *name, void *buf, u32 bufsz,
			   u32 *out_len)
{
	if (strcmp(name, "probe"))
		return -ENOENT;

	/* A NULL buffer is a size query — getvar partition-size asks that
	 * way before the host will issue the fetch itself. */
	if (!buf) {
		*out_len = probe_report_len ? probe_report_len : 1;
		return 0;
	}

	if (!probe_report || !probe_report_len) {
		/* Fetching before the probe ran is a plausible mistake, so
		 * hand back an explanation rather than an empty file. */
		static const char none[] =
			"no probe report — run `probe` first\n";

		*out_len = min_t(u32, bufsz, (u32)sizeof(none) - 1);
		memcpy(buf, none, *out_len);
		return 0;
	}

	*out_len = min_t(u32, bufsz, probe_report_len);
	memcpy(buf, probe_report, *out_len);
	return 0;
}

static int do_probe(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	carthing_probe_run();
	return 0;
}

U_BOOT_CMD(
	probe, 1, 0, do_probe,
	"run the full hardware probe and stash the report",
	"\n"
	"  Collects eMMC identity, rail voltages, boot/A-B state, a\n"
	"  sustained 16 MiB read at each bus speed, and the rx phase x\n"
	"  delay window at DDR52. Nothing is written to the device.\n"
	"\n"
	"  Retrieve with:  fastboot fetch probe report.txt\n"
	"\n"
	"  CONFIG_CARTHING_AUTOPROBE builds an image that runs this by\n"
	"  itself at boot and then waits in fastboot."
);
