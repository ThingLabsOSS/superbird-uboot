// SPDX-License-Identifier: GPL-2.0+
/*
 * Identify the LCD panel variant (BOE / Wily / Holitech ST7701S 480x800) from
 * the TLSC6X touch controller's config NVM, which carries a vendor_id and
 * chip_code for the touch+panel module the unit was built with.
 *
 * Ported from vendor u-boot's sp_probe_display_stack
 * (include/spotify/hw_probe.h). Touch controller is I2C 0x2e on i2c0.
 */
#include <dm.h>
#include <dm/uclass.h>
#include <i2c.h>
#include <linux/delay.h>
#include <linux/string.h>

#include "panel_probe.h"

#define TOUCH_I2C_BUS		0	/* alias i2c0 */
#define TOUCH_I2C_ADDR		0x2e

#define TOUCH_DMA_REG		0x42bd
#define TOUCH_MODE_REG		0x0001
#define TOUCH_MCCODE_REG	0x8000
#define TOUCH_MCCODE_ALT_REG	0x0009
#define TOUCH_CONFIG_0_REG	0xd6e0
#define TOUCH_CONFIG_1_REG	0x9e00

static const uint8_t dma_enable_cmd[6] = { 0x28, 0x35, 0xc1, 0x00, 0x35, 0xae };

static int panel_get_chip(struct udevice **chip)
{
	struct udevice *bus;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_I2C, TOUCH_I2C_BUS, &bus);
	if (ret)
		return ret;
	ret = i2c_get_chip(bus, TOUCH_I2C_ADDR, 2, chip);
	if (ret)
		return ret;
	/* Force 2-byte sub-address — touch chip's register space is
	 * 16-bit-addressed. */
	return i2c_set_chip_offset_len(*chip, 2);
}

static int panel_enable_dma(struct udevice *chip)
{
	uint8_t reply[3];
	int i;

	for (i = 0; i < 5; i++) {
		if (dm_i2c_write(chip, TOUCH_DMA_REG,
				 dma_enable_cmd, sizeof(dma_enable_cmd)))
			continue;
		mdelay(50);
		if (dm_i2c_read(chip, TOUCH_MODE_REG, reply, sizeof(reply)))
			continue;
		/* Vendor check: reply[0] >> 1 == 0x2e (chip's own addr
		 * echoed back) AND reply[2] == 0x01 (DMA mode flag). */
		if ((reply[0] >> 1) == TOUCH_I2C_ADDR && reply[2] == 0x01)
			return 0;
	}
	return -1;
}

int carthing_panel_probe(struct carthing_panel_info *info)
{
	struct udevice *chip;
	uint8_t mcbuf[12];
	uint8_t conf[204];
	uint32_t mc0, mc2;
	uint16_t conf_reg;
	uint32_t ver;
	int mccode = -1;
	int ret;

	memset(info, 0, sizeof(*info));

	ret = panel_get_chip(&chip);
	if (ret)
		return ret;

	ret = panel_enable_dma(chip);
	if (ret)
		return ret;

	/* Read MCCODE: 12 bytes at reg 0x8000. */
	if (dm_i2c_read(chip, TOUCH_MCCODE_REG, mcbuf, 12) == 0) {
		mc0 = (uint32_t)mcbuf[0] | ((uint32_t)mcbuf[1] << 8) |
		      ((uint32_t)mcbuf[2] << 16) | ((uint32_t)mcbuf[3] << 24);
		mc2 = (uint32_t)mcbuf[8] | ((uint32_t)mcbuf[9] << 8) |
		      ((uint32_t)mcbuf[10] << 16) | ((uint32_t)mcbuf[11] << 24);
		if (mc2 == 0x544c4e4b) {		/* "KNLT" */
			mccode = (mc0 == 0x35368008) ? 1 : 0;
		}
	}
	if (mccode < 0) {
		uint8_t alt[3];

		if (dm_i2c_read(chip, TOUCH_MCCODE_ALT_REG, alt, 3) == 0) {
			uint32_t v = (uint32_t)alt[0] |
				     ((uint32_t)alt[1] << 8) |
				     ((uint32_t)alt[2] << 16);
			mccode = (v == 0x444240 || v == 0x5c5c5c) ? 1 : 0;
		} else {
			mccode = 0;
		}
	}

	conf_reg = mccode ? TOUCH_CONFIG_1_REG : TOUCH_CONFIG_0_REG;
	ret = dm_i2c_read(chip, conf_reg, conf, sizeof(conf));
	if (ret)
		return ret;

	/* Vendor parsing:
	 *   ver = (u32)conf_buff[1] << 16 | conf_buff[0]    (u16 little-endian fields)
	 *   chip_code = conf_buff[53]
	 *   vendor_id = (ver >> 9) & 0x7f
	 *   conf_ver  = ver >> 26
	 *
	 * conf_buff is u16[], so each entry is 2 bytes (little-endian on the wire). */
	ver = ((uint32_t)conf[3] << 24) | ((uint32_t)conf[2] << 16) |
	      ((uint32_t)conf[1] << 8)  |  (uint32_t)conf[0];
	info->vendor_id = (ver >> 9) & 0x7f;
	info->conf_ver  = (ver >> 26) & 0x3f;
	info->hw_id     = (uint16_t)conf[106] | ((uint16_t)conf[107] << 8);
	info->mccode    = (uint8_t)mccode;
	info->valid     = true;

	/* Map to known variants per vendor table. */
	if (info->vendor_id == 0x11 && info->hw_id == 0x65c)
		info->variant = "BOE";
	else if (info->vendor_id == 0x70 && info->hw_id == 0x2d5c)
		info->variant = "Wily";
	else if (info->vendor_id == 0x11 && info->hw_id == 0xd5c)
		info->variant = "Holitech";
	else
		info->variant = "unknown";
	return 0;
}
