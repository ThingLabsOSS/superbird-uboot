// SPDX-License-Identifier: GPL-2.0+
/*
 * `hwinfo` u-boot command — companion to the bootmenu's Hardware Info
 * screen. Same detection (board rev via SARADC ch 1, charger via
 * MAX14656, temps via G12A tsensor) but printed to UART for
 * scripting / logging.
 */
#include <command.h>
#include <env.h>
#include <asm/arch/boot.h>
#include <asm/global_data.h>

#include "boardrev.h"
#include "charger.h"
#include "panel_probe.h"

int carthing_tsensor_read_pll(void);
int carthing_tsensor_read_ddr(void);

static int do_hwinfo(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	struct carthing_charger_info ch;
	const char *bv;
	u32 socinfo;
	int rev, t_pll, t_ddr;
	bool charger_ok;
	DECLARE_GLOBAL_DATA_PTR;

	printf("\n  Car Thing — Hardware Info\n");
	printf("  -------------------------\n");

	rev = carthing_probe_board_rev();
	if (rev > 0)
		printf("  Board       : Rev %d\n", rev);
	else
		printf("  Board       : unknown (SARADC ch1 lookup failed)\n");

	socinfo = meson_get_socinfo();
	printf("  SoC         : Amlogic G12A S905D2  rev %x:%x  pack %x:%x\n",
	       (socinfo >> 24) & 0xff, (socinfo >> 8) & 0xff,
	       (socinfo >> 16) & 0xff, socinfo & 0xff);

	printf("  DRAM        : %llu MiB\n",
	       (unsigned long long)gd->ram_size / (1024 * 1024));

	{
		struct carthing_panel_info panel;

		if (carthing_panel_probe(&panel) == 0 && panel.valid)
			printf("  Panel       : %s / ST7701S  (vid=%02x hw=%03x)\n",
			       panel.variant, panel.vendor_id, panel.hw_id);
		else
			printf("  Panel       : ST7701S (probe failed)\n");
	}

	charger_ok = (carthing_charger_read(&ch) == 0 && ch.valid);
	if (charger_ok) {
		printf("  Charger     : %s\n",
		       carthing_charger_type_str(ch.status));
		printf("  Charger IC  : MAX14656 rev %x\n", ch.regs[0] & 0xf);
	} else {
		printf("  Charger     : (MAX14656 not responding)\n");
	}

	t_pll = carthing_tsensor_read_pll();
	t_ddr = carthing_tsensor_read_ddr();
	if (t_pll >= 0 && t_ddr >= 0)
		printf("  Temperature : PLL %d C    DDR %d C\n", t_pll, t_ddr);
	else
		printf("  Temperature : read failed\n");

	bv = env_get("brightness");
	printf("  Backlight   : %s\n", bv ? bv : "(unset)");

	printf("  I2C bus 2   : MAX14656 (0x35),  TMD2772 prox/ALS (0x39)\n");
	printf("  I2C bus 0   : TLSC6X touch + display-detect (0x2e)\n");
	printf("  I2C bus 3   : Apple MFi auth chip (0x10, idle)\n");

	printf("\n  -- present on PCB but not driven by u-boot --\n");
	printf("    Bluetooth chip   (BCM family, SDIO + UART_AO_B)\n");
	printf("    PDM mic array    (4 mics, kernel-only)\n");
	printf("\n");

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	hwinfo, 1, 1, do_hwinfo,
	"Print Car Thing hardware inventory + live sensor readings",
	"\n"
	"  Same content as the bootmenu's Hardware Info screen, dumped to\n"
	"  UART. Includes board rev (SARADC ch1), SoC + DRAM, charger type\n"
	"  (MAX14656), on-die temperatures (PLL + DDR tsensors), I2C bus\n"
	"  inventory, and a list of peripherals known to be on the board\n"
	"  but not driven by this u-boot."
);
