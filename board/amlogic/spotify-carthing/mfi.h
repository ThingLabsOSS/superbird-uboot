// SPDX-License-Identifier: GPL-2.0+
#ifndef __CARTHING_MFI_H
#define __CARTHING_MFI_H

#include <linux/types.h>

#define MFI_CMD_VERSION		0x00
#define MFI_CMD_ERROR		0x05
#define MFI_CMD_STATUS		0x10
#define MFI_CMD_RESPONSE	0x12
#define MFI_CMD_CHALLENGE_LEN	0x20
#define MFI_CMD_CHALLENGE	0x21
#define MFI_CMD_CERT_LEN	0x30
#define MFI_CMD_CERT		0x31
#define MFI_CMD_SERIAL		0x4E

#define MFI_CHALLENGE_SIZE	32
#define MFI_RESPONSE_SIZE	64
#define MFI_SERIAL_SIZE		32
#define MFI_CERT_MAX_SIZE	1024

int carthing_mfi_get_chip(struct udevice **chip);
int carthing_mfi_read_version(uint8_t *out);
int carthing_mfi_read_serial(uint8_t out[MFI_SERIAL_SIZE]);
int carthing_mfi_read_cert_len(uint16_t *out);
int carthing_mfi_read_cert(uint8_t *out, size_t len);
int carthing_mfi_challenge_response(const uint8_t challenge[MFI_CHALLENGE_SIZE],
				    uint8_t response[MFI_RESPONSE_SIZE]);

#endif /* __CARTHING_MFI_H */
