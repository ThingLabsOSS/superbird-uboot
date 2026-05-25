// SPDX-License-Identifier: GPL-2.0+
/*
 * USB-source classifier on the Car Thing.
 *
 * I2C addr 0x35 on SoC i2c2 (u-boot bus 0). Strict 16-byte register
 * space. Almost certainly a MAX14656-family BC1.2/QC/PD source
 * detector (Maxim WLCSP-16, AAL marking).
 *
 * Only the status register (0x03) and the empirical "good source"
 * codes from vendor u-boot's check_charger macro are confirmed; the
 * rest is speculative without an actual datasheet match. Treat reads
 * as opaque + best-effort.
 */
#include <dm.h>
#include <dm/uclass.h>
#include <i2c.h>

#include "charger.h"

#define CHARGER_I2C_BUS		2	/* matches vendor DT bus numbering (i2c2 alias) */
#define CHARGER_I2C_ADDR	0x35
#define CHARGER_REG_DEVICE_ID	0x00	/* [7:4] VENDOR_ID, [3:0] CHIP_REV */
#define CHARGER_REG_STATUS1	0x03	/* [6] CHG_DET_RUN_S, [4] VB_VALID_S, [3:0] CHG_TYP_S */
#define CHARGER_REG_CONTROL1	0x07	/* [3:2] USB_SWC, [4] INT_EN, ... */
#define CHARGER_REG_CONTROL3	0x09	/* [1] CHG_TYP_MAN, [0] CHG_DET_EN, ... */
#define CHARGER_REDETECT_VAL	0x8F	/* CHG_TYP_MAN=1 + CHG_DET_EN=1 + DCD_EN=1 + DCD_2S_CT=1 + OVP_EN=10 */
#define CHARGER_NUM_REGS	10	/* 0x00..0x09 are defined; 0x0a..0x0f are RFU/undefined */

static int carthing_charger_get_chip(struct udevice **chip)
{
	struct udevice *bus;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_I2C, CHARGER_I2C_BUS, &bus);
	if (ret)
		return ret;
	return dm_i2c_probe(bus, CHARGER_I2C_ADDR, 0, chip);
}

int carthing_charger_read(struct carthing_charger_info *info)
{
	struct udevice *chip;
	int ret;

	if (!info)
		return -EINVAL;

	info->valid = 0;
	info->status = 0;

	ret = carthing_charger_get_chip(&chip);
	if (ret)
		return ret;

	ret = dm_i2c_read(chip, 0, info->regs, CHARGER_NUM_REGS);
	if (ret)
		return ret;

	info->status = info->regs[CHARGER_REG_STATUS1];
	info->valid = 1;
	return 0;
}

int carthing_charger_read_reg(uint8_t reg, uint8_t *val)
{
	struct udevice *chip;
	int ret;

	if (!val || reg >= CHARGER_NUM_REGS)
		return -EINVAL;
	ret = carthing_charger_get_chip(&chip);
	if (ret)
		return ret;
	return dm_i2c_read(chip, reg, val, 1);
}

int carthing_charger_write_reg(uint8_t reg, uint8_t val)
{
	struct udevice *chip;
	int ret;

	if (reg >= CHARGER_NUM_REGS)
		return -EINVAL;
	ret = carthing_charger_get_chip(&chip);
	if (ret)
		return ret;
	return dm_i2c_write(chip, reg, &val, 1);
}

int carthing_charger_redetect(void)
{
	return carthing_charger_write_reg(CHARGER_REG_CONTROL3,
					  CHARGER_REDETECT_VAL);
}

/*
 * Drive the chip's DPDT analog switch routing the USB-C connector's
 * D+/D- lines. Bits [3:2] of CONTROL 1 (reg 0x07) = USB_SWC.
 * Other bits in 0x07 keep their current value (read-modify-write).
 */
int carthing_charger_set_usb_swc(enum carthing_charger_swc swc)
{
	uint8_t reg, val;
	int ret;

	if ((unsigned)swc > 3)
		return -EINVAL;

	ret = carthing_charger_read_reg(CHARGER_REG_CONTROL1, &reg);
	if (ret)
		return ret;

	val = (reg & ~0x0c) | (((uint8_t)swc & 0x3) << 2);
	return carthing_charger_write_reg(CHARGER_REG_CONTROL1, val);
}

/*
 * Decode the CHG_TYP_S field (bits [3:0] of STATUS 1 / register 0x03).
 * Per MAX14656 datasheet table 2 (status register definitions). The
 * upper bits of the byte are CHG_DET_RUN_S / VB_VALID_S etc and are
 * not part of the type code.
 */
const char *carthing_charger_type_str(uint8_t status)
{
	switch (status & 0xf) {
	case 0x0: return "no source";
	case 0x1: return "USB SDP (~500 mA host)";
	case 0x2: return "USB CDP (~1.5 A host)";
	case 0x3: return "USB DCP (~1.5 A charger)";
	case 0x4: return "Apple 500 mA";
	case 0x5: return "Apple 1 A";
	case 0x6: return "Apple 2 A";
	case 0x7: return "Special 500 mA";
	case 0xc: return "Apple RFU";
	default:  return "reserved";
	}
}
