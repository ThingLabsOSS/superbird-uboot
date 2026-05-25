// SPDX-License-Identifier: GPL-2.0+
/*
 * Compose a compact JSON blob describing the carthing's hardware
 * (board rev, SoC ID, DRAM, panel, charger, MFi version, eMMC vendor
 * + part + size) and QR-encode it via nayuki's encoder. The intended
 * use is "scan the QR to grab the variant info" for cataloguing the
 * carthing-fleet's hardware variability.
 *
 * Only PROBED data goes in. We skip the cert / SHA / unprobed
 * peripherals (BT, mics) to keep the payload small enough for a
 * scannable QR version while still telling someone meaningful about
 * what's inside the unit.
 */
#include <dm.h>
#include <dm/uclass.h>
#include <env.h>
#include <mmc.h>
#include <vsprintf.h>
#include <linux/string.h>
#include <asm/arch/boot.h>

#include "boardrev.h"
#include "charger.h"
#include "mfi.h"
#include "qrcodegen.h"
#include "hwinfo_qr.h"
#include "panel_probe.h"

int carthing_tsensor_read_pll(void);
int carthing_tsensor_read_ddr(void);

/* Short string for the charger source type. Stripped-down version of
 * carthing_charger_type_str() — JSON-friendly tokens, not human prose. */
static const char *chr_type_token(uint8_t status)
{
	switch (status & 0xf) {
	case 0x0: return "none";
	case 0x1: return "SDP";
	case 0x2: return "CDP";
	case 0x3: return "DCP";
	case 0x4: return "Apple500";
	case 0x5: return "Apple1A";
	case 0x6: return "Apple2A";
	case 0x7: return "Special500";
	default:  return "unknown";
	}
}

/* Build the JSON. Returns the length written (excluding NUL) or 0
 * on error. The buffer must be at least HWINFO_QR_JSON_MAX bytes. */
size_t carthing_hwinfo_build_json(char *buf, size_t buflen)
{
	struct carthing_charger_info ch;
	struct mmc *mmc;
	uint8_t mfi_ver = 0;
	int rev, t_pll, t_ddr;
	u32 socinfo;
	char *p = buf;
	size_t left = buflen;
	int n;
	bool first = true;

	#define APPEND(fmt, ...) do { \
		n = snprintf(p, left, fmt, ##__VA_ARGS__); \
		if (n < 0 || (size_t)n >= left) return 0; \
		p += n; left -= n; \
	} while (0)

	#define KV(...) do { \
		if (!first) APPEND(","); \
		first = false; \
		APPEND(__VA_ARGS__); \
	} while (0)

	APPEND("{");

	rev = carthing_probe_board_rev();
	if (rev > 0)
		KV("\"rev\":%d", rev);

	{
		const char *s = env_get("serial#");
		const char *fs = env_get("f_serial");

		if (s)
			KV("\"serial\":\"%s\"", s);
		if (fs)
			KV("\"f_serial\":\"%s\"", fs);
	}

	socinfo = meson_get_socinfo();
	KV("\"soc\":\"G12A %x:%x\"",
	   (socinfo >> 24) & 0xff, (socinfo >> 8) & 0xff);

	KV("\"dram_mib\":%llu",
	   (unsigned long long)gd->ram_size / (1024 * 1024));

	{
		struct carthing_panel_info panel;

		if (carthing_panel_probe(&panel) == 0 && panel.valid)
			KV("\"panel\":{\"v\":\"%s\",\"vid\":%u,\"hw\":%u}",
			   panel.variant, panel.vendor_id, panel.hw_id);
		else
			KV("\"panel\":\"unknown\"");
	}

	mmc = find_mmc_device(0);
	if (mmc) {
		char name[7] = {0};

		name[0] = mmc->cid[0] & 0xff;
		name[1] = (mmc->cid[1] >> 24) & 0xff;
		name[2] = (mmc->cid[1] >> 16) & 0xff;
		name[3] = (mmc->cid[1] >> 8)  & 0xff;
		name[4] =  mmc->cid[1]        & 0xff;
		name[5] = (mmc->cid[2] >> 24) & 0xff;
		KV("\"emmc\":{\"mid\":%u,\"name\":\"%s\",\"mib\":%lu}",
		   (mmc->cid[0] >> 24) & 0xff, name,
		   (unsigned long)(mmc->capacity_user / (1024 * 1024)));
	}

	if (carthing_charger_read(&ch) == 0 && ch.valid)
		KV("\"chrg\":{\"src\":\"%s\",\"rev\":%u}",
		   chr_type_token(ch.status), ch.regs[0] & 0xf);

	if (carthing_mfi_read_version(&mfi_ver) == 0)
		KV("\"mfi_ver\":%u", mfi_ver);

	t_pll = carthing_tsensor_read_pll();
	t_ddr = carthing_tsensor_read_ddr();
	if (t_pll >= 0 && t_ddr >= 0)
		KV("\"temp\":{\"pll\":%d,\"ddr\":%d}", t_pll, t_ddr);

	APPEND("}");
	return p - buf;

	#undef KV
	#undef APPEND
}
