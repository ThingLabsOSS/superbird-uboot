// SPDX-License-Identifier: GPL-2.0+
/*
 * Apple MFi authentication coprocessor (CP2.0 / CP3.0), I2C bus 3 addr 0x10.
 * Protocol reconstructed from a clean-room decompile of stock's
 * apple_mfi_auth_i2c.ko. Register map + IOCTL mapping:
 * superbird-docs/hardware/i2c-devices.md.
 *
 * Three quirks vs a normal SMBus device, all load-bearing:
 *  - Sleeps when idle; the first transaction wakes it and NAKs. Needs up to
 *    3 retries ~860 us apart.
 *  - Reads are SPLIT: the cmd write is its own transaction (with STOP), then
 *    a separate read with no sub-address. u-boot's combined APIs issue
 *    write-then-repeated-start, which the chip mishandles on most registers.
 *  - CERT_LEN (0x30) and CERT (0x31) need a 10 ms settle before the read.
 */
#include <dm.h>
#include <dm/uclass.h>
#include <i2c.h>
#include <linux/delay.h>

#include "mfi.h"

#define MFI_I2C_BUS		3	/* matches vendor DT bus numbering */
#define MFI_I2C_ADDR		0x10

/*
 * The kmod uses 3 retries with __const_udelay(2147500) ≈ 860 us, which
 * works under Linux because the kernel's i2c subsystem adds extra
 * latency between each retry. In u-boot the dm_i2c_xfer round-trip
 * is tighter, and the chip falls back asleep quickly between u-boot
 * commands, so we need a much fatter wake budget. 8 retries × 5 ms
 * = 40 ms total empirically gets cold reads to succeed every time
 * on the carthing's MFi 3.0 / CP3.0 chip.
 */
#define MFI_MAX_RETRIES		8
#define MFI_RETRY_MS		5
#define MFI_CERT_SETTLE_MS	10

int carthing_mfi_get_chip(struct udevice **chip)
{
	struct udevice *bus;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_I2C, MFI_I2C_BUS, &bus);
	if (ret)
		return ret;
	/* Use i2c_get_chip (no live probe) instead of dm_i2c_probe. The
	 * MFi chip sleeps and NACKs the probe transaction, which would
	 * make get_chip fail before our retry loops get a chance to wake
	 * it. */
	return i2c_get_chip(bus, MFI_I2C_ADDR, 1, chip);
}

/* Write a single command byte. Mimics i2c_smbus_write_byte() — full
 * transaction, no payload. Retries up to MFI_MAX_RETRIES times to wake
 * the chip from low-power sleep. */
static int mfi_prepare(struct udevice *chip, uint8_t cmd)
{
	struct i2c_msg msg = {
		.addr = MFI_I2C_ADDR,
		.flags = 0,
		.buf = &cmd,
		.len = 1,
	};
	int attempt = 0;
	int ret;

	do {
		ret = dm_i2c_xfer(chip, &msg, 1);
		if (ret >= 0)
			return ret;
		mdelay(MFI_RETRY_MS);
	} while (++attempt < MFI_MAX_RETRIES);

	return ret;
}

/* Raw read transaction — START + addr/R + read + STOP. No sub-address
 * write. The chip's internal pointer was set by the prior prepare. */
static int mfi_raw_read(struct udevice *chip, void *buf, size_t len)
{
	struct i2c_msg msg = {
		.addr = MFI_I2C_ADDR,
		.flags = I2C_M_RD,
		.buf = buf,
		.len = len,
	};
	int attempt = 0;
	int ret;

	do {
		ret = dm_i2c_xfer(chip, &msg, 1);
		if (ret >= 0)
			return ret;
		mdelay(MFI_RETRY_MS);
	} while (++attempt < MFI_MAX_RETRIES);

	return ret;
}

/* SMBus block read: write the cmd byte AGAIN at the start of the read
 * transaction, then read N bytes. The kmod uses
 * i2c_smbus_read_i2c_block_data() which is equivalent to u-boot's
 * dm_i2c_read with a 1-byte offset. Used for "small" registers
 * (VERSION, STATUS, ERROR, CHALLENGE_LEN, CERT_LEN). */
static int mfi_smbus_read(struct udevice *chip, uint8_t cmd, void *buf,
			  size_t len)
{
	int attempt = 0;
	int ret;

	do {
		ret = dm_i2c_read(chip, cmd, buf, len);
		if (ret >= 0)
			return ret;
		mdelay(MFI_RETRY_MS);
	} while (++attempt < MFI_MAX_RETRIES);

	return ret;
}

/* "Raw recv" operation: prepare(cmd), then read N bytes with no
 * sub-address byte. Used for payloads bigger than the SMBus 32-byte
 * block limit (CERT, RESPONSE) and for SERIAL. CERT/CERT_LEN
 * additionally need a 10 ms settle between prepare and read. */
static int mfi_op_read_raw(struct udevice *chip, uint8_t cmd, void *buf,
			   size_t len, bool settle)
{
	int ret = mfi_prepare(chip, cmd);

	if (ret < 0)
		return ret;
	mdelay(settle ? MFI_CERT_SETTLE_MS : 1);
	return mfi_raw_read(chip, buf, len);
}

/* SMBus block read with prepare + optional settle.
 *
 * In addition to the documented 10 ms cert settle, a small inter-
 * transaction gap (a few hundred us) is needed after prepare even
 * for non-cert reads. The kmod doesn't insert this explicitly but
 * Linux's i2c subsystem adds enough overhead between transfers that
 * it ends up there implicitly. In u-boot we have to be explicit. */
static int mfi_op_read_smbus(struct udevice *chip, uint8_t cmd, void *buf,
			     size_t len, bool settle)
{
	int ret = mfi_prepare(chip, cmd);

	if (ret < 0)
		return ret;
	mdelay(settle ? MFI_CERT_SETTLE_MS : 1);
	return mfi_smbus_read(chip, cmd, buf, len);
}

int carthing_mfi_read_version(uint8_t *out)
{
	struct udevice *chip;
	int ret = carthing_mfi_get_chip(&chip);

	if (ret)
		return ret;
	return mfi_op_read_smbus(chip, MFI_CMD_VERSION, out, 1, false);
}

int carthing_mfi_read_serial(uint8_t out[MFI_SERIAL_SIZE])
{
	struct udevice *chip;
	int ret = carthing_mfi_get_chip(&chip);

	if (ret)
		return ret;
	return mfi_op_read_raw(chip, MFI_CMD_SERIAL, out, MFI_SERIAL_SIZE, false);
}

int carthing_mfi_read_cert_len(uint16_t *out)
{
	struct udevice *chip;
	uint8_t buf[2];
	int ret = carthing_mfi_get_chip(&chip);

	if (ret)
		return ret;
	ret = mfi_op_read_smbus(chip, MFI_CMD_CERT_LEN, buf, 2, true);
	if (ret < 0)
		return ret;
	*out = ((uint16_t)buf[0] << 8) | buf[1];
	return 0;
}

int carthing_mfi_read_cert(uint8_t *out, size_t len)
{
	struct udevice *chip;
	int ret = carthing_mfi_get_chip(&chip);

	if (ret)
		return ret;
	return mfi_op_read_raw(chip, MFI_CMD_CERT, out, len, true);
}

/*
 * Mirrors the vendor's apple_mfi_auth.c MFI_IOCTL_CHALLENGE flow:
 *   1. write CHALLENGE (cmd 0x21 + 32 bytes)
 *   2. read CHALLENGE_LEN (reg 0x20) — sanity check, expect 32
 *   3. write STATUS (cmd 0x10, value 1) to kick signing
 *   4. wait ~500 ms for ECDSA P-256 signing
 *   5. read STATUS — expect 0x10 = done
 *   6. read RESPONSE (cmd 0x12, 64 bytes)
 */
int carthing_mfi_challenge_response(const uint8_t challenge[MFI_CHALLENGE_SIZE],
				    uint8_t response[MFI_RESPONSE_SIZE])
{
	struct udevice *chip;
	uint8_t cmdbuf[1 + MFI_CHALLENGE_SIZE];
	struct i2c_msg msg = {
		.addr = MFI_I2C_ADDR,
		.flags = 0,
		.buf = cmdbuf,
		.len = sizeof(cmdbuf),
	};
	uint8_t chal_len_be[2];
	uint16_t chal_len;
	uint8_t status;
	int attempt;
	int ret = carthing_mfi_get_chip(&chip);

	if (ret)
		return ret;

	/* 1. Write challenge (cmd 0x21 + 32 bytes). */
	cmdbuf[0] = MFI_CMD_CHALLENGE;
	memcpy(cmdbuf + 1, challenge, MFI_CHALLENGE_SIZE);
	for (attempt = 0; attempt < MFI_MAX_RETRIES; attempt++) {
		ret = dm_i2c_xfer(chip, &msg, 1);
		if (ret >= 0)
			break;
		mdelay(MFI_RETRY_MS);
	}
	if (ret < 0)
		return ret;

	/* 2. Verify chip latched 32 bytes by reading CHALLENGE_LEN. */
	ret = mfi_op_read_smbus(chip, MFI_CMD_CHALLENGE_LEN, chal_len_be, 2, false);
	if (ret < 0)
		return ret;
	chal_len = ((uint16_t)chal_len_be[0] << 8) | chal_len_be[1];
	if (chal_len != MFI_CHALLENGE_SIZE)
		return -EIO;

	/* 3. Kick signing: write 1 to STATUS register. */
	cmdbuf[0] = MFI_CMD_STATUS;
	cmdbuf[1] = 1;
	msg.len = 2;
	for (attempt = 0; attempt < MFI_MAX_RETRIES; attempt++) {
		ret = dm_i2c_xfer(chip, &msg, 1);
		if (ret >= 0)
			break;
		mdelay(MFI_RETRY_MS);
	}
	if (ret < 0)
		return ret;

	/* 4. Wait for ECDSA P-256 signing to complete. */
	mdelay(500);

	/* 5. Sanity-check status. 0x10 = done. */
	ret = mfi_op_read_smbus(chip, MFI_CMD_STATUS, &status, 1, false);
	if (ret < 0)
		return ret;
	if (status != 0x10)
		return -EBUSY;

	/* 6. Read 64-byte signature. */
	return mfi_op_read_raw(chip, MFI_CMD_RESPONSE, response,
			       MFI_RESPONSE_SIZE, false);
}
