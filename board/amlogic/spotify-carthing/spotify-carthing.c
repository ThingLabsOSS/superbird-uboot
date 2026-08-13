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
#include <event.h>
#include <fastboot.h>
#include <fs.h>
#include <part.h>
#include <video.h>
#include <mapmem.h>
#include <asm/arch/boot.h>
#include <asm/arch/sm.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <g_dnl.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <version_string.h>
#include <vsprintf.h>

#include "boardrev.h"
#include "carthing_mmc.h"
#include "charger.h"

DECLARE_GLOBAL_DATA_PTR;

int meson_get_boot_device(void);

/*
 * Per-unit serial number lives in the SoC's eFuse user area at offset 18,
 * 16 bytes of ASCII (NUL-padded). It's the same value adb reports as the
 * device serial and that the stock userland writes into the USB gadget's
 * serialnumber descriptor — see efuse_architecture.md for the layout.
 *
 * Reading goes via the secure-monitor SMC (BL31 owns the fuse controller
 * MMIO). If the SMC fails or the user area is blank, fall back to a
 * generic vendor string so adb/fastboot still come up.
 */
#define EFUSE_USID_OFFSET	18
#define EFUSE_USID_SIZE		16
#define EFUSE_F_SERIAL_OFFSET	34
#define EFUSE_F_SERIAL_SIZE	15
#define FALLBACK_SERIAL		"AMLG12ASPOTIFYCARTHING"

/* Trim NUL or non-printable trailing bytes. */
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

	/* Factory serial — date-coded, follows the usid. Soft-fail if it
	 * doesn't read cleanly. */
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
 * Boot-splash smear fix: light a dim glow early, sync the panel onto a black
 * FB, then paint the logo + ramp the backlight once it locks (board_init /
 * misc_init_r / apply_saved_brightness). Backlight % is inverted like
 * cmd_setbright.c (higher = dimmer), and the uclass quantizes onto the DT
 * levels so the ramp is only a few steps. Tune on-device.
 */
#define CARTHING_BOOT_GLOW		100	/* dim glow (inverted PWM: higher = dimmer) */
#define CARTHING_PANEL_SETTLE_MS	80	/* wait for the panel to lock a clean frame */
#define CARTHING_RAMP_STEP		4	/* brightness % per ramp tick */
#define CARTHING_RAMP_STEP_MS		12	/* delay between ramp ticks */

/*
 * Stock firmware fires the backlight on within ~100ms of power-on
 * (before the panel content is initialised) so the user sees the
 * screen visibly lit immediately and "the device is alive". The
 * panel garbage they're illuminating doesn't matter — it gets
 * overwritten the moment the panel init sequence finishes.
 *
 * We mirror the trick from board_init: probe UCLASS_PANEL_BACKLIGHT
 * (which cascades into pwm-meson + the BL_EN gpio + the vddao_3v3
 * regulator — a much narrower dep graph than full video / DSI bringup),
 * then call backlight_enable() + set a dim boot-glow (not the DT
 * default-brightness-level, which is hardware-max on the inverted PWM) so
 * the still-uninitialized panel is only faintly lit. The UCLASS_VIDEO probe
 * (= the ST7701S init sequence) is deferred until a consumer needs the panel
 * (currently: the bootmenu's vidconsole probe). For boots that never
 * enter bootmenu, the panel stays uninit'd but lit by the backlight.
 */
int board_init(void)
{
	struct udevice *dev;

	/* Walk the dependency chain explicitly. board_init runs *before*
	 * initr_dm_devices, so devices flagged "probe-during-bind" haven't
	 * been brought up yet — naive uclass_first_device on backlight
	 * cascade-probes them but only sometimes wins the race against
	 * deferred-init. Order matters: clock controller -> PWM -> bl. */
	uclass_first_device_err(UCLASS_CLK, &dev);
	uclass_first_device_err(UCLASS_PWM, &dev);
	if (!uclass_first_device_err(UCLASS_PANEL_BACKLIGHT, &dev)) {
		backlight_enable(dev);
		/* Dim glow, not DT-max -- hides the uninit garbage + DSI sync
		 * transient; ramped up in misc_init_r once the panel locks.
		 * The debug build wants the log legible from its first line,
		 * so it skips the glow/ramp dance and goes straight to max
		 * (inverted PWM: 0 = brightest). */
		backlight_set_brightness(dev,
					 IS_ENABLED(CONFIG_CARTHING_DEBUG_CONSOLE) ?
					 0 : CARTHING_BOOT_GLOW);
	}

	/* Debug build only: the vidconsole in stdout makes console_init_r
	 * probe UCLASS_VIDEO, which is *before* misc_init_r gets its chance
	 * to set hide_logo. board_init runs after initr_dm (devices bound,
	 * none probed), so this is the last hook early enough to stop
	 * video_post_probe() painting the baked-in logo over the boot log.
	 * uclass_find_first_device() looks up without probing, so the panel
	 * bring-up still happens later, on the console's terms. */
	if (IS_ENABLED(CONFIG_CARTHING_DEBUG_CONSOLE)) {
		struct udevice *vid;

		if (!uclass_find_first_device(UCLASS_VIDEO, &vid) && vid) {
			struct video_uc_plat *plat = dev_get_uclass_plat(vid);

			plat->hide_logo = true;
		}
	}
	return 0;
}

#if IS_ENABLED(CONFIG_CARTHING_DEBUG_CONSOLE)
/*
 * Re-force the on-screen console after the saved environment is loaded.
 *
 * STDOUT_CFG puts "serial,vidconsole" in the *compiled* env, but a unit
 * that has ever run `saveenv` (every unit with a yocto install — the A/B
 * selector saveenv's on every boot) carries stdout=serial in uboot.env,
 * and initr_env would hand exactly that to console_init_r. Since the
 * bootlooping units we want to diagnose are precisely the ones with a
 * saved env, override it here.
 *
 * EVT_SETTINGS_R is the right seam: it fires after initr_env and before
 * stdio_add_devices/console_init_r, so the vidconsole is in stdout by
 * the time iomux_doenv() goes looking for it (which is what probes
 * UCLASS_VIDEO and registers the device).
 */
#define DBG_CONSOLE_DEVS	"serial,vidconsole"

/* What stdout/stderr were before we overrode them, so a flashed build can
 * put them back around an env_save() and never persist the panel console. */
static char dbg_orig_stdout[32];
static char dbg_orig_stderr[32];

static int carthing_debug_force_console(void)
{
	const char *out = env_get("stdout");
	const char *err = env_get("stderr");

	strlcpy(dbg_orig_stdout, (out && *out) ? out : "serial",
		sizeof(dbg_orig_stdout));
	strlcpy(dbg_orig_stderr, (err && *err) ? err : "serial",
		sizeof(dbg_orig_stderr));

	/* GD_FLG_DEVINIT isn't set this early, so on_console() ignores these
	 * — no re-mux here, just the values console_init_r will read. */
	env_set("stdout", DBG_CONSOLE_DEVS);
	env_set("stderr", DBG_CONSOLE_DEVS);
	return 0;
}
EVENT_SPY_SIMPLE(EVT_SETTINGS_R, carthing_debug_force_console);

/*
 * Save the environment without persisting the debug console.
 *
 * Only the flashed build gets here (the transient one doesn't save at
 * all). It has to save for real — it *is* the bootloader, and A/B has to
 * keep working — but uboot.env must not come away with
 * stdout=serial,vidconsole, or the unit would still be printing to the
 * panel after it's flashed back to a normal image.
 *
 * So put the original values back across the save. This does re-mux the
 * live console, because GD_FLG_DEVINIT is set by now and on_console()
 * acts on the change — the handful of lines env_save() prints go to
 * serial only, then the panel comes back. Cosmetic, and worth it for a
 * clean env.
 */
static int carthing_debug_env_save(void)
{
	int ret;

	env_set("stdout", dbg_orig_stdout);
	env_set("stderr", dbg_orig_stderr);

	ret = env_save();

	env_set("stdout", DBG_CONSOLE_DEVS);
	env_set("stderr", DBG_CONSOLE_DEVS);
	return ret;
}

/*
 * Called where a normal build would reset. Never returns — both debug
 * variants stop dead with the log on screen.
 *
 * A reset is what makes a bootloop undiagnosable: the failure scrolls
 * past and the panel is wiped a second later, so nobody can photograph
 * the one screen that explains it. Freezing costs nothing, because the
 * information we want was already printed by the time we get here.
 *
 * The A/B state machine still runs normally in the flashed build — it
 * just advances one power cycle at a time instead of resetting itself.
 * The decrement or slot flip has already been persisted by the caller
 * before this point, so the next power-on picks up exactly where a
 * self-resetting build would have: fewer tries, or the other slot.
 * Rollback still works; it just asks for a power button.
 *
 * Halting *into fastboot* rather than spinning keeps the screen frozen
 * for the photo while a host, if there is one, can still attach and run
 * `debugreport`. Spin if fastboot won't start or falls out of its loop
 * — returning would let the caller reset.
 */
static void carthing_debug_stop(const char *why)
{
	printf("\n*** HALTED: %s ***\n", why);
	printf("Screen frozen deliberately — this build does not auto-reset.\n");
	printf("Take a photo of this screen. Power-cycle to continue booting\n");
	printf("(A/B state is already saved — the next boot advances).\n");

	carthing_mmc_fast_for_host();
	run_command("fastboot 0", 0);

	printf("fastboot exited — halting.\n");
	for (;;)
		mdelay(1000);
}
#else
static inline void carthing_debug_stop(const char *why) { }
#endif

/*
 * Vendor u-boot's check_charger macro periodically writes 0x8F to
 * MAX14656 register 0x09 (CONTROL 3, sets CHG_TYP_MAN=1) while
 * waiting for a "good" source. We do an opportunistic one-shot
 * redetect at boot: read the chip's status first, and only retrigger
 * if the chip's still mid-detection (CHG_DET_RUN_S, bit 6) or if it
 * reports "nothing attached" (CHG_TYP_S = 0). Helps catch
 * mis-classification on slow-rising VBUS. Returns silently if the
 * chip isn't responding.
 */
static int charger_status_settled(uint8_t status)
{
	return ((status >> 6) & 1) == 0 && (status & 0xf) != 0;
}

/*
 * Map the MAX14656 CHG_TYP_S nibble to a safe peak CPU frequency for
 * the kernel to cap scaling_max_freq at. The kernel's extlinux.conf
 * references ${charger_cap_khz} on the APPEND line, so u-boot's
 * syslinux parser substitutes whatever we set here into the cmdline
 * (kernel side: superbird-cpufreq-cap systemd oneshot reads
 * superbird.max_cpufreq_khz=N and clamps cpufreq accordingly).
 *
 * Bands come from the upstream G12A OPP table:
 *   1512000 kHz @ ~791 mV — safe on a USB-SDP-500 port (~2.5 W budget)
 *   1704000 kHz @ ~861 mV — today's previously-pinned peak, fine on CDP
 *   1800000 kHz @  981 mV — full peak, only on a real DCP / high-current
 *
 * Default is the conservative SDP cap: if we can't classify or the
 * chip isn't talking, treat it as worst-case and let the kernel
 * unclamp later if userspace knows better.
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
		/* Conservative cap even when we can't read the chip — kernel
		 * would default to 1.5 GHz on an empty value anyway, but
		 * setting it explicitly keeps /proc/cmdline self-describing. */
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

/*
 * Resolve a setbright-style level string to a backlight_set_brightness()
 * percent. The Car Thing PWM is inverted: 0 = brightest, 100 = dimmest
 * (mirrors cmd_setbright.c).
 */
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
 * Apply the saved brightness (bootmenu/`setbright`, default "med"), easing up
 * from the dim boot-glow rather than snapping. Called after the splash paints,
 * so the ramp lands on the clean logo.
 */
static void apply_saved_brightness(void)
{
	const char *v = env_get("brightness");
	struct udevice *bl;
	int target, cur, step;

	/* No saved value -> "med": DT default-brightness-level=0 is hardware-max
	 * on the inverted PWM, too bright for a fresh boot. */
	if (!v || !*v)
		v = "med";
	target = brightness_to_percent(v);

	if (uclass_first_device_err(UCLASS_PANEL_BACKLIGHT, &bl)) {
		/* Backlight lookup failed — fall back to the command path so
		 * the saved level is still honoured (just without the ramp). */
		char cmd[24];

		snprintf(cmd, sizeof(cmd), "setbright %s", v);
		run_command(cmd, 0);
		return;
	}

	if (target == BACKLIGHT_OFF) {
		backlight_set_brightness(bl, BACKLIGHT_OFF);
		return;
	}

	/* Ease from the dim boot-glow up to the target rather than snapping.
	 * The PWM is inverted (higher value = dimmer), so brightening means
	 * stepping the value DOWN toward the target. */
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

#if IS_ENABLED(CONFIG_CARTHING_DEBUG_CONSOLE)
static void carthing_debug_report(void);
#else
static inline void carthing_debug_report(void) { }
#endif

/*
 * The G12A mask ROM records the boot source in AO_SEC_GP_CFG0 bits [3:0].
 * meson_get_boot_device() (arch/arm/mach-meson/board-g12a.c) decodes that
 * into BOOT_DEVICE_EMMC (1) / SD (4) / USB (5). When we RAM-load this u-boot
 * via superbird-tool's `--burn_mode CUSTOM_FIP`, BL2 sees the mask-ROM USB
 * boot source — so any time we read BOOT_DEVICE_USB here, we know we're
 * iterating from RAM rather than executing the production boot path.
 *
 * Stash the result in env var `boot_source` (usb/sd/emmc/other). The
 * `boot_check` env macro reads this and routes USB boots straight into
 * fastboot_with_screen (so dev iterations via `--burn_mode` drop the
 * host into a fastboot session without any panel-side navigation).
 *
 * We deliberately do NOT env_set("bootcmd", ...) here. That used to be
 * the mechanism — but any env_save() that fires later in this boot
 * (e.g. apply_saved_brightness defaulting on a fresh-terraform FAT,
 * or setbright/bootmenu Settings) would persist the transient bootcmd
 * override to uboot.env, and subsequent eMMC-side power-on boots would
 * silently auto-fastboot instead of honouring the menu-button-hold
 * path. Keeping the decision in `boot_check` means transient env state
 * stays transient — boot_source is re-derived at every boot.
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
 * Detect a failed mask-ROM USB boot attempt by reading the "POC" field
 * (Power-On Configuration / boot intent) that mask-ROM latches into
 * AO_SEC_GP_CFG0[7:4] at strapping time. The value persists across
 * mask-ROM's internal fallback chain (USB-timeout → eMMC) because the
 * fallback is an internal state-machine reset, not a power cycle, so
 * the AO domain stays alive throughout. Confirmed empirically:
 *
 *   POC = 0xD  ↔  buttons 1+4 held at strapping  ↔  USB boot intent
 *   POC = 0xF  ↔  no/wrong buttons               ↔  normal eMMC intent
 *
 * Layout of CFG0 low-byte (verified by reading both BL2's UART banner
 * and the register after our u-boot starts):
 *
 *   bits  layout    e.g. 0x020004d1  e.g. 0x020004f1
 *   [3:0] boot dev  1 = EMMC          1 = EMMC
 *   [7:4] POC       0xD = USB intent  0xF = eMMC intent
 *
 * So "user wanted mask-ROM USB but didn't get it" =
 *   (POC == 0xD) && (boot_source == EMMC)
 *
 * This is button-state-independent: even if the user releases the
 * buttons during the ~10 seconds between reset and our u-boot reading
 * AO, the latched POC value tells us their original intent.
 *
 * The detection runs once at misc_init_r time and sets env var
 * "maskrom_failed=1" if it fires. The `boot_check` env macro reads it
 * and routes into the bootmenu so the help screen can render — kept
 * out of C-side env_set("bootcmd", ...) so a later env_save can't
 * persist a transient bootmenu override into uboot.env.
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
 * Custom boot logo from the OS partitions.
 *
 * By default u-boot's video uclass auto-paints a baked-in splash (the
 * BMP linked in as __splash_u_boot_logo_begin) during the UCLASS_VIDEO
 * probe. We override that: if an OS slot ships its own /logo.bmp in its
 * boot_<slot> filesystem, paint that instead — so a slot can carry its
 * own branding without reflashing u-boot. Falls back to the baked-in
 * logo when neither slot has a valid one.
 *
 * Slot order: the active slot (slot_active, default 'a' — same rule as
 * do_ab_boot) is tried first, then the other slot. First valid BMP wins.
 * This runs from the misc_init_r video probe, BEFORE ab_boot picks a
 * slot, but slot_active is just an env var so we resolve it ourselves.
 * Cost is one eMMC + FS read (~tens of ms for a few-hundred-KB BMP).
 *
 * 16/24/32bpp BMP support is enabled in the defconfig so a normal
 * full-colour BMP works; without those the uclass only renders 8bpp
 * palette images and a colour logo would be rejected by
 * video_bmp_display(). Skipped entirely under quick_boot=1 (the panel
 * isn't probed on that path).
 */
#define CARTHING_LOGO_PATH	"/logo.bmp"
/*
 * Scratch load address for the BMP. The kernel_addr_r region is free at
 * misc_init_r time (no kernel loaded yet) and video_bmp_display() copies
 * the pixels straight into the framebuffer, so this buffer is transient.
 */
#define CARTHING_LOGO_LOADADDR	0x08080000UL
/* Cap the read so a bogus oversized logo.bmp can't run off into DRAM.
 * A full-panel 800x480 32bpp BMP is ~1.5 MB; 4 MB is comfortable slack. */
#define CARTHING_LOGO_MAX	(4 * 1024 * 1024)

/*
 * Paint a BMP at `data`, mirroring video-uclass.c show_splash(): a
 * full-panel image is drawn corner-aligned at (0,0); anything smaller
 * keeps the default top-left logo placement. Returns 0 if painted.
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

/*
 * Load CARTHING_LOGO_PATH from boot_<slot>'s filesystem into `buf`.
 * Returns the byte count on success, <=0 if the partition, filesystem
 * or file is absent.
 */
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
 * Resolve the active OS slot from the slot_active env var: 'b' only for
 * an exact "b", otherwise 'a' (covers unset, garbage, or "a"). Single
 * source of truth shared by the splash painter, the unbypassable slot
 * publish in misc_init_r, and the A/B selector — so they can never
 * disagree about which slot is current.
 */
static char resolve_slot(void)
{
	const char *active = env_get("slot_active");

	return (active && active[0] == 'b' && active[1] == '\0') ? 'b' : 'a';
}

/*
 * Paint the boot splash: prefer a partition-supplied /logo.bmp (active
 * slot first, then the other), else the baked-in logo.
 */
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

	/* No usable partition logo — paint the baked-in one. */
	carthing_paint_bmp(dev, video_get_u_boot_logo());
}

/*
 * Guarantee env vars that booting depends on but that a saved
 * CONFIG_ENV_IS_IN_FAT uboot.env replaces wholesale (a saved env
 * overrides the compiled-in environment rather than merging with
 * CFG_EXTRA_ENV_SETTINGS). A stale, sparse, or foreign saved env can
 * therefore arrive missing these — so we re-establish them here, on the
 * unbypassable misc_init_r path, the same way set_boot_source() and
 * set_serial_from_efuse() force their own state regardless of the env.
 *
 * Set-if-absent only: a correct saved env or an explicit user override
 * still wins, and the A/B selector (do_ab_boot) stays free to rewrite
 * `slot` per failover when it actually runs.
 *
 *  - kernel_comp_addr_r / kernel_comp_size: the decompression buffer for
 *    a gzipped kernel (extlinux `KERNEL Image.gz`). Defined nowhere in
 *    our compiled env — spotify-carthing.h's CFG_EXTRA_ENV_SETTINGS
 *    override drops the meson64.h defaults — so a foreign env lacking
 *    them dies with "kernel_comp_addr_r or kernel_comp_size is not
 *    provided!". Values match the known-good wic env; the 0x0a000000..
 *    0x0e000000 buffer sits between kernel_addr_r (0x08080000, holding
 *    the compressed Image.gz) and ramdisk_addr_r (0x13000000) on the
 *    512 MiB part — no collision.
 *  - slot: extlinux.conf substitutes ${slot} into
 *    root=PARTLABEL=root_${slot}. do_ab_boot publishes it on the normal
 *    path, but a foreign bootcmd that never runs ab_boot would otherwise
 *    leave ${slot} empty (root=PARTLABEL=root_). Publish the resolved
 *    slot up front so it's never empty regardless of which bootcmd fires.
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

int misc_init_r(void)
{
	set_serial_from_efuse();
	set_boot_source();
	log_charger_state();
	carthing_guarantee_env();
	/* g_dnl's default product string is "USB download gadget"; replace
	 * with the project name. Manufacturer is set at compile time via
	 * CONFIG_USB_GADGET_MANUFACTURER. */
	g_dnl_set_product("Superbird");
	/* Start capturing console output so `fastboot oem console "<cmd>"`
	 * can return the cmd's output to the host. Kconfig allocates the
	 * 128 KiB ring buffer; this enables actual recording. */
	console_record_reset_enable();
	/* Probe UCLASS_VIDEO so the panel is fully initialized + the splash
	 * paints + vidconsole is ready before autoboot. Costs ~700 ms (the
	 * ST7701S DSI init sequence) on the reset→autoboot path. On by
	 * default so an average user sees a normal boot.
	 *
	 * Set env `quick_boot=1` to skip — the bootmenu's own UCLASS_VIDEO
	 * probe still cascade-inits the panel on demand when the menu
	 * enters. But the OS that follows u-boot will inherit an
	 * uninitialized panel: fine if the OS does its own DRM init
	 * (mainline Linux), broken if the OS just adopts u-boot's state
	 * (e.g. the stock Spotify rootfs takes the framebuffer as-is).
	 *
	 * Backlight is kicked on early in board_init() regardless, so the
	 * device always looks "alive" within ~100 ms of power-on. */
	/* Debug build: the panel is already up (console_init_r probed it to
	 * get the vidconsole) and the screen is the log, so there's no
	 * splash to paint and no glow to ramp out of. Just dump the boot
	 * report before the router can hand off. */
	if (IS_ENABLED(CONFIG_CARTHING_DEBUG_CONSOLE)) {
		carthing_debug_report();
	} else {
		if (env_get_yesno("quick_boot") != 1) {
			struct udevice *dev;

			/* Set hide_logo before probing so video_post_probe() skips
			 * its auto-splash and the panel syncs onto the cleared
			 * (black) FB -- nothing on screen for the sync transient to
			 * smear. */
			if (!uclass_find_first_device(UCLASS_VIDEO, &dev) && dev) {
				struct video_uc_plat *plat =
					dev_get_uclass_plat(dev);

				plat->hide_logo = true;
			}

			if (!uclass_first_device_err(UCLASS_VIDEO, &dev)) {
				/* Let the panel lock a clean (black) frame, then
				 * paint the logo (slot /logo.bmp, else baked-in). */
				mdelay(CARTHING_PANEL_SETTLE_MS);
				carthing_show_splash(dev);
			}
		}
		/* Ramp from the dim boot-glow up to med/saved, now on a clean
		 * logo. */
		apply_saved_brightness();
	}
	/* Run the boot router BEFORE autoboot fires bootcmd. This is the
	 * "menu-button-hold always works" guarantee — no matter what
	 * bootcmd is set to (saved env, flashed image, user override),
	 * holding the menu button at boot opens the bootmenu. Same goes
	 * for the boot_source=usb / maskrom_failed / reboot_reason
	 * routing. See carthing_boot_route() for the full priority list. */
	carthing_boot_route();
	return 0;
}

/*
 * Reboot-reason stash — PREG_STICKY_REG3 (0xff6345cc, PERIPHS block).
 *
 * `fastboot reboot bootloader` needs the reason to survive the reset so the
 * next boot lands back in fastboot. The Amlogic convention is AO_SEC_SD_CFG15
 * (0xff80023c), but that register is owned by the SCP and ANY CPU write to it
 * hard-hangs the bus — confirmed on hardware at EL2 and EL3, both from a live
 * SMC and from the BL31 reset path. Setting it the vendor way needs an
 * undocumented SCPI command baked into the closed SCP core.
 *
 * So we use a PREG_STICKY scratch register instead. Verified on hardware:
 * (a) freely CPU-writable — no SCP, no SMC, no hang; and (b) survives the
 * SCPI reboot intact (a cold power cycle clears it, which is correct: cold
 * boot => normal). Tagged with a magic so stale/garbage isn't taken for a
 * real reason, and one-shot cleared on read. Being a plain non-secure
 * register, Linux can write it too for a real `reboot bootloader`.
 */
#define CARTHING_RR_STICKY	0xff6345ccUL	/* PREG_STICKY_REG3 */
#define CARTHING_RR_MAGIC	0x5242a100U	/* "RB" tag, bits 31:8 */
#define CARTHING_RR_MAGIC_MASK	0xffffff00U

/*
 * Carthing-local reboot reason (outside the Amlogic 0..13 enum). Unlike the
 * others, this one is consumed by *BL31* at reset time — its g12a_system_reset
 * sees the reason, asks the SCP for USB_BOOT, and the software reset then lands
 * in mask-ROM USB mode (1b8e:c003). u-boot never routes on it.
 */
#define REBOOT_REASON_MASKROM	0x4d		/* 'M' */

static void carthing_set_reboot_reason(unsigned int reason)
{
	writel(CARTHING_RR_MAGIC | (reason & 0xffU), CARTHING_RR_STICKY);
}

/*
 * Consume the stashed reason: returns 0..255, or -1 if none/invalid.
 * One-shot — clears the register so the reason fires exactly once.
 */
static int carthing_take_reboot_reason(void)
{
	u32 v = readl(CARTHING_RR_STICKY);

	if ((v & CARTHING_RR_MAGIC_MASK) != CARTHING_RR_MAGIC)
		return -1;
	writel(0, CARTHING_RR_STICKY);
	return (int)(v & 0xffU);
}

#if IS_ENABLED(CONFIG_CARTHING_DEBUG_CONSOLE)
/*
 * Non-consuming variant for the debug report — carthing_boot_route()
 * still needs to consume the reason itself a moment later.
 */
static int carthing_peek_reboot_reason(void)
{
	u32 v = readl(CARTHING_RR_STICKY);

	if ((v & CARTHING_RR_MAGIC_MASK) != CARTHING_RR_MAGIC)
		return -1;
	return (int)(v & 0xffU);
}

static const char *envstr(const char *name)
{
	const char *v = env_get(name);

	return (v && *v) ? v : "<unset>";
}

/*
 * Report whether a slot actually has something to boot.
 *
 * ab_boot hands `sysboot ... /extlinux/extlinux.conf` to the parser and
 * treats any return as a failed attempt, so a slot whose boot partition
 * is missing, unformatted, or half-written by an interrupted OTA loops
 * pre-handoff and looks identical to a slot whose kernel crashes. These
 * two files are the invariants sysboot needs; the FDT is named by
 * extlinux.conf itself so it can't be checked generically here.
 */
static void carthing_debug_slot_files(char slot)
{
	static const char * const files[] = {
		"/extlinux/extlinux.conf",
		"/extlinux/Image",
	};
	struct blk_desc *desc = blk_get_dev("mmc", 0);
	struct disk_partition info;
	char partname[8], devpart[8];
	int part, i;

	snprintf(partname, sizeof(partname), "boot_%c", slot);
	if (!desc) {
		printf("boot_%c: no mmc 0\n", slot);
		return;
	}
	part = part_get_info_by_name(desc, partname, &info);
	if (part < 1) {
		printf("boot_%c: NOT IN PARTITION TABLE\n", slot);
		return;
	}

	snprintf(devpart, sizeof(devpart), "0:%d", part);
	printf("boot_%c=part%d:", slot, part);
	for (i = 0; i < ARRAY_SIZE(files); i++) {
		/* fs_exists() consumes the blk dev selection, so re-set it. */
		bool ok = !fs_set_blk_dev("mmc", devpart, FS_TYPE_ANY) &&
			  fs_exists(files[i]);

		printf(" %s=%s", files[i], ok ? "ok" : "MISSING");
	}
	printf("\n");
}

/*
 * The diagnostic payload: everything you'd ask a user with a bootlooping
 * unit for if you had a serial console, printed to the panel before the
 * boot router gets a chance to hand off.
 *
 * Ordered most-to-least likely to explain a loop. The A/B block is first
 * for a reason — the documented degenerate case of the slot selector
 * (both slots failing, each flip refilling the other's budget) is an
 * a<->b oscillation that looks exactly like a bootloop from outside, and
 * the try counters are the only way to tell it apart from a u-boot-side
 * crash or a kernel that resets after handoff.
 */
static void carthing_debug_report(void)
{
	int rr = carthing_peek_reboot_reason();
	int rev = carthing_probe_board_rev();

	printf("\n===== Car Thing debug build =====\n");
	printf("%s\n", version_string);

	printf("-- A/B state --\n");
	printf("slot_active=%s  a_tries=%s  b_tries=%s\n",
	       envstr("slot_active"), envstr("slot_a_tries"),
	       envstr("slot_b_tries"));
	printf("bootcmd=%s\n", envstr("bootcmd"));

	printf("-- boot path --\n");
	printf("boot device=%d  boot_source=%s  maskrom_failed=%s\n",
	       meson_get_boot_device(), envstr("boot_source"),
	       envstr("maskrom_failed"));
	if (rr < 0)
		printf("reboot reason=<none> (cold boot or already consumed)\n");
	else
		printf("reboot reason=0x%02x\n", rr);

	printf("-- environment --\n");
	printf("source=%s\n", gd->env_valid == ENV_VALID ?
	       "uboot.env (saved)" : "built-in defaults");
	printf("scriptaddr=%s  kernel_comp_addr_r=%s\n",
	       envstr("scriptaddr"), envstr("kernel_comp_addr_r"));

	printf("-- hardware --\n");
	printf("board rev=%d  serial#=%s\n", rev, envstr("serial#"));
	/* The negotiated mode, not the requested one — a marginal card or a
	 * failed pin both show up here rather than being assumed away. */
	printf("eMMC bus mode=%s\n", carthing_mmc_current_mode());

	printf("-- boot artifacts --\n");
	carthing_debug_slot_files('a');
	carthing_debug_slot_files('b');

	printf("-- partitions --\n");
	if (run_command("part list mmc 0", 0))
		printf("part list FAILED — no GPT on mmc 0?\n");
	printf("=================================\n\n");
}

static int do_debugreport(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	carthing_debug_report();
	return 0;
}

U_BOOT_CMD(
	debugreport, 1, 1, do_debugreport,
	"re-print the Car Thing boot diagnostic report",
	"\n"
	"  Same report misc_init_r prints at boot. Useful once the boot log\n"
	"  has scrolled, or over `fastboot oem console \"debugreport\"`."
);
#endif /* CONFIG_CARTHING_DEBUG_CONSOLE */

/*
 * Override the upstream default fastboot_set_reboot_flag (which writes
 * Android "bootonce-bootloader" strings into a misc partition we don't have).
 * Stash the reason in PREG_STICKY_REG3; carthing_boot_route reads it on the
 * next boot and routes BOOTLOADER/FASTBOOT into fastboot.
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
		/* `fastboot oem maskrom` — consumed by BL31 at the PSCI reset
		 * (SCP USB_BOOT), not by carthing_boot_route on next boot. */
		aml_reason = REBOOT_REASON_MASKROM;
		break;
	default:
		return -EINVAL;
	}
	carthing_set_reboot_reason(aml_reason);
	return 0;
}

/*
 * Boot routing — runs unconditionally from misc_init_r (and also
 * exposed as the `boot_check` u-boot command for CLI/scripts). The
 * misc_init_r call happens *before* autoboot fires bootcmd, so the
 * routing can't be bypassed by an env override on `bootcmd`.
 *
 * Priority order:
 *  1. boot_source=usb (set by set_boot_source — we were RAM-loaded via
 *     mask-ROM USB / `--burn_mode CUSTOM_FIP`). Auto-enter fastboot
 *     with the on-panel splash so dev iterations drop the host into a
 *     fastboot session.
 *  2. maskrom_failed=1 (set by detect_maskrom_failed — POC intent was
 *     USB but the SoC fell back to eMMC). Open the bootmenu so it can
 *     render the help screen.
 *  3. Reboot reason is "bootloader" or "fastboot" (set by our
 *     fastboot_set_reboot_flag for `fastboot reboot bootloader` /
 *     `fastboot reboot fastboot`, stashed in PREG_STICKY_REG3 which
 *     survives the reset; see carthing_set_reboot_reason). Consumed
 *     one-shot here. Auto-enter fastboot.
 *  4. Reboot reason is "recovery" — drop to bootmenu (until we have an
 *     actual recovery flow).
 *  5. User is holding the menu button — drop to bootmenu. This is the
 *     "always works no matter what bootcmd is" entry point.
 *  6. Otherwise: return cleanly. autoboot fires whatever bootcmd is
 *     set to (extlinux, etc.).
 *
 * Idempotent within a single boot via the static `route_done` flag —
 * any second invocation (e.g. from a `bootcmd=boot_check` calling the
 * U_BOOT_CMD wrapper) no-ops. Required because the conditions above
 * (especially maskrom_failed, reboot_reason) don't self-clear within
 * one boot, so re-running would loop the user back into bootmenu /
 * fastboot every time.
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
		/* The debug build stays on the plain fastboot command: the
		 * FASTBOOT splash would paint over the boot report we just
		 * printed, and the report is the whole reason a RAM-loaded
		 * debug image exists. Note this branch means a RAM-loaded
		 * image never reaches ab_boot on its own — to exercise the
		 * real boot path (and see it fail) drive it from the host
		 * with `fastboot oem console "ab_boot"`. */
		if (IS_ENABLED(CONFIG_CARTHING_DEBUG_CONSOLE)) {
			printf("Debug build: host can run the real boot path with\n"
			       "  fastboot oem console \"ab_boot\"\n");
			carthing_mmc_fast_for_host();
			run_command("fastboot 0", 0);
		} else {
			run_command("fastboot_with_screen", 0);
		}
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

/*
 * `maskrom` — reboot into mask-ROM USB download mode (1b8e:c003), no buttons.
 *
 * Stashes the MASKROM reboot reason and resets. The reset is a PSCI software
 * reset, so BL31's g12a_system_reset() runs with the SCP still powered: it
 * sees the reason and issues a SCPI USB_BOOT to the SCP, which re-arms the
 * bootROM's USB-first window. Net effect: the device comes back up in mask-ROM
 * USB mode instead of booting. Recovery is any hardware reset-pin reset (it
 * resets the SCP, clearing the request) or a cold power cycle.
 *
 * Reachable from the host via `fastboot oem console "maskrom"`.
 */
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
 * A/B slot selection + rollback state machine (Task 4).
 *
 * Runs as the normal-boot bootcmd — i.e. *after* boot_check
 * (carthing_boot_route in misc_init_r) has declined to intercept for
 * fastboot / bootmenu / recovery. Picks an OS slot, burns one try
 * against it, persists that to uboot.env, then hands off to the extlinux
 * sysboot. The decision lives in the binary (not an env macro) for the
 * same reason boot_check does: a saved uboot.env that didn't carry the
 * macro forward can't break booting (see commit 7578f41b06).
 *
 * State (all in uboot.env, shared with Linux via libubootenv):
 *   slot_active   which slot is current ("a"/"b"). Linux flips it on OTA.
 *   slot_a_tries  attempts remaining for slot a.
 *   slot_b_tries  attempts remaining for slot b.
 *   slot          (output) the slot picked this boot; extlinux.conf
 *                 substitutes ${slot} into root=PARTLABEL=root_${slot}
 *                 and superbird.slot=${slot}.
 *
 * Per boot:
 *   1. slot  := slot_active           (default "a" if unset/garbage)
 *      tries := slot_<slot>_tries      (default 3 if unset)
 *   2. tries <= 0 → active slot exhausted: flip slot_active to the other
 *      slot, give it a fresh budget of 3, saveenv, reset. The selector
 *      re-runs next boot against the new active slot.
 *   3. tries > 0  → burn one attempt up front (tries-1) and saveenv
 *      *before* booting, so a kernel that hangs before the Linux
 *      slot-OK service runs still counts against this slot. Publish
 *      `slot`, resolve the boot partition, sysboot.
 *   4. sysboot only returns if the boot FAILED. Treat the return as a
 *      failed attempt and reset; the selector re-runs with a lower try
 *      count and eventually flips.
 *
 * The Linux side closes the loop: superbird-slot-ok resets
 * slot_<booted>_tries to 3 after ~60 s of stable uptime, so a slot only
 * counts down when a boot doesn't stay healthy.
 *
 * Both-slots-broken degrades to an a<->b oscillation (each flip resets
 * the target to 3) rather than a hard hang — an acceptable soft-brick
 * for v1 that keeps retrying. A bounded variant could count flips in a
 * slot_flip_count env var and drop to fastboot after N cycles, but that
 * needs the Linux slot-OK service to also zero slot_flip_count on a
 * healthy boot, so it's left out until that's wired on the yocto side.
 */
#define AB_DEFAULT_TRIES	3

/*
 * env_save() for the A/B selector, which behaves differently depending
 * on how the running image got here.
 *
 * Transient debug image (RAM-loaded or chainloaded): don't save at all.
 * A diagnostic that visits a unit must not write to it. Two ways the
 * save bit us on the bench: it burns a try against the slot — so merely
 * *looking* at a unit nudged it toward a failover and corrupted the
 * evidence we came to read — and it persisted stdout=serial,vidconsole
 * into uboot.env, leaving the unit printing to the panel long after it
 * was back on a normal image.
 *
 * Flashed debug image: save for real, because it *is* the bootloader
 * and A/B has to keep working — but route through the wrapper that
 * keeps the debug console out of the saved env.
 *
 * Normal build: plain env_save().
 *
 * The in-RAM env_set()s are left alone in every case: the boot still
 * needs `slot` published for extlinux.conf, and the report reads the
 * counters before any of this runs.
 */
static int carthing_env_save(void)
{
#if IS_ENABLED(CONFIG_CARTHING_DEBUG_CONSOLE)
	if (IS_ENABLED(CONFIG_CARTHING_DEBUG_TRANSIENT)) {
		printf("AB: transient debug build — NOT saving env "
		       "(unit left untouched)\n");
		return 0;
	}
	return carthing_debug_env_save();
#else
	return env_save();
#endif
}

static int do_ab_boot(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	char slot, other;
	char tries_var[16], slot_str[2];
	long tries;
	char cmd[160];

	/* 1. Pick the active slot via resolve_slot() (shared with the splash
	 *    painter and misc_init_r's slot publish): 'b' only for an exact
	 *    "b", else 'a' — so an unset or garbage slot_active can't fail
	 *    into an empty boot_ label. */
	slot = resolve_slot();
	other = (slot == 'a') ? 'b' : 'a';

	/* 2. Read this slot's remaining tries. env_get_ulong returns the
	 *    default for an unset var, so a missing counter falls back to a
	 *    full budget rather than instantly failing over. */
	snprintf(tries_var, sizeof(tries_var), "slot_%c_tries", slot);
	tries = (long)env_get_ulong(tries_var, 10, AB_DEFAULT_TRIES);

	if (tries <= 0) {
		/* Active slot exhausted — fail over: make the other slot
		 * active with a fresh budget, then reboot so the selector
		 * re-runs against it. */
		printf("AB: slot %c exhausted, failing over to slot %c\n",
		       slot, other);
		slot_str[0] = other;
		slot_str[1] = '\0';
		env_set("slot_active", slot_str);
		snprintf(tries_var, sizeof(tries_var), "slot_%c_tries", other);
		env_set_ulong(tries_var, AB_DEFAULT_TRIES);
		if (carthing_env_save())
			printf("AB: WARNING: saveenv failed on failover\n");
		carthing_debug_stop("A/B failover — slot exhausted");
		run_command("reset", 0);
		return 0;	/* not reached */
	}

	/* 3. Burn one attempt and persist it BEFORE booting, so a hang or
	 *    crash before the Linux slot-OK service still counts down. */
	printf("AB: booting slot %c (%ld tries left after this attempt)\n",
	       slot, tries - 1);
	env_set_ulong(tries_var, tries - 1);
	if (carthing_env_save())
		printf("AB: WARNING: saveenv failed; try counter may not "
		       "persist across a hang\n");

	/* 4. Publish the chosen slot so extlinux.conf substitutes
	 *    root_${slot} / superbird.slot=${slot}, then hand off. The
	 *    ${boot_partnum}/${scriptaddr} refs are expanded by the parser
	 *    at run time — boot_partnum is set by `part number`, scriptaddr
	 *    comes from uboot.env. */
	slot_str[0] = slot;
	slot_str[1] = '\0';
	env_set("slot", slot_str);

	snprintf(cmd, sizeof(cmd),
		 "part number mmc 0 boot_%c boot_partnum && "
		 "sysboot mmc 0:${boot_partnum} any ${scriptaddr} "
		 "/extlinux/extlinux.conf", slot);
	run_command(cmd, 0);

	/* sysboot only returns on failure. The decrement above already
	 * persisted, so just reboot — the selector re-runs with a lower
	 * try count and eventually flips. */
	printf("AB: slot %c boot returned/failed, rebooting\n", slot);
	carthing_debug_stop("boot failed — sysboot returned");
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
 * Hook into cmd/usb_mass_storage.c so the user can exit the UMS loop
 * without UART. Pressing "back" stops UMS cleanly.
 *
 * Polled once per USB-MS handle cycle; the back button is edge-tracked
 * (released-to-pressed) so a held button doesn't fire repeatedly.
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
