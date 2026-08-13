// SPDX-License-Identifier: GPL-2.0+
/*
 * Board file for the Spotify Car Thing (G12A / S905X2).
 */

#include <init.h>
#include <env.h>
#include <command.h>
#include <backlight.h>
#include <blk.h>
#include <button.h>
#include <console.h>
#include <dm.h>
#include <dm/uclass.h>
#include <dm/uclass-internal.h>
#include <fastboot.h>
#include <fs.h>
#include <part.h>
#include <video.h>
#include <mapmem.h>
#include <mmc.h>
#include <asm/arch/boot.h>
#include <asm/arch/sm.h>
#include <asm/io.h>
#include <g_dnl.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <vsprintf.h>

#include "charger.h"

int meson_get_boot_device(void);

/*
 * eFuse user-area serials, read via the secure-monitor SMC (BL31 owns the
 * fuse controller MMIO). Layout: superbird-docs/uboot/efuse_architecture.md.
 */
#define EFUSE_USID_OFFSET	18
#define EFUSE_USID_SIZE		16
#define EFUSE_F_SERIAL_OFFSET	34
#define EFUSE_F_SERIAL_SIZE	15
#define FALLBACK_SERIAL		"AMLG12ASPOTIFYCARTHING"

static void efuse_str_trim(char *s, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (!s[i] || !isprint((unsigned char)s[i])) {
			s[i] = '\0';
			return;
		}
	}
	s[n] = '\0';
}

static void set_serial_from_efuse(void)
{
	char usid[EFUSE_USID_SIZE + 1] = {0};
	char fser[EFUSE_F_SERIAL_SIZE + 1] = {0};
	ssize_t len;

	len = meson_sm_read_efuse(EFUSE_USID_OFFSET, usid, EFUSE_USID_SIZE);
	if (len != EFUSE_USID_SIZE)
		goto fallback;
	efuse_str_trim(usid, EFUSE_USID_SIZE);
	if (!usid[0])
		goto fallback;
	env_set("serial#", usid);

	/* Factory serial is optional — soft-fail. */
	len = meson_sm_read_efuse(EFUSE_F_SERIAL_OFFSET, fser,
				  EFUSE_F_SERIAL_SIZE);
	if (len == EFUSE_F_SERIAL_SIZE) {
		efuse_str_trim(fser, EFUSE_F_SERIAL_SIZE);
		if (fser[0])
			env_set("f_serial", fser);
	}
	return;

fallback:
	env_set("serial#", FALLBACK_SERIAL);
}

/*
 * Boot glow + splash ramp tuning. Backlight percent is INVERTED (higher =
 * dimmer). Tune on-device with `blramp`.
 * Rationale: superbird-docs/uboot/splash-and-backlight.md.
 */
#define CARTHING_BOOT_GLOW		100	/* dim glow */
#define CARTHING_PANEL_SETTLE_MS	80	/* panel lock time before painting */
#define CARTHING_RAMP_STEP		4	/* percent per ramp tick */
#define CARTHING_RAMP_STEP_MS		12

/*
 * Light the backlight ~100ms into power-on, before the panel is initialised,
 * so the device reads as alive (mirrors stock). Only the backlight is probed
 * here; UCLASS_VIDEO is deferred to misc_init_r.
 */
int board_init(void)
{
	struct udevice *dev;

	/* Explicit chain: board_init runs before initr_dm_devices, so a naive
	 * uclass_first_device on the backlight only sometimes wins the race
	 * against deferred probe-during-bind init. */
	uclass_first_device_err(UCLASS_CLK, &dev);
	uclass_first_device_err(UCLASS_PWM, &dev);
	if (!uclass_first_device_err(UCLASS_PANEL_BACKLIGHT, &dev)) {
		backlight_enable(dev);
		/* Dim, not the DT level — that is hardware-max on this PWM. */
		backlight_set_brightness(dev, CARTHING_BOOT_GLOW);
	}
	return 0;
}

/* Settled = not mid-detection (CHG_DET_RUN_S) and something is attached. */
static int charger_status_settled(uint8_t status)
{
	return ((status >> 6) & 1) == 0 && (status & 0xf) != 0;
}

/*
 * CHG_TYP_S -> peak CPU kHz the kernel should cap scaling_max_freq at,
 * published as ${charger_cap_khz} for extlinux to substitute. Bands and
 * kernel-side wiring: superbird-misc-notes/i2c/README.md.
 * Unknown/unreadable falls through to the conservative SDP cap.
 */
static const char *charger_cpufreq_cap_khz(uint8_t status)
{
	switch (status & 0xf) {
	case 0x2:	/* USB CDP (~1.5 A host) */
		return "1704000";
	case 0x3:	/* USB DCP (~1.5 A charger) */
	case 0x6:	/* Apple 2 A — DCP-equivalent */
		return "1800000";
	case 0x1:	/* USB SDP (~500 mA host) */
	case 0x4:	/* Apple 500 mA */
	case 0x5:	/* Apple 1 A — between SDP and CDP, stay conservative */
	case 0x7:	/* Special 500 mA */
	case 0x0:	/* no source visible — worst case */
	default:	/* reserved / unknown — worst case */
		return "1512000";
	}
}

static void log_charger_state(void)
{
	struct carthing_charger_info info;
	const char *cap_khz;

	if (carthing_charger_read(&info) || !info.valid) {
		printf("Charger: I2C read failed (probably no chip on this rev)\n");
		/* Set explicitly, not left empty, so /proc/cmdline self-describes. */
		env_set("charger_cap_khz", "1512000");
		return;
	}

	if (!charger_status_settled(info.status)) {
		(void)carthing_charger_redetect();
		mdelay(CARTHING_CHARGER_REDETECT_DELAY_MS);
		(void)carthing_charger_read(&info);
	}

	printf("Charger: %s (status=0x%02x, MAX14656 rev %d)\n",
	       carthing_charger_type_str(info.status),
	       info.status, info.regs[0x00] & 0xf);

	cap_khz = charger_cpufreq_cap_khz(info.status);
	env_set("charger_cap_khz", cap_khz);
	printf("Charger: CPU freq cap = %s kHz\n", cap_khz);
}

/* setbright-style level string -> inverted percent (0 = brightest). */
static int brightness_to_percent(const char *v)
{
	if (!strcmp(v, "off"))
		return BACKLIGHT_OFF;
	if (!strcmp(v, "low"))
		return 100;
	if (!strcmp(v, "med") || !strcmp(v, "medium"))
		return 70;
	if (!strcmp(v, "high") || !strcmp(v, "max"))
		return 0;
	{
		long n = simple_strtol(v, NULL, 10);

		if (n < 0)
			n = 0;
		if (n > 100)
			n = 100;
		return (int)n;
	}
}

/*
 * Ease from the boot glow up to the saved brightness. Called after the splash
 * paints so the ramp lands on a clean logo.
 */
static void apply_saved_brightness(void)
{
	const char *v = env_get("brightness");
	struct udevice *bl;
	int target, cur, step;

	/* "med", not the DT level — that is hardware-max on this PWM. */
	if (!v || !*v)
		v = "med";
	target = brightness_to_percent(v);

	if (uclass_first_device_err(UCLASS_PANEL_BACKLIGHT, &bl)) {
		/* Honour the saved level via the cmd path, just without a ramp. */
		char cmd[24];

		snprintf(cmd, sizeof(cmd), "setbright %s", v);
		run_command(cmd, 0);
		return;
	}

	if (target == BACKLIGHT_OFF) {
		backlight_set_brightness(bl, BACKLIGHT_OFF);
		return;
	}

	/* Inverted PWM: brightening steps the value DOWN. */
	cur = CARTHING_BOOT_GLOW;
	step = (target < cur) ? -CARTHING_RAMP_STEP : CARTHING_RAMP_STEP;
	while (cur != target) {
		if ((step < 0 && cur + step < target) ||
		    (step > 0 && cur + step > target))
			cur = target;
		else
			cur += step;
		backlight_set_brightness(bl, cur);
		mdelay(CARTHING_RAMP_STEP_MS);
	}
}

static void detect_maskrom_failed(int boot_device);
static void carthing_boot_route(void);

/*
 * Publish the mask-ROM boot source as `boot_source` (usb/sd/emmc/other).
 * USB means we were RAM-loaded, so carthing_boot_route sends it to fastboot.
 *
 * Deliberately NOT env_set("bootcmd") — a later env_save would persist that
 * transient override and every subsequent eMMC boot would auto-fastboot.
 * See superbird-docs/uboot/boot-flow.md.
 */
static void set_boot_source(void)
{
	int dev = meson_get_boot_device();
	const char *src;

	switch (dev) {
	case BOOT_DEVICE_EMMC: src = "emmc"; break;
	case BOOT_DEVICE_SD:   src = "sd";   break;
	case BOOT_DEVICE_USB:  src = "usb";  break;
	default:               src = "other"; break;
	}
	env_set("boot_source", src);

	if (dev == BOOT_DEVICE_USB)
		printf("Boot source: USB (RAM-loaded) — boot_check will auto-enter fastboot\n");

	detect_maskrom_failed(dev);
}

/*
 * "User held buttons 1+4 for mask-ROM but the SoC fell back to eMMC."
 * The POC nibble latched at strapping records the intent and survives the
 * fallback, so this works even if the buttons were released since.
 * Nibble values + register layout: superbird-docs/uboot/boot-flow.md.
 */
#define AO_SEC_GP_CFG0	0xff800240UL
#define POC_SHIFT	4
#define POC_MASK	0xf
#define POC_INTENT_USB	0xd
#define POC_INTENT_EMMC	0xf

static unsigned int read_poc(void)
{
	return (readl((void *)AO_SEC_GP_CFG0) >> POC_SHIFT) & POC_MASK;
}

static void detect_maskrom_failed(int boot_device)
{
	unsigned int poc = read_poc();

	if (poc == POC_INTENT_USB && boot_device != BOOT_DEVICE_USB) {
		printf("Mask-ROM USB attempt failed (POC=0x%x, fell back to "
		       "boot device %d) — boot_check will surface the help "
		       "screen.\n", poc, boot_device);
		env_set("maskrom_failed", "1");
	} else {
		env_set("maskrom_failed", "0");
	}
}

/*
 * Per-slot boot logo: an OS slot shipping /logo.bmp in its boot_<slot>
 * filesystem gets painted instead of the baked-in splash, so a slot can
 * rebrand without reflashing u-boot.
 * Requires 16/24/32bpp BMP support in the defconfig — without it the uclass
 * rejects colour BMPs. Details: superbird-docs/uboot/splash-and-backlight.md.
 */
#define CARTHING_LOGO_PATH	"/logo.bmp"
/* kernel_addr_r's region — free here, and the BMP is consumed immediately. */
#define CARTHING_LOGO_LOADADDR	0x08080000UL
/* Bounds a bogus BMP; a full-panel 800x480 32bpp image is ~1.5 MB. */
#define CARTHING_LOGO_MAX	(4 * 1024 * 1024)

/*
 * Paint a BMP, mirroring video-uclass.c show_splash() placement: full-panel
 * images corner-align at (0,0), smaller ones keep the top-left logo offset.
 */
static int carthing_paint_bmp(struct udevice *dev, u8 *data)
{
	struct video_priv *priv = dev_get_uclass_priv(dev);
	ulong bmp_w = 0, bmp_h = 0;
	uint bmp_bpix;
	int x = -4, y = 4;
	bool align = true;

	if (!(data[0] == 'B' && data[1] == 'M'))
		return -EINVAL;

	video_bmp_get_info(data, &bmp_w, &bmp_h, &bmp_bpix);
	if (bmp_w == priv->xsize && bmp_h == priv->ysize) {
		x = 0;
		y = 0;
		align = false;
	}

	return video_bmp_display(dev, map_to_sysmem(data), x, y, align);
}

/* Returns bytes read, or <=0 if the partition, filesystem or file is absent. */
static int carthing_load_slot_logo(char slot, u8 *buf)
{
	struct blk_desc *desc;
	struct disk_partition info;
	char partname[8], devpart[8];
	loff_t actread = 0;
	int part;

	desc = blk_get_dev("mmc", 0);
	if (!desc)
		return -1;

	snprintf(partname, sizeof(partname), "boot_%c", slot);
	part = part_get_info_by_name(desc, partname, &info);
	if (part < 1)
		return -1;

	/* fs_read consumes the blk dev selection, so set it each call. */
	snprintf(devpart, sizeof(devpart), "0:%d", part);
	if (fs_set_blk_dev("mmc", devpart, FS_TYPE_ANY))
		return -1;

	if (fs_read(CARTHING_LOGO_PATH, map_to_sysmem(buf), 0,
		    CARTHING_LOGO_MAX, &actread))
		return -1;

	return (int)actread;
}

/*
 * 'b' only for an exact "b", else 'a' (unset, garbage, or "a"). Shared by the
 * splash painter, the misc_init_r slot publish and ab_boot so the three can
 * never disagree.
 */
static char resolve_slot(void)
{
	const char *active = env_get("slot_active");

	return (active && active[0] == 'b' && active[1] == '\0') ? 'b' : 'a';
}

/* Active slot's /logo.bmp first, then the other, else the baked-in logo. */
static void carthing_show_splash(struct udevice *dev)
{
	char slot = resolve_slot();
	char order[2] = { slot, (slot == 'a') ? 'b' : 'a' };
	u8 *buf = (u8 *)CARTHING_LOGO_LOADADDR;
	int i, n;

	for (i = 0; i < 2; i++) {
		n = carthing_load_slot_logo(order[i], buf);
		if (n > 2 && buf[0] == 'B' && buf[1] == 'M' &&
		    !carthing_paint_bmp(dev, buf)) {
			printf("Splash: custom logo from boot_%c (%d bytes)\n",
			       order[i], n);
			return;
		}
	}

	carthing_paint_bmp(dev, video_get_u_boot_logo());
}

/*
 * Re-establish env vars that booting depends on. A saved ENV_IS_IN_FAT
 * uboot.env REPLACES the compiled-in env rather than merging with it, so a
 * stale or foreign env can arrive missing these entirely.
 *
 * Set-if-absent, so a correct saved env or a user override still wins, and
 * ab_boot stays free to rewrite `slot` per failover.
 * Why these two specifically: superbird-docs/uboot/boot-flow.md.
 */
static void carthing_guarantee_env(void)
{
	char slot_str[2];

	if (!env_get("kernel_comp_addr_r"))
		env_set("kernel_comp_addr_r", "0x0a000000");
	if (!env_get("kernel_comp_size"))
		env_set("kernel_comp_size", "0x4000000");

	if (!env_get("slot")) {
		slot_str[0] = resolve_slot();
		slot_str[1] = '\0';
		env_set("slot", slot_str);
	}
}

/*
 * Asking for a bus mode and getting it are different things: on a failed
 * switch the mmc core silently drops to a slower mode and reports success.
 * A degraded bus is a candidate for the pre-handoff bootloop, so make it
 * loud, and publish `emmc_mode` so it is readable without a UART.
 * Per-part measurements: the DT overlay + superbird-misc-notes/perf-bench.
 */
static void carthing_report_mmc_mode(void)
{
	struct mmc *mmc = find_mmc_device(0);
	const char *name;

	if (!mmc || !mmc->has_init) {
		env_set("emmc_mode", "uninitialised");
		printf("eMMC: not initialised\n");
		return;
	}

	name = mmc_mode_name(mmc->selected_mode);
	env_set("emmc_mode", name);

	if (mmc->selected_mode < MMC_HS_52) {
		printf("eMMC: WARNING - bus degraded to %s (expected %s)\n",
		       name, mmc_mode_name(MMC_HS_52));
		printf("eMMC: the mode switch failed; reads may be slow or\n");
		printf("eMMC: unreliable. Report this along with `mmc info`.\n");
	} else {
		printf("eMMC: %s\n", name);
	}
}

int misc_init_r(void)
{
	set_serial_from_efuse();
	set_boot_source();
	carthing_report_mmc_mode();
	log_charger_state();
	carthing_guarantee_env();
	g_dnl_set_product("Superbird");
	/* Enables recording into the Kconfig-allocated ring buffer, so
	 * `fastboot oem console "<cmd>"` can return output to the host. */
	console_record_reset_enable();
	/*
	 * Probing video here costs ~700 ms (the ST7701S DSI init) but leaves the
	 * panel initialised for whatever boots next. quick_boot=1 skips it —
	 * safe only if the OS does its own DRM init. See boot-flow.md.
	 */
	if (env_get_yesno("quick_boot") != 1) {
		struct udevice *dev;

		/* hide_logo BEFORE probing: the panel then syncs onto a cleared
		 * FB, so the sync transient has nothing to smear. */
		if (!uclass_find_first_device(UCLASS_VIDEO, &dev) && dev) {
			struct video_uc_plat *plat = dev_get_uclass_plat(dev);

			plat->hide_logo = true;
		}

		if (!uclass_first_device_err(UCLASS_VIDEO, &dev)) {
			mdelay(CARTHING_PANEL_SETTLE_MS);
			carthing_show_splash(dev);
		}
	}
	apply_saved_brightness();
	/* Must run BEFORE autoboot fires bootcmd — that ordering is what makes
	 * menu-button-hold work regardless of what bootcmd is set to. */
	carthing_boot_route();
	return 0;
}

/*
 * Reboot-reason stash. NOT the Amlogic-convention AO_SEC_SD_CFG15 — that is
 * SCP-owned and ANY CPU write to it hard-hangs the bus (confirmed at EL2 and
 * EL3). PREG_STICKY_REG3 is freely CPU-writable, survives the SCPI reboot,
 * and clears on a cold power cycle. Magic-tagged and one-shot so garbage is
 * never read as a reason. See superbird-docs/uboot/reboot-bootloader.md.
 */
#define CARTHING_RR_STICKY	0xff6345ccUL	/* PREG_STICKY_REG3 */
#define CARTHING_RR_MAGIC	0x5242a100U	/* "RB" tag, bits 31:8 */
#define CARTHING_RR_MAGIC_MASK	0xffffff00U

/* Carthing-local, outside the Amlogic 0..13 enum. Consumed by BL31 at reset,
 * not by carthing_boot_route. */
#define REBOOT_REASON_MASKROM	0x4d		/* 'M' */

static void carthing_set_reboot_reason(unsigned int reason)
{
	writel(CARTHING_RR_MAGIC | (reason & 0xffU), CARTHING_RR_STICKY);
}

/* Returns 0..255, or -1 if none/invalid. Clears, so the reason fires once. */
static int carthing_take_reboot_reason(void)
{
	u32 v = readl(CARTHING_RR_STICKY);

	if ((v & CARTHING_RR_MAGIC_MASK) != CARTHING_RR_MAGIC)
		return -1;
	writel(0, CARTHING_RR_STICKY);
	return (int)(v & 0xffU);
}

/*
 * Overrides the upstream default, which writes Android "bootonce-bootloader"
 * strings into a misc partition we don't have.
 */
int fastboot_set_reboot_flag(enum fastboot_reboot_reason reason)
{
	unsigned int aml_reason;

	switch (reason) {
	case FASTBOOT_REBOOT_REASON_BOOTLOADER:
		aml_reason = REBOOT_REASON_BOOTLOADER;
		break;
	case FASTBOOT_REBOOT_REASON_FASTBOOTD:
		aml_reason = REBOOT_REASON_FASTBOOT;
		break;
	case FASTBOOT_REBOOT_REASON_RECOVERY:
		aml_reason = REBOOT_REASON_RECOVERY;
		break;
	case FASTBOOT_REBOOT_REASON_MASKROM:
		aml_reason = REBOOT_REASON_MASKROM;
		break;
	default:
		return -EINVAL;
	}
	carthing_set_reboot_reason(aml_reason);
	return 0;
}

/*
 * Decides whether to intercept the boot before autoboot fires bootcmd, so an
 * env override can't bypass it. Priority order is the sequence below; the
 * reasoning behind it is in superbird-docs/uboot/boot-flow.md.
 *
 * Idempotent: none of these conditions self-clear within a boot, so a second
 * invocation (e.g. bootcmd=boot_check) would otherwise loop forever.
 */
static bool boot_route_done;

static void carthing_boot_route(void)
{
	const char *boot_source;
	const char *maskrom_failed;
	struct udevice *menu_btn;
	int rr;

	if (boot_route_done)
		return;
	boot_route_done = true;

	boot_source = env_get("boot_source");
	if (boot_source && !strcmp(boot_source, "usb")) {
		printf("Boot source: USB (RAM-loaded) — auto-entering fastboot\n");
		run_command("fastboot_with_screen", 0);
		return;
	}

	maskrom_failed = env_get("maskrom_failed");
	if (maskrom_failed && !strcmp(maskrom_failed, "1")) {
		printf("Mask-ROM USB attempt failed — opening bootmenu\n");
		run_command("bootmenu", 0);
		return;
	}

	rr = carthing_take_reboot_reason();
	if (rr == REBOOT_REASON_BOOTLOADER || rr == REBOOT_REASON_FASTBOOT) {
		printf("Auto-entering fastboot from reboot reason: %d\n", rr);
		run_command("fastboot 0", 0);
		return;
	}
	if (rr == REBOOT_REASON_RECOVERY) {
		run_command("bootmenu", 0);
		return;
	}

	if (!button_get_by_label("menu", &menu_btn) &&
	    button_get_state(menu_btn) == BUTTON_ON) {
		printf("Menu button held — opening bootmenu\n");
		run_command("bootmenu", 0);
		return;
	}
}

static int do_boot_check(struct cmd_tbl *cmdtp, int flag, int argc,
			 char *const argv[])
{
	carthing_boot_route();
	return 0;
}

U_BOOT_CMD(
	boot_check, 1, 1, do_boot_check,
	"Car Thing boot router",
	"\n"
	"  Runs the boot-routing logic (idempotent — already invoked from\n"
	"  misc_init_r before autoboot). Exposed as a command so scripts /\n"
	"  the CLI can trigger it explicitly if needed."
);

static int do_maskrom(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	printf("Rebooting into mask-ROM USB mode (SCP USB_BOOT on reset)...\n");
	carthing_set_reboot_reason(REBOOT_REASON_MASKROM);
	run_command("reset", 0);
	return 0;	/* not reached */
}

U_BOOT_CMD(
	maskrom, 1, 0, do_maskrom,
	"reboot into mask-ROM USB download mode (1b8e:c003)",
	"\n"
	"  Sets the MASKROM reboot reason and resets. BL31 sees it on the way\n"
	"  through PSCI reset and asks the SCP to drop the bootROM into USB\n"
	"  download mode — no buttons. Recover with a reset-pin reset or a cold\n"
	"  power cycle. Host-side: fastboot oem console \"maskrom\"."
);

/*
 * A/B slot selection + rollback. Runs as the normal-boot bootcmd, after
 * boot_check declines to intercept. State (slot_active, slot_<x>_tries) lives
 * in uboot.env, shared with Linux via libubootenv; superbird-slot-ok on the
 * Linux side restores the counter after a healthy boot.
 *
 * In the binary rather than an env macro so a saved uboot.env that didn't
 * carry the macro forward can't break booting (commit 7578f41b06).
 *
 * Full state machine + the both-slots-broken behaviour:
 * superbird-docs/uboot/boot-flow.md.
 */
#define AB_DEFAULT_TRIES	3

static int do_ab_boot(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	char slot, other;
	char tries_var[16], slot_str[2];
	long tries;
	char cmd[160];

	slot = resolve_slot();
	other = (slot == 'a') ? 'b' : 'a';

	/* An unset counter falls back to a full budget, not an instant failover. */
	snprintf(tries_var, sizeof(tries_var), "slot_%c_tries", slot);
	tries = (long)env_get_ulong(tries_var, 10, AB_DEFAULT_TRIES);

	if (tries <= 0) {
		printf("AB: slot %c exhausted, failing over to slot %c\n",
		       slot, other);
		slot_str[0] = other;
		slot_str[1] = '\0';
		env_set("slot_active", slot_str);
		snprintf(tries_var, sizeof(tries_var), "slot_%c_tries", other);
		env_set_ulong(tries_var, AB_DEFAULT_TRIES);
		if (env_save())
			printf("AB: WARNING: saveenv failed on failover\n");
		run_command("reset", 0);
		return 0;	/* not reached */
	}

	/* Persist BEFORE booting: a hang before the Linux slot-OK service must
	 * still count down, or a wedged slot retries forever. */
	printf("AB: booting slot %c (%ld tries left after this attempt)\n",
	       slot, tries - 1);
	env_set_ulong(tries_var, tries - 1);
	if (env_save())
		printf("AB: WARNING: saveenv failed; try counter may not "
		       "persist across a hang\n");

	/* extlinux.conf substitutes ${slot} into root_${slot}. The
	 * ${boot_partnum}/${scriptaddr} refs below expand at run time. */
	slot_str[0] = slot;
	slot_str[1] = '\0';
	env_set("slot", slot_str);

	snprintf(cmd, sizeof(cmd),
		 "part number mmc 0 boot_%c boot_partnum && "
		 "sysboot mmc 0:${boot_partnum} any ${scriptaddr} "
		 "/extlinux/extlinux.conf", slot);
	run_command(cmd, 0);

	/* sysboot only returns on failure; the decrement already persisted. */
	printf("AB: slot %c boot returned/failed, rebooting\n", slot);
	run_command("reset", 0);
	return 0;	/* not reached */
}

U_BOOT_CMD(
	ab_boot, 1, 1, do_ab_boot,
	"Car Thing A/B slot selector + rollback",
	"\n"
	"  Picks an OS slot from slot_active + per-slot try counters, burns\n"
	"  one try, saveenv, and sysboots it. A failed/returned boot reboots;\n"
	"  when a slot's tries hit 0 it flips to the other slot. Intended as\n"
	"  the normal-boot bootcmd (runs after boot_check declines)."
);

/*
 * Hook for cmd/usb_mass_storage.c: lets "back" exit the UMS loop without a
 * UART. Edge-tracked so a held button doesn't fire repeatedly.
 */
int ums_board_abort_check(void)
{
	static struct udevice *back_dev;
	static int prev;
	int now, edge;

	if (!back_dev) {
		if (button_get_by_label("back", &back_dev))
			return 0;
	}

	now = (button_get_state(back_dev) == BUTTON_ON);
	edge = now && !prev;
	prev = now;
	return edge;
}
