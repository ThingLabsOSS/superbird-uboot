// SPDX-License-Identifier: GPL-2.0+
/*
 * `tsensor` u-boot command — read the two G12A on-die temperature
 * sensors (PLL-domain and DDR-domain).
 *
 * Ported from Spotify's vendor u-boot (spsgsb/uboot common/cmd_cpu_temp.c,
 * R1P1_TSENSOR_MODE path) which itself is from Amlogic's BSP. The
 * sensors live in the always-on power domain and use per-chip
 * calibration values stored in eFuse (re-exposed via AO secure
 * registers). Mainline u-boot has no driver for these so we just
 * touch the registers directly.
 *
 * Mainline Linux has drivers/thermal/amlogic_thermal.c which does
 * the same thing via the proper thermal framework — that's the right
 * path for a kernel port, but overkill for a u-boot one-shot read.
 */
#include <command.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/types.h>

#define TS_PLL_CFG_REG1		0xff634804	/* PLL sensor enable */
#define TS_PLL_STAT0		0xff634840	/* PLL sensor readout (low 16 bits) */
#define TS_DDR_CFG_REG1		0xff634c04	/* DDR sensor enable */
#define TS_DDR_STAT0		0xff634c40	/* DDR sensor readout (low 16 bits) */
#define HHI_TS_CLK_CNTL		0xff63c190	/* TS clock enable */
#define AO_SEC_GP_CFG10		0xff800268	/* PLL sensor cal trim (low 16 bits) */
#define AO_SEC_SD_CFG12		0xff800230	/* DDR sensor cal trim (low 16 bits) */

/* G12A / G12B / TL1 / SM1 thermal formula constants (from vendor) */
#define TS_A	9411
#define TS_B	3159
#define TS_M	424
#define TS_N	324

/*
 * Vendor's code-to-temp formula:
 *   T = 727.8 * (u_real + u_efuse / 2^16) - 274.7
 *   u_real = (5.05 * raw) / (2^16 + 4.05 * raw)
 * Integer-arithmetic implementation lifted from vendor cmd_cpu_temp.c.
 */
static int code_to_temp(u32 raw, u32 trim)
{
	s64 t;

	t = ((s64)raw * TS_M) * (1 << 16) / ((s64)100 * (1 << 16) + (s64)TS_N * raw);
	if (trim & 0x8000)
		t = ((t - (trim & 0x7fff)) * TS_A / (1 << 16) - TS_B) / 10;
	else
		t = ((t + (trim & 0x7fff)) * TS_A / (1 << 16) - TS_B) / 10;
	return (int)t;
}

int carthing_tsensor_read_pll(void);
int carthing_tsensor_read_ddr(void);

static int tsensor_read(u32 cfg_reg, u32 stat_reg, u32 trim_reg,
			u32 *out_raw, u32 *out_trim)
{
	u32 trim, val;
	u32 sum = 0;
	int cnt = 0;
	int i;

	trim = readl(trim_reg) & 0xffff;

	/* Enable thermal block + tsclk. Magic values from vendor. */
	writel(0x62b, cfg_reg);
	writel(0x130, HHI_TS_CLK_CNTL);
	mdelay(5);

	/* Drain first ~10 readings while the sensor settles. */
	for (i = 0; i < 10; i++) {
		udelay(50);
		(void)readl(stat_reg);
	}

	/* Average up to 17 readings inside the valid range. */
	for (i = 0; i <= 16; i++) {
		udelay(4500);
		val = readl(stat_reg) & 0xffff;
		if (val >= 0x1500 && val <= 0x3500) {
			sum += val;
			cnt++;
		}
	}

	if (!cnt)
		return -1;

	*out_raw = sum / cnt;
	*out_trim = trim;
	return code_to_temp(*out_raw, trim);
}

int carthing_tsensor_read_pll(void)
{
	u32 raw, trim;
	return tsensor_read(TS_PLL_CFG_REG1, TS_PLL_STAT0, AO_SEC_GP_CFG10,
			    &raw, &trim);
}

int carthing_tsensor_read_ddr(void)
{
	u32 raw, trim;
	return tsensor_read(TS_DDR_CFG_REG1, TS_DDR_STAT0, AO_SEC_SD_CFG12,
			    &raw, &trim);
}

static int do_tsensor(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	u32 raw, trim;
	int t;

	t = tsensor_read(TS_PLL_CFG_REG1, TS_PLL_STAT0, AO_SEC_GP_CFG10,
			 &raw, &trim);
	if (t < 0)
		printf("PLL tsensor: read failed (no readings in valid range)\n");
	else
		printf("PLL tsensor:  %d C  (raw=0x%04x, trim=0x%04x)\n",
		       t, raw, trim);

	t = tsensor_read(TS_DDR_CFG_REG1, TS_DDR_STAT0, AO_SEC_SD_CFG12,
			 &raw, &trim);
	if (t < 0)
		printf("DDR tsensor: read failed (no readings in valid range)\n");
	else
		printf("DDR tsensor:  %d C  (raw=0x%04x, trim=0x%04x)\n",
		       t, raw, trim);

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	tsensor, 1, 1, do_tsensor,
	"read G12A on-die temperature sensors (PLL + DDR domains)",
	"\n"
	"  Two thermal sensors live in the always-on power domain:\n"
	"    PLL  -- closer to the CPU complex / video clock area\n"
	"    DDR  -- near the DDR controller\n"
	"  Each takes ~80 ms (average of 17 readings)."
);
