// SPDX-License-Identifier: GPL-2.0+
/*
 * `mfi` u-boot command — exercise the Apple MFi auth chip driver.
 *
 *   mfi version       - read VERSION (cmd 0x00, 1 byte)
 *   mfi cert_len      - read CERT_LEN (cmd 0x30, 2 B BE)
 *   mfi cert          - read full X.509 certificate (cmd 0x31)
 *   mfi serial        - read SERIAL (cmd 0x4E, 32 B)
 */
#include <command.h>
#include <malloc.h>

#include "mfi.h"

static void print_hex(const char *prefix, const uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if ((i % 16) == 0)
			printf("%s%04zx:", prefix, i);
		printf(" %02x", buf[i]);
		if ((i % 16) == 15)
			printf("\n");
	}
	if ((len % 16) != 0)
		printf("\n");
}

static int do_mfi_version(void)
{
	uint8_t v;
	int ret = carthing_mfi_read_version(&v);

	if (ret < 0) {
		printf("mfi: read failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	printf("MFi VERSION: 0x%02x\n", v);
	return CMD_RET_SUCCESS;
}

static int do_mfi_cert_len(void)
{
	uint16_t len;
	int ret = carthing_mfi_read_cert_len(&len);

	if (ret < 0) {
		printf("mfi: read failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	printf("MFi CERT_LEN: %u bytes\n", len);
	return CMD_RET_SUCCESS;
}

static int do_mfi_cert(void)
{
	uint16_t cert_len;
	uint8_t *buf;
	int ret = carthing_mfi_read_cert_len(&cert_len);

	if (ret < 0) {
		printf("mfi: cert_len read failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	if (cert_len == 0 || cert_len > MFI_CERT_MAX_SIZE) {
		printf("mfi: implausible cert length %u\n", cert_len);
		return CMD_RET_FAILURE;
	}

	buf = malloc(cert_len);
	if (!buf) {
		printf("mfi: alloc failed\n");
		return CMD_RET_FAILURE;
	}
	ret = carthing_mfi_read_cert(buf, cert_len);
	if (ret < 0) {
		printf("mfi: cert read failed (%d)\n", ret);
		free(buf);
		return CMD_RET_FAILURE;
	}

	printf("MFi CERT (%u bytes):\n", cert_len);
	print_hex("  ", buf, cert_len);
	free(buf);
	return CMD_RET_SUCCESS;
}

static int do_mfi_serial(void)
{
	uint8_t serial[MFI_SERIAL_SIZE];
	int ret = carthing_mfi_read_serial(serial);

	if (ret < 0) {
		printf("mfi: read failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	printf("MFi SERIAL (32 B):\n");
	print_hex("  ", serial, MFI_SERIAL_SIZE);
	return CMD_RET_SUCCESS;
}

static int do_mfi(struct cmd_tbl *cmdtp, int flag, int argc,
		  char *const argv[])
{
	if (argc <= 1)
		return CMD_RET_USAGE;
	if (!strcmp(argv[1], "version"))
		return do_mfi_version();
	if (!strcmp(argv[1], "cert_len") || !strcmp(argv[1], "certlen"))
		return do_mfi_cert_len();
	if (!strcmp(argv[1], "cert"))
		return do_mfi_cert();
	if (!strcmp(argv[1], "serial"))
		return do_mfi_serial();
	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	mfi, 2, 1, do_mfi,
	"Apple MFi auth coprocessor (Car Thing only)",
	"\n"
	"  mfi version    - read firmware version byte (cmd 0x00)\n"
	"  mfi cert_len   - read X.509 certificate length (cmd 0x30)\n"
	"  mfi cert       - dump full X.509 certificate (cmd 0x31)\n"
	"  mfi serial     - read 32-byte device serial / UID (cmd 0x4E)\n"
);
