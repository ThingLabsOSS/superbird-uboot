// SPDX-License-Identifier: GPL-2.0+
/*
 * `chainload` — run another u-boot.bin from the fastboot staging buffer.
 *
 * Motivation: RAM-loading a diagnostic u-boot normally means mask-ROM USB
 * mode, which needs the button-1+4 chord and a reset. A unit that merely
 * bootloops can still be driven into fastboot (the boot router honours a
 * held menu button before bootcmd runs), so if fastboot can chainload a
 * second u-boot we get the same "try an image without flashing anything"
 * loop over a cable the user already has plugged in:
 *
 *     fastboot stage u-boot.bin
 *     fastboot oem console "chainload"
 *
 * Why not just `go`: cmd/boot.c's do_go_exec() calls the entry point
 * directly, so the incoming u-boot starts executing its cache and MMU
 * setup while the outgoing one's MMU is still on and its page tables are
 * still live. Confirmed on hardware — the SoC drops off USB and never
 * comes back. Booting Linux has exactly the same problem, and arm64
 * solves it in do_nonsec_virt_switch() with dcache_disable(), which on
 * armv8 flushes the caches and takes the MMU down together. Do the same
 * here, plus an icache invalidate because the image arrived through the
 * data path.
 *
 * The copy is memmove() rather than memcpy(): the staging buffer and
 * CONFIG_TEXT_BASE are far apart today, but nothing enforces that.
 */

#include <command.h>
#include <cpu_func.h>
#include <env.h>
#include <g_dnl.h>
#include <mapmem.h>
#include <vsprintf.h>
#include <linux/string.h>

static int do_chainload(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	void (*entry)(void) = (void (*)(void))(uintptr_t)CONFIG_TEXT_BASE;
	ulong src = CONFIG_FASTBOOT_BUF_ADDR;
	ulong size = 0;

	if (argc > 1)
		src = hextoul(argv[1], NULL);
	if (argc > 2)
		size = hextoul(argv[2], NULL);

	/* `fastboot stage` leaves the transfer size in fastboot_bytes; a
	 * plain `load`/`fatload` leaves it in filesize. Either will do. */
	if (!size)
		size = env_get_hex("fastboot_bytes", 0);
	if (!size)
		size = env_get_hex("filesize", 0);
	if (!size) {
		printf("chainload: no size — pass one, or stage an image first\n");
		return CMD_RET_USAGE;
	}

	printf("chainload: 0x%lx -> 0x%x (0x%lx bytes), then jumping\n",
	       src, CONFIG_TEXT_BASE, size);

	/* Let the host see the message land before USB goes away. */
	if (IS_ENABLED(CONFIG_USB_GADGET_DOWNLOAD))
		g_dnl_unregister();

	memmove((void *)(uintptr_t)CONFIG_TEXT_BASE,
		(void *)(uintptr_t)src, size);

	/* Order matters: push the image out of the dcache and drop the
	 * stale instruction stream before the MMU goes away, then take
	 * caches+MMU down so the new u-boot's own setup starts from the
	 * state its _start expects. Nothing below here may be cached. */
	flush_dcache_all();
	invalidate_icache_all();
	dcache_disable();
	icache_disable();

	entry();

	/* Only reached if the image isn't executable at TEXT_BASE. */
	printf("chainload: returned — image not runnable?\n");
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(
	chainload, 3, 0, do_chainload,
	"run another u-boot.bin from RAM (no flashing)",
	"[src [size]]\n"
	"  Copies a raw u-boot.bin to CONFIG_TEXT_BASE, tears down caches\n"
	"  and the MMU, and jumps to it. src defaults to the fastboot\n"
	"  staging buffer and size to fastboot_bytes/filesize, so the usual\n"
	"  invocation from a host is:\n"
	"    fastboot stage u-boot.bin\n"
	"    fastboot oem console \"chainload\"\n"
	"  Unlike `go`, this survives — see the comment in cmd_chainload.c."
);
