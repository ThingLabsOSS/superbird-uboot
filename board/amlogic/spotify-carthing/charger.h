// SPDX-License-Identifier: GPL-2.0+
#ifndef __CARTHING_CHARGER_H
#define __CARTHING_CHARGER_H

#include <linux/types.h>

/*
 * USB_SWC values for the chip's DPDT analog switch. Drives where the
 * USB-C connector's D+/D- lines get routed. See MAX14656 datasheet
 * register CONTROL 1 (0x07) bits [3:2].
 */
enum carthing_charger_swc {
	CARTHING_CHARGER_SWC_OPEN = 0,	/* all switches open - D+/D- disconnected from SoC */
	CARTHING_CHARGER_SWC_UART = 1,	/* CD+/CD- routed to UT/UR (UART) */
	CARTHING_CHARGER_SWC_USB  = 2,	/* CD+/CD- routed to TD+/TD- (forced USB to SoC) */
	CARTHING_CHARGER_SWC_AUTO = 3,	/* follow the charger-detection state machine (default) */
};

struct carthing_charger_info {
	uint8_t regs[10];	/* registers 0x00..0x09; 0x0a..0x0f are RFU */
	uint8_t status;		/* convenience: regs[0x03] (STATUS 1) */
	int valid;		/* 1 if read succeeded, 0 otherwise */
};

/* Settling delay after triggering a redetect via carthing_charger_redetect()
 * before reading the status register back. Empirical — the chip takes ~150 ms
 * to re-run BC1.2 detection on a stable VBUS; 250 ms gives margin for
 * slow-rising supplies. */
#define CARTHING_CHARGER_REDETECT_DELAY_MS	250

/* Reads registers 0x00..0x09 from the USB-source classifier IC (0x0a..0x0f
 * are RFU and not read). Returns 0 on success, negative errno on I2C
 * failure. `info` is always populated (info->valid distinguishes good vs
 * failed). */
int carthing_charger_read(struct carthing_charger_info *info);

/* Decode the status register byte into a human-readable label.
 * Always returns a non-NULL constant string. */
const char *carthing_charger_type_str(uint8_t status);

/* Write a single register. Use with care — writing into a power IC
 * with no confirmed datasheet can mis-configure the load switch.
 * Returns 0 / negative errno. */
int carthing_charger_write_reg(uint8_t reg, uint8_t val);

/* Read a single register. Returns 0 / negative errno; result via *val. */
int carthing_charger_read_reg(uint8_t reg, uint8_t *val);

/* Re-trigger USB-source detection. Vendor u-boot's check_charger
 * macro does this by writing 0x8F to register 0x09 in a retry loop.
 * Returns 0 / negative errno. */
int carthing_charger_redetect(void);

/* Route the USB-C connector's D+/D- via the chip's DPDT analog switch.
 * Read-modify-write on CONTROL 1 reg 0x07 [3:2]. Other bits preserved.
 * Returns 0 / negative errno. */
int carthing_charger_set_usb_swc(enum carthing_charger_swc swc);

#endif /* __CARTHING_CHARGER_H */
