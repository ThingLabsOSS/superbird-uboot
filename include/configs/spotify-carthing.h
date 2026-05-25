/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Configuration header for the Spotify Car Thing.
 */

#ifndef __SPOTIFY_CARTHING_CONFIG_H
#define __SPOTIFY_CARTHING_CONFIG_H

#include <configs/meson64.h>

/*
 * Override meson64.h defaults: serial-only stdout by default.
 * The splash logo renders independently via show_splash() during video
 * uclass probe; subsequent text output (boot messages, command prompt,
 * etc.) goes only to UART so the panel stays a clean splash. To bring
 * up the on-screen console when needed (boot failure, menu entry,
 * debug), `run show_console` from u-boot CLI or a boot script.
 */
#undef STDOUT_CFG
#define STDOUT_CFG "serial"
#undef STDIN_CFG
#define STDIN_CFG "serial"

#undef CFG_EXTRA_ENV_SETTINGS
#define CFG_EXTRA_ENV_SETTINGS \
	"stdin=" STDIN_CFG "\0" \
	"stdout=" STDOUT_CFG "\0" \
	"stderr=" STDOUT_CFG "\0" \
	/* Compressed-kernel decompression buffer. meson64.h defines these,  \
	 * but our CFG_EXTRA_ENV_SETTINGS override above drops them; without  \
	 * them a gzipped extlinux KERNEL fails with "kernel_comp_addr_r or   \
	 * kernel_comp_size is not provided!". misc_init_r also force-sets     \
	 * these if a saved uboot.env lacks them — this carries them in the   \
	 * default (no-saved-env) environment too. Buffer 0x0a000000.. sits   \
	 * between kernel_addr_r and ramdisk_addr_r on the 512 MiB part. */    \
	"kernel_comp_addr_r=0x0a000000\0" \
	"kernel_comp_size=0x4000000\0" \
	/* Boot scratch/load addresses. meson64.h defines these, but the      \
	 * CFG_EXTRA_ENV_SETTINGS override above drops them, so the compiled   \
	 * default (no-saved-env) env lacks them — a blank/reset env then      \
	 * can't sysboot (scriptaddr) or booti (kernel/fdt/ramdisk). Restore   \
	 * by macro (no magic numbers, no drift). Deliberately NOT pulling in  \
	 * BOOTENV / dfu_alt_info / fdtfile — the board uses ab_boot/boot_check,\
	 * not distro_bootcmd, and extlinux.conf names its own FDT. */         \
	"kernel_addr_r="     KERNEL_ADDR_R     "\0" \
	"fdt_addr_r="        FDT_ADDR_R        "\0" \
	"scriptaddr="        SCRIPT_ADDR_R     "\0" \
	"pxefile_addr_r="    PXEFILE_ADDR_R    "\0" \
	"fdtoverlay_addr_r=" FDTOVERLAY_ADDR_R "\0" \
	"ramdisk_addr_r="    RAMDISK_ADDR_R    "\0" \
	"show_console=setenv stdout serial,vidconsole;" \
	             "setenv stderr serial,vidconsole;" \
	             "cls\0" \
	"hide_console=setenv stdout serial;" \
	             "setenv stderr serial\0" \
	/* selfflash: end-to-end u-boot self-update via fastboot+mmc.\n     \
	 * Run from u-boot CLI: `run selfflash`. Then on host:\n            \
	 *   sudo fastboot stage <2_097_152-byte boot partition image>\n    \
	 *   sudo fastboot continue\n                                       \
	 * The script then writes 4096 sectors (info_sector + BL2 + FIP)\n  \
	 * to both boot0 (hwpart 1) and boot1 (hwpart 2) and reports done.\n\
	 * Image is sized to fit the 2 MiB boot-partition variant\n         \
	 * (BOOT_SIZE_MULT=16); the 4 MiB variant accommodates it too.\n    \
	 * Image format: same as flash_boot_partition.py produces (see\n    \
	 * superbird-fip-tools). */                                         \
	"selfflash=echo Waiting for fastboot upload on USB-C...;" \
	          "fastboot 0;" \
	          "echo Writing boot0 (hwpart 1)...;" \
	          "mmc dev 0 1;" \
	          "mmc write 0x6000000 0 0x1000;" \
	          "echo Writing boot1 (hwpart 2)...;" \
	          "mmc dev 0 2;" \
	          "mmc write 0x6000000 0 0x1000;" \
	          "mmc dev 0;" \
	          "echo Done. Run reset to boot the new image.\0"

/*
 * Default bootcmd: the in-C `ab_boot` A/B slot selector (see
 * spotify-carthing.c). The boot router (`boot_check` / carthing_boot_route)
 * still runs unconditionally from misc_init_r *before* autoboot, so the
 * fastboot / bootmenu / recovery routing happens regardless of bootcmd;
 * ab_boot is only reached on the normal-boot fall-through. Both ab_boot
 * and boot_check are in-C rather than CFG_-time env macros so a saved
 * uboot.env that doesn't carry them forward can't break booting (see
 * commit 7578f41b06).
 *
 * The yocto default uboot.env should also set `bootcmd=ab_boot` (it
 * currently ships a static part+sysboot bootcmd) so saved-env units pick
 * up A/B selection; ab_boot reads slot_active / slot_<x>_tries from that
 * same env. scriptaddr must be present in the env (yocto supplies it).
 */
#undef CONFIG_BOOTCOMMAND
#define CONFIG_BOOTCOMMAND "ab_boot"

#endif /* __SPOTIFY_CARTHING_CONFIG_H */
