// SPDX-License-Identifier: GPL-2.0+
/*
 * `chargerinfo` u-boot command — diagnostic + manual register access
 * for the BC1.2/USB-C source detector at I2C 0x35.
 *
 * Subcommands:
 *   chargerinfo               -> dump all 16 regs + decoded status
 *   chargerinfo redetect      -> write 0x8F to reg 0x09, re-trigger detection
 *   chargerinfo read <reg>    -> read one register
 *   chargerinfo write <r> <v> -> write one register (HEX values)
 */
#include <command.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <vsprintf.h>

#include "charger.h"

static const char *swc_str(uint8_t reg07)
{
	switch ((reg07 >> 2) & 0x3) {
	case 0: return "open (D+/D- disconnected)";
	case 1: return "UART (D+/D- -> UT/UR)";
	case 2: return "USB (D+/D- -> TD+/TD- to SoC)";
	case 3: return "auto (state machine)";
	}
	return "?";
}

static int do_chargerinfo_dump(void)
{
	struct carthing_charger_info info;
	int i;

	if (carthing_charger_read(&info) || !info.valid) {
		printf("Charger: I2C read failed\n");
		return CMD_RET_FAILURE;
	}

	printf("MAX14656 USB charger detector @ I2C 0x35\n");
	printf("Registers 0x00..0x09:\n");
	for (i = 0; i < 10; i += 5) {
		printf("  0x%02x:", i);
		for (int j = 0; j < 5 && i + j < 10; j++)
			printf(" %02x", info.regs[i + j]);
		printf("\n");
	}

	printf("\nDecoded:\n");
	printf("  DEVICE ID (0x00):    vendor=%d  rev=%d\n",
	       (info.regs[0x00] >> 4) & 0xf, info.regs[0x00] & 0xf);
	printf("  STATUS 1 (0x03):     0x%02x\n", info.status);
	printf("    CHG_TYP_S [3:0]: %x -> %s\n",
	       info.status & 0xf, carthing_charger_type_str(info.status));
	printf("    VB_VALID_S [4]:  %d  (VBUS %svalid)\n",
	       (info.status >> 4) & 1, ((info.status >> 4) & 1) ? "" : "in");
	printf("    OVP_S [5]:       %d  (%sovervoltage)\n",
	       (info.status >> 5) & 1, ((info.status >> 5) & 1) ? "" : "no ");
	printf("    CHG_DET_RUN_S [6]: %d  (detection %s)\n",
	       (info.status >> 6) & 1,
	       ((info.status >> 6) & 1) ? "RUNNING" : "idle");
	printf("  STATUS 2 (0x04):     0x%02x  ADC_S=0x%02x (ID pin resistor)\n",
	       info.regs[0x04], info.regs[0x04] & 0x1f);
	printf("  CONTROL 1 (0x07):    0x%02x  USB_SWC=%s\n",
	       info.regs[0x07], swc_str(info.regs[0x07]));
	printf("  CONTROL 2 (0x08):    0x%02x\n", info.regs[0x08]);
	printf("  CONTROL 3 (0x09):    0x%02x  (write 0x8F here to force redetect)\n",
	       info.regs[0x09]);
	return CMD_RET_SUCCESS;
}

static int do_chargerinfo_redetect(void)
{
	struct carthing_charger_info before, after;
	int ret;

	if (carthing_charger_read(&before) || !before.valid) {
		printf("chargerinfo: pre-read failed\n");
		return CMD_RET_FAILURE;
	}
	printf("Status before: 0x%02x (%s)\n", before.status,
	       carthing_charger_type_str(before.status));

	ret = carthing_charger_redetect();
	if (ret) {
		printf("chargerinfo: redetect write failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	printf("Triggered redetection — waiting %d ms...\n",
	       CARTHING_CHARGER_REDETECT_DELAY_MS);
	mdelay(CARTHING_CHARGER_REDETECT_DELAY_MS);

	if (carthing_charger_read(&after) || !after.valid) {
		printf("chargerinfo: post-read failed\n");
		return CMD_RET_FAILURE;
	}
	printf("Status after:  0x%02x (%s)\n", after.status,
	       carthing_charger_type_str(after.status));
	return CMD_RET_SUCCESS;
}

static int parse_hex_byte(const char *s, uint8_t *out)
{
	char *end;
	unsigned long v = simple_strtoul(s, &end, 16);

	if (*end || v > 0xff)
		return -1;
	*out = (uint8_t)v;
	return 0;
}

static int do_chargerinfo_dataswitch(const char *arg)
{
	enum carthing_charger_swc swc;
	int ret;
	uint8_t reg;

	if (!strcmp(arg, "off") || !strcmp(arg, "open"))
		swc = CARTHING_CHARGER_SWC_OPEN;
	else if (!strcmp(arg, "uart"))
		swc = CARTHING_CHARGER_SWC_UART;
	else if (!strcmp(arg, "usb") || !strcmp(arg, "on"))
		swc = CARTHING_CHARGER_SWC_USB;
	else if (!strcmp(arg, "auto"))
		swc = CARTHING_CHARGER_SWC_AUTO;
	else
		return CMD_RET_USAGE;

	ret = carthing_charger_set_usb_swc(swc);
	if (ret) {
		printf("dataswitch write failed (%d)\n", ret);
		return CMD_RET_FAILURE;
	}
	if (!carthing_charger_read_reg(0x07, &reg))
		printf("USB_SWC -> %s (reg 0x07 = 0x%02x)\n",
		       swc_str(reg), reg);
	return CMD_RET_SUCCESS;
}

static int do_chargerinfo(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	uint8_t reg, val;

	if (argc <= 1)
		return do_chargerinfo_dump();

	if (!strcmp(argv[1], "redetect"))
		return do_chargerinfo_redetect();

	if (!strcmp(argv[1], "dataswitch") && argc == 3)
		return do_chargerinfo_dataswitch(argv[2]);

	if (!strcmp(argv[1], "read") && argc == 3) {
		if (parse_hex_byte(argv[2], &reg))
			return CMD_RET_USAGE;
		if (carthing_charger_read_reg(reg, &val)) {
			printf("read failed\n");
			return CMD_RET_FAILURE;
		}
		printf("reg 0x%02x = 0x%02x\n", reg, val);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "write") && argc == 4) {
		if (parse_hex_byte(argv[2], &reg) ||
		    parse_hex_byte(argv[3], &val))
			return CMD_RET_USAGE;
		if (reg >= 16) {
			printf("reg out of range (chip has 16-byte space)\n");
			return CMD_RET_USAGE;
		}
		if (carthing_charger_write_reg(reg, val)) {
			printf("write failed\n");
			return CMD_RET_FAILURE;
		}
		printf("wrote 0x%02x -> reg 0x%02x\n", val, reg);
		return CMD_RET_SUCCESS;
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	chargerinfo, 4, 1, do_chargerinfo,
	"MAX14656 USB charger detector control",
	"\n"
	"  chargerinfo                  - dump all registers + decoded\n"
	"  chargerinfo redetect         - re-trigger USB source detection\n"
	"  chargerinfo dataswitch <m>   - set DPDT analog switch\n"
	"      m = off  -> all switches open (disconnect D+/D- from SoC)\n"
	"      m = uart -> route D+/D- to UT/UR (chip's UART lines)\n"
	"      m = usb  -> route D+/D- to TD+/TD- (force USB to SoC)\n"
	"      m = auto -> follow detection state machine (default)\n"
	"  chargerinfo read <reg>       - read one register (HEX)\n"
	"  chargerinfo write <r> <v>    - write one register (HEX)\n"
	"\n"
	"  Chip is MAX14656 at I2C 0x35. Registers 0x00..0x09 only;\n"
	"  reads at 0x0a+ return undefined data."
);
