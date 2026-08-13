// SPDX-License-Identifier: GPL-2.0+
/*
 * Hardware inventory screens.
 *
 *   0  overview (board / SoC / DRAM / panel / charger / temp / eMMC / serials)
 *   1  I2C bus map + undriven peripherals
 *   2  MFi cert info -- knob press opens the cert-hex modal
 *   3  MFi challenge/response (live ECDSA sign)
 *   4  SARADC channels, decoded
 *   5  QR code of the hwinfo JSON payload
 *
 * Page data is cached, not re-read per redraw: the MFi cert alone costs ~40 ms
 * of cold-wake retries plus a 10 ms settle before the 608-byte payload.
 */

#include <adc.h>
#include <console.h>
#include <dm.h>
#include <dm/uclass.h>
#include <env.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <mmc.h>
#include <time.h>
#include <u-boot/sha256.h>
#include <video.h>
#include <vsprintf.h>

#include <asm/arch/boot.h>
#include <asm/global_data.h>

#include "boardrev.h"
#include "cert_parse.h"
#include "charger.h"
#include "hwinfo_qr.h"
#include "menu_hwinfo.h"
#include "menu_ui.h"
#include "mfi.h"
#include "panel_probe.h"
#include "qrcodegen.h"
#include "wheel.h"

/* From cmd_tsensor.c — on-die temperature sensors. */
int carthing_tsensor_read_pll(void);
int carthing_tsensor_read_ddr(void);

#define HWINFO_NPAGES	6

static int hwinfo_page;		/* persists across re-entry */

/* Cert hex modal, entered by knob press on page 2. */
static int cert_scroll_lines;	/* top-of-window, in 16-byte lines */
#define CERT_HEX_VISIBLE_ROWS	11
#define CERT_HEX_BYTES_PER_ROW	16

static struct {
	bool valid;
	char json[HWINFO_QR_JSON_MAX];
	size_t json_len;
	/* Sized per nayuki's BUFFER_LEN_MAX for the max version we permit. */
	uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
	uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
	int qr_size;
} hwinfo_qr_cache;

static struct {
	bool valid;
	uint raw[8];
} hwinfo_adc;

static struct carthing_panel_info hwinfo_panel_cache;

static struct {
	bool valid;
	uint8_t version;
	uint8_t serial[MFI_SERIAL_SIZE];
	uint16_t cert_len;
	uint8_t cert_sha256[SHA256_SUM_LEN];
	uint8_t cert[MFI_CERT_MAX_SIZE];
	struct carthing_cert_info parsed;
} hwinfo_mfi_cache;

static struct {
	bool valid;
	uint32_t counter;
	uint8_t challenge[MFI_CHALLENGE_SIZE];
	uint8_t response[MFI_RESPONSE_SIZE];
	uint32_t elapsed_ms;
} hwinfo_chr_cache;

static int refresh_mfi_cache(void)
{
	int ret;

	hwinfo_mfi_cache.valid = false;

	ret = carthing_mfi_read_version(&hwinfo_mfi_cache.version);
	if (ret < 0)
		return ret;
	ret = carthing_mfi_read_serial(hwinfo_mfi_cache.serial);
	if (ret < 0)
		return ret;
	ret = carthing_mfi_read_cert_len(&hwinfo_mfi_cache.cert_len);
	if (ret < 0)
		return ret;
	if (hwinfo_mfi_cache.cert_len == 0 ||
	    hwinfo_mfi_cache.cert_len > MFI_CERT_MAX_SIZE)
		return -EINVAL;
	ret = carthing_mfi_read_cert(hwinfo_mfi_cache.cert,
				     hwinfo_mfi_cache.cert_len);
	if (ret < 0)
		return ret;
	sha256_csum_wd(hwinfo_mfi_cache.cert, hwinfo_mfi_cache.cert_len,
		       hwinfo_mfi_cache.cert_sha256, CHUNKSZ_SHA256);
	(void)carthing_cert_parse(hwinfo_mfi_cache.cert,
				  hwinfo_mfi_cache.cert_len,
				  &hwinfo_mfi_cache.parsed);
	hwinfo_mfi_cache.valid = true;
	return 0;
}

static void draw_hwinfo_chrome(int page)
{
	char title[VC_COLS + 1];
	char sep[VC_COLS + 1];
	int i, tlen;
	/* NEVER write the last column: vidconsole wraps after it, which on the
	 * bottom row scrolls the whole screen. */
	int max_w = VC_COLS - 1;

	snprintf(title, sizeof(title), "HARDWARE INFO");
	tlen = strlen(title);
	for (i = tlen; i < max_w - 8; i++)
		title[i] = ' ';
	title[max_w - 8] = '\0';
	snprintf(title + strlen(title), sizeof(title) - strlen(title),
		 "Page %d/%d", page + 1, HWINFO_NPAGES);

	for (i = 0; i < max_w; i++)
		sep[i] = '-';
	sep[max_w] = '\0';

	vc_at(0, 0);
	vc_puts(title);
	vc_at(1, 0);
	vc_puts(sep);
}

/* JEDEC eMMC manufacturer IDs. The carthing shipped with parts from several
 * vendors across builds, so this covers the plausible candidates. */
static const char *mmc_mid_name(uint8_t mid)
{
	switch (mid) {
	case 0x02: return "SanDisk";
	case 0x11: return "Toshiba";
	case 0x13: return "Micron";
	case 0x15: return "Samsung";
	case 0x45: return "SanDisk";
	case 0x70: return "Kingston";
	case 0x88: return "Foresee";
	case 0x90: return "SK Hynix";
	case 0x9b: return "YMTC";
	case 0xfe: return "Numonyx";
	default:   return NULL;
	}
}

static void draw_hwinfo_page0(void)
{
	struct carthing_charger_info ch;
	char buf[64];
	u32 socinfo;
	int rev, t_pll, t_ddr, row = 2;
	bool charger_ok;
	DECLARE_GLOBAL_DATA_PTR;

	rev = carthing_probe_board_rev();
	if (rev > 0)
		snprintf(buf, sizeof(buf), "Board    : Rev %d", rev);
	else
		snprintf(buf, sizeof(buf), "Board    : unknown");
	vc_at(row++, 0);
	vc_puts(buf);

	socinfo = meson_get_socinfo();
	snprintf(buf, sizeof(buf), "SoC      : G12A S905D2 %x:%x",
		 (socinfo >> 24) & 0xff, (socinfo >> 8) & 0xff);
	vc_at(row++, 0);
	vc_puts(buf);

	snprintf(buf, sizeof(buf), "DRAM     : %llu MiB",
		 (unsigned long long)gd->ram_size / (1024 * 1024));
	vc_at(row++, 0);
	vc_puts(buf);

	if (hwinfo_panel_cache.valid)
		snprintf(buf, sizeof(buf),
			 "Panel    : %s ST7701S (vid=%02x hw=%03x)",
			 hwinfo_panel_cache.variant,
			 hwinfo_panel_cache.vendor_id,
			 hwinfo_panel_cache.hw_id);
	else
		snprintf(buf, sizeof(buf),
			 "Panel    : ST7701S (probe failed)");
	vc_at(row++, 0);
	vc_puts(buf);

	charger_ok = (carthing_charger_read(&ch) == 0 && ch.valid);
	if (charger_ok)
		snprintf(buf, sizeof(buf), "Charger  : %s",
			 carthing_charger_type_str(ch.status));
	else
		snprintf(buf, sizeof(buf), "Charger  : (no response)");
	vc_at(row++, 0);
	vc_puts(buf);

	t_pll = carthing_tsensor_read_pll();
	t_ddr = carthing_tsensor_read_ddr();
	if (t_pll >= 0 && t_ddr >= 0)
		snprintf(buf, sizeof(buf), "Temp     : PLL %dC  DDR %dC",
			 t_pll, t_ddr);
	else
		snprintf(buf, sizeof(buf), "Temp     : read failed");
	vc_at(row++, 0);
	vc_puts(buf);

	{
		const char *bv = env_get("brightness");

		if (!bv)
			bv = "(unset)";
		snprintf(buf, sizeof(buf), "Backlight: %s", bv);
		vc_at(row++, 0);
		vc_puts(buf);
	}

	{
		struct mmc *mmc = find_mmc_device(0);

		if (mmc) {
			const char *mfg = mmc_mid_name(mmc->cid[0] >> 24);
			char name[7] = {0};
			unsigned long mib;

			name[0] = mmc->cid[0] & 0xff;
			name[1] = (mmc->cid[1] >> 24) & 0xff;
			name[2] = (mmc->cid[1] >> 16) & 0xff;
			name[3] = (mmc->cid[1] >> 8)  & 0xff;
			name[4] =  mmc->cid[1]        & 0xff;
			name[5] = (mmc->cid[2] >> 24) & 0xff;
			mib = (unsigned long)(mmc->capacity_user / (1024 * 1024));

			if (mfg)
				snprintf(buf, sizeof(buf),
					 "eMMC     : %s %s, %lu MiB",
					 mfg, name, mib);
			else
				snprintf(buf, sizeof(buf),
					 "eMMC     : %s (MID 0x%02x), %lu MiB",
					 name, mmc->cid[0] >> 24, mib);
			vc_at(row++, 0);
			vc_puts(buf);

			/* Negotiated bus mode, not the advertised one — a
			 * failed mode switch silently drops to something
			 * slower, so this is worth seeing. Expected HS52
			 * 8-bit; anything less means a degraded bus. */
			if (mmc->has_init)
				snprintf(buf, sizeof(buf),
					 "eMMC bus : %s, %d-bit",
					 mmc_mode_name(mmc->selected_mode),
					 mmc->bus_width);
			else
				snprintf(buf, sizeof(buf),
					 "eMMC bus : not initialised");
			vc_at(row++, 0);
			vc_puts(buf);
		}
	}

	/* eFuse serials, published at misc_init_r. serial# is the usid adb
	 * reports; f_serial is the date-coded factory trace serial. */
	{
		const char *s = env_get("serial#");

		if (s) {
			snprintf(buf, sizeof(buf), "Serial   : %s", s);
			vc_at(row++, 0);
			vc_puts(buf);
		}
		s = env_get("f_serial");
		if (s) {
			snprintf(buf, sizeof(buf), "F-serial : %s", s);
			vc_at(row++, 0);
			vc_puts(buf);
		}
	}
}

static void draw_hwinfo_page1(void)
{
	struct carthing_charger_info ch;
	char buf[64];
	int row = 2;
	bool charger_ok;

	vc_at(row++, 0);
	vc_puts("I2C@bus2 (EE):");
	charger_ok = (carthing_charger_read(&ch) == 0 && ch.valid);
	if (charger_ok)
		snprintf(buf, sizeof(buf),
			 "  0x35 chrg MAX14656 rev %x",
			 ch.regs[0] & 0xf);
	else
		snprintf(buf, sizeof(buf), "  0x35 chrg MAX14656 (no resp)");
	vc_at(row++, 0);
	vc_puts(buf);
	vc_at(row++, 0);
	vc_puts("  0x39 prox/ALS TMD2772");

	vc_at(row++, 0);
	vc_puts("I2C@bus0 : 0x2e TLSC6X touch");
	vc_at(row++, 0);
	vc_puts("I2C@bus3 : 0x10 Apple MFi (idle)");

	row++;
	vc_at(row++, 0);
	vc_puts("Undriven : BT (BCM, SDIO+UART_AO)");
	vc_at(row++, 0);
	vc_puts("           PDM mics (4-mic array)");
}

/* Each counter value gives a distinct deterministic challenge, so a rotate-
 * press demo signs something visibly fresh each time. */
static void challenge_from_counter(uint32_t counter,
				   uint8_t out[MFI_CHALLENGE_SIZE])
{
	uint8_t seed[4] = {
		counter >> 24, counter >> 16, counter >> 8, counter
	};

	sha256_csum_wd(seed, sizeof(seed), out, CHUNKSZ_SHA256);
}

static int refresh_chr_cache(void)
{
	uint32_t t0;
	int ret;

	hwinfo_chr_cache.valid = false;
	challenge_from_counter(hwinfo_chr_cache.counter,
			       hwinfo_chr_cache.challenge);

	t0 = get_timer(0);
	ret = carthing_mfi_challenge_response(hwinfo_chr_cache.challenge,
					      hwinfo_chr_cache.response);
	hwinfo_chr_cache.elapsed_ms = get_timer(t0);
	if (ret < 0)
		return ret;
	hwinfo_chr_cache.valid = true;
	hwinfo_chr_cache.counter++;
	return 0;
}

static void hex_byte(char *out, uint8_t b)
{
	static const char *hex = "0123456789abcdef";

	out[0] = hex[b >> 4];
	out[1] = hex[b & 0xf];
}

/* ASN.1 "YYMMDDHHMMSSZ" / "YYYYMMDDHHMMSSZ" -> "YYYY-MM-DD", or "?". */
static const char *fmt_time(const char *t, char buf[16])
{
	int yy, mm, dd, year;
	size_t len = strlen(t);

	if (len < 12)
		return "?";
	if (len == 13 || len == 12) {
		yy = (t[0] - '0') * 10 + (t[1] - '0');
		mm = (t[2] - '0') * 10 + (t[3] - '0');
		dd = (t[4] - '0') * 10 + (t[5] - '0');
		year = (yy < 50) ? 2000 + yy : 1900 + yy;
	} else if (len >= 14) {
		year = (t[0] - '0') * 1000 + (t[1] - '0') * 100 +
		       (t[2] - '0') * 10 + (t[3] - '0');
		mm = (t[4] - '0') * 10 + (t[5] - '0');
		dd = (t[6] - '0') * 10 + (t[7] - '0');
	} else {
		return "?";
	}
	snprintf(buf, 16, "%04d-%02d-%02d", year, mm, dd);
	return buf;
}

/* "Apple Accessories Certification Authority - 00000002" is 52 chars and
 * overflows the column budget; the abbreviation still reads unambiguously. */
static const char *abbrev_issuer(const char *full)
{
	static char buf[48];
	const char *suffix;

	if (!full || !full[0])
		return "(parse failed)";
	suffix = strstr(full, " - ");
	if (strstr(full, "Apple Accessories")) {
		if (suffix)
			snprintf(buf, sizeof(buf),
				 "Apple Accessories CA%s", suffix);
		else
			snprintf(buf, sizeof(buf),
				 "Apple Accessories CA");
		return buf;
	}
	return full;
}

static void draw_hwinfo_page2(void)
{
	char buf[64], tbuf1[16], tbuf2[16], serial[MFI_SERIAL_SIZE + 1];
	int row = 2;

	vc_at(row++, 0);
	vc_puts("MFi 3.0 / CP3.0 @ i2c3 0x10");

	if (!hwinfo_mfi_cache.valid) {
		vc_at(row++, 0);
		vc_puts("(read failed)");
		return;
	}

	snprintf(buf, sizeof(buf), "Ver: 0x%02x  Cert: %u B",
		 hwinfo_mfi_cache.version, hwinfo_mfi_cache.cert_len);
	vc_at(row++, 0);
	vc_puts(buf);

	/* Chip serial register: 32 ASCII chars of device UID hex. */
	memcpy(serial, hwinfo_mfi_cache.serial, MFI_SERIAL_SIZE);
	serial[MFI_SERIAL_SIZE] = '\0';
	vc_at(row++, 0);
	vc_puts("Serial:");
	vc_at(row++, 0);
	vc_puts("  ");
	vc_at(row - 1, 2);
	vc_puts(serial);

	vc_at(row++, 0);
	vc_puts("Subject:");
	snprintf(buf, sizeof(buf), "  %s",
		 hwinfo_mfi_cache.parsed.subject_cn[0]
			? hwinfo_mfi_cache.parsed.subject_cn
			: "(parse failed)");
	vc_at(row++, 0);
	vc_puts(buf);

	vc_at(row++, 0);
	vc_puts("Issuer:");
	snprintf(buf, sizeof(buf), "  %s",
		 abbrev_issuer(hwinfo_mfi_cache.parsed.issuer_cn));
	vc_at(row++, 0);
	vc_puts(buf);

	snprintf(buf, sizeof(buf), "Valid: %s -> %s",
		 fmt_time(hwinfo_mfi_cache.parsed.not_before, tbuf1),
		 fmt_time(hwinfo_mfi_cache.parsed.not_after, tbuf2));
	vc_at(row++, 0);
	vc_puts(buf);
}

static int max_cert_scroll(void)
{
	int total_lines;

	if (!hwinfo_mfi_cache.valid)
		return 0;
	total_lines = (hwinfo_mfi_cache.cert_len + CERT_HEX_BYTES_PER_ROW - 1)
		      / CERT_HEX_BYTES_PER_ROW;
	if (total_lines <= CERT_HEX_VISIBLE_ROWS)
		return 0;
	return total_lines - CERT_HEX_VISIBLE_ROWS;
}

/* Full-screen modal, not one of the rotate-pages. */
static void draw_cert_hex(void)
{
	char buf[64];
	char *p;
	const char *sep = "--------------------------------------------------";
	int start_byte, end_byte, max, i, j;

	if (!hwinfo_mfi_cache.valid)
		return;

	vc_clear();

	start_byte = cert_scroll_lines * CERT_HEX_BYTES_PER_ROW;
	end_byte = start_byte + CERT_HEX_VISIBLE_ROWS * CERT_HEX_BYTES_PER_ROW;
	if (end_byte > hwinfo_mfi_cache.cert_len)
		end_byte = hwinfo_mfi_cache.cert_len;
	max = hwinfo_mfi_cache.cert_len;

	snprintf(buf, sizeof(buf),
		 "MFi Cert hex      0x%03x-0x%03x/0x%03x",
		 start_byte, end_byte, max);
	vc_at(0, 0);
	vc_puts(buf);
	vc_at(1, 0);
	vc_puts(sep);

	for (i = 0; i < CERT_HEX_VISIBLE_ROWS; i++) {
		int off = start_byte + i * CERT_HEX_BYTES_PER_ROW;

		if (off >= max)
			break;
		p = buf;
		p += snprintf(p, sizeof(buf), "%04x ", off);
		for (j = 0; j < CERT_HEX_BYTES_PER_ROW; j++) {
			if (off + j >= max)
				break;
			hex_byte(p, hwinfo_mfi_cache.cert[off + j]);
			p += 2;
		}
		*p = '\0';
		vc_at(2 + i, 0);
		vc_puts(buf);
	}

	{
		const char *exit_hint = "Back: MFi";
		const char *knob_hint = "Rotate: scroll  Press: refresh";
		char line[VC_COLS + 1];
		int gap = (VC_COLS - 1) - (int)strlen(knob_hint) -
			  (int)strlen(exit_hint);

		if (gap < 2)
			gap = 2;
		snprintf(line, sizeof(line), "%s%*s%s",
			 knob_hint, gap, "", exit_hint);
		vc_at(VC_ROWS - 1, 0);
		vc_puts(line);
	}

	vc_sync();
}

static int refresh_qr_cache(void)
{
	hwinfo_qr_cache.valid = false;
	hwinfo_qr_cache.json_len = carthing_hwinfo_build_json(
		hwinfo_qr_cache.json, sizeof(hwinfo_qr_cache.json));
	if (!hwinfo_qr_cache.json_len)
		return -1;
	if (!qrcodegen_encodeText(hwinfo_qr_cache.json,
				  hwinfo_qr_cache.tmp,
				  hwinfo_qr_cache.qr,
				  qrcodegen_Ecc_MEDIUM,
				  qrcodegen_VERSION_MIN,
				  15,
				  qrcodegen_Mask_AUTO,
				  true))
		return -1;
	hwinfo_qr_cache.qr_size = qrcodegen_getSize(hwinfo_qr_cache.qr);
	hwinfo_qr_cache.valid = true;
	return 0;
}

/*
 * Direct-framebuffer QR renderer — bypasses vidconsole and writes pixels, so
 * each QR module becomes an NxN block sized to fit the panel with a 4-module
 * quiet zone.
 */
static void draw_hwinfo_page5(void)
{
	struct video_priv *vid_priv;
	struct udevice *vdev;
	int total, px_per_mod, side_px, ox, oy;
	int x, y, ymod, xmod;
	uint32_t *fb;
	uint32_t white, black;

	if (!hwinfo_qr_cache.valid) {
		vc_at(2, 0);
		vc_puts("(QR build failed)");
		return;
	}

	if (uclass_first_device_err(UCLASS_VIDEO, &vdev))
		return;
	vid_priv = dev_get_uclass_priv(vdev);
	if (!vid_priv || !vid_priv->fb)
		return;

	total = hwinfo_qr_cache.qr_size + 8;	/* + quiet zone */

	/* The FB is natively portrait (480x800) even though the console runs
	 * landscape, so budget from the SHORTER native dimension. */
	{
		int short_side = vid_priv->xsize < vid_priv->ysize
				? vid_priv->xsize : vid_priv->ysize;

		px_per_mod = short_side / total;
		if (px_per_mod < 1)
			px_per_mod = 1;
	}
	side_px = total * px_per_mod;

	ox = (vid_priv->xsize - side_px) / 2;
	oy = (vid_priv->ysize - side_px) / 2;
	if (ox < 0)
		ox = 0;
	if (oy < 0)
		oy = 0;

	/* 32 BPP, 4 B/px, ARGB8888-ish; alpha 0xff = fully opaque. */
	fb = (uint32_t *)vid_priv->fb;
	white = 0xffffffff;
	black = 0xff000000;
	{
		int pixel_pitch = vid_priv->line_length / 4;

		for (y = 0; y < side_px; y++) {
			uint32_t *row = fb + (oy + y) * pixel_pitch + ox;

			for (x = 0; x < side_px; x++)
				row[x] = white;
		}

		for (ymod = 0; ymod < hwinfo_qr_cache.qr_size; ymod++) {
			for (xmod = 0; xmod < hwinfo_qr_cache.qr_size; xmod++) {
				int px_x, px_y, j;

				if (!qrcodegen_getModule(hwinfo_qr_cache.qr,
							 xmod, ymod))
					continue;
				px_x = ox + (xmod + 4) * px_per_mod;
				px_y = oy + (ymod + 4) * px_per_mod;

				for (j = 0; j < px_per_mod; j++) {
					uint32_t *row = fb +
						(px_y + j) * pixel_pitch + px_x;
					int k;

					for (k = 0; k < px_per_mod; k++)
						row[k] = black;
				}
			}
		}
	}

	/* Mark damage so video_sync actually flushes what we poked in. */
	if (vid_priv->damage.xend == 0)
		vid_priv->damage.xend = vid_priv->xsize;
	if (vid_priv->damage.yend == 0)
		vid_priv->damage.yend = vid_priv->ysize;
}

static int refresh_adc_cache(void)
{
	struct udevice *adcdev;
	int i;

	hwinfo_adc.valid = false;
	if (uclass_get_device_by_name(UCLASS_ADC, "adc@9000", &adcdev))
		return -1;
	for (i = 0; i < 8; i++) {
		if (adc_channel_single_shot("adc@9000", i, &hwinfo_adc.raw[i]))
			return -1;
	}
	hwinfo_adc.valid = true;
	return 0;
}

/* Vref ~1.8 V, 12-bit. */
static int adc_raw_to_mv(uint raw)
{
	return (int)((raw * 1800) / 4095);
}

/*
 * NTC thermistor table from the vendor kernel DT (rt1/rt2/rt3
 * `temperature-lookup-table`), vendor 10-bit raw -> °C. Higher raw is hotter
 * (NTC wired as the upper divider). -40..+85 °C in 5 °C steps, interpolated.
 */
static const struct { int raw; int c; } ntc_lut[] = {
	{ 894,  85 }, { 878,  80 }, { 860,  75 }, { 839,  70 },
	{ 815,  65 }, { 789,  60 }, { 759,  55 }, { 726,  50 },
	{ 690,  45 }, { 650,  40 }, { 607,  35 }, { 560,  30 },
	{ 512,  25 }, { 462,  20 }, { 411,  15 }, { 360,  10 },
	{ 311,   5 }, { 265,   0 }, { 221,  -5 }, { 182, -10 },
	{ 147, -15 }, { 117, -20 }, {  92, -25 }, {  70, -30 },
	{  53, -35 }, {  40, -40 },
};

/* Returns 0 and writes *c_out, or -1 if out of range. Takes 12-bit raw. */
static int ntc_raw_to_c(uint raw12, int *c_out)
{
	int raw10 = (int)(raw12 >> 2);
	int n = (int)(sizeof(ntc_lut) / sizeof(ntc_lut[0]));
	int i;

	if (raw10 >= ntc_lut[0].raw) {
		*c_out = ntc_lut[0].c;
		return 0;
	}
	if (raw10 <= ntc_lut[n - 1].raw) {
		*c_out = ntc_lut[n - 1].c;
		return 0;
	}
	for (i = 0; i < n - 1; i++) {
		int a = ntc_lut[i].raw, b = ntc_lut[i + 1].raw;

		if (raw10 <= a && raw10 >= b) {
			int span = a - b;
			int frac = a - raw10;
			int c_a = ntc_lut[i].c;
			int c_b = ntc_lut[i + 1].c;

			*c_out = c_a + ((c_b - c_a) * frac) / span;
			return 0;
		}
	}
	return -1;
}

/* Per-channel meaning, from vendor kernel DT (superbird_evt_512.dts). */
struct adc_chan_meta {
	const char *label;
	bool is_ntc;
};

static const struct adc_chan_meta adc_channels[8] = {
	[0] = { "PCB   ", true  },	/* rt3 = pcb_thermal */
	[1] = { "rev   ", false },	/* board-revision divider */
	[2] = { "BT    ", true  },	/* rt1 = bluetooth_thermal */
	[3] = { "DRAM  ", true  },	/* rt2 = dram_thermal */
	[4] = { "unused", false },
	[5] = { "unused", false },
	[6] = { "unused", false },
	[7] = { "unused", false },
};

static void draw_hwinfo_page4(void)
{
	char buf[64];
	int row = 2, i;

	vc_at(row++, 0);
	vc_puts("SARADC channels (Vref 1.8 V, 12-bit)");

	if (!hwinfo_adc.valid) {
		vc_at(row++, 0);
		vc_puts("(read failed)");
		return;
	}

	/* ch4-7 aren't referenced in the vendor DT — they float at ESD-clamp /
	 * leakage voltage and would just be noise on the page. */
	for (i = 0; i < 4; i++) {
		const struct adc_chan_meta *m = &adc_channels[i];
		char note[16] = "";
		int mv = adc_raw_to_mv(hwinfo_adc.raw[i]);

		if (i == 1) {
			int rev = carthing_probe_board_rev();

			if (rev > 0)
				snprintf(note, sizeof(note), " REV %d", rev);
			else
				snprintf(note, sizeof(note), " REV ?");
		} else if (m->is_ntc) {
			int c;

			if (ntc_raw_to_c(hwinfo_adc.raw[i], &c) == 0)
				snprintf(note, sizeof(note), " ~%d C", c);
		}
		snprintf(buf, sizeof(buf), "ch%d %s: %4u raw %4d mV%s",
			 i, m->label, hwinfo_adc.raw[i], mv, note);
		vc_at(row++, 0);
		vc_puts(buf);
	}
}

static void draw_hwinfo_page3(void)
{
	char buf[64];
	int row = 2, i;

	vc_at(row++, 0);
	vc_puts("MFi Challenge / Response");
	row++;

	if (!hwinfo_chr_cache.valid) {
		vc_at(row++, 0);
		vc_puts("(signing failed or not run)");
		return;
	}

	vc_at(row++, 0);
	vc_puts("Challenge (32B):");
	for (i = 0; i < 2; i++) {
		int j;
		char *p = buf;

		*p++ = ' ';
		*p++ = ' ';
		for (j = 0; j < 16; j++) {
			hex_byte(p, hwinfo_chr_cache.challenge[i * 16 + j]);
			p += 2;
		}
		*p = '\0';
		vc_at(row++, 0);
		vc_puts(buf);
	}

	vc_at(row++, 0);
	vc_puts("Signature (64B, ECDSA P-256):");
	for (i = 0; i < 4; i++) {
		int j;
		char *p = buf;

		*p++ = ' ';
		*p++ = ' ';
		for (j = 0; j < 16; j++) {
			hex_byte(p, hwinfo_chr_cache.response[i * 16 + j]);
			p += 2;
		}
		*p = '\0';
		vc_at(row++, 0);
		vc_puts(buf);
	}

	snprintf(buf, sizeof(buf), "Signed in %u ms",
		 hwinfo_chr_cache.elapsed_ms);
	vc_at(row++, 0);
	vc_puts(buf);
}

static void draw_hwinfo(void)
{
	const char *exit_hint = "Back: exit";
	const char *knob_hint;

	vc_clear();
	draw_hwinfo_chrome(hwinfo_page);

	switch (hwinfo_page) {
	case 0: draw_hwinfo_page0(); break;
	case 1: draw_hwinfo_page1(); break;
	case 2: draw_hwinfo_page2(); break;
	case 3: draw_hwinfo_page3(); break;
	case 4: draw_hwinfo_page4(); break;
	case 5: draw_hwinfo_page5(); break;
	}

	switch (hwinfo_page) {
	case 2: knob_hint = "Rotate: page   Press: full cert"; break;
	case 3: knob_hint = "Rotate: page   Press: re-sign";   break;
	default: knob_hint = "Rotate: page   Press: refresh";  break;
	}

	/* One string, knob hint left and exit hint right-justified, capped at
	 * VC_COLS-1 — a char in the last column wraps and scrolls the screen. */
	{
		char line[VC_COLS + 1];
		int gap = (VC_COLS - 1) - (int)strlen(knob_hint) -
			  (int)strlen(exit_hint);

		if (gap < 2)
			gap = 2;
		snprintf(line, sizeof(line), "%s%*s%s",
			 knob_hint, gap, "", exit_hint);
		vc_at(VC_ROWS - 1, 0);
		vc_puts(line);
	}

	vc_sync();
}

/* Pages 3 and 5 are expensive, so fill them on first visit rather than up
 * front. Pages 0/1/2/4 are covered by the caches primed on entry. */
static void ensure_page_cache(int page)
{
	if (page == 3 && !hwinfo_chr_cache.valid)
		(void)refresh_chr_cache();
	if (page == 5 && !hwinfo_qr_cache.valid)
		(void)refresh_qr_cache();
}

void menu_hwinfo_run(struct menu_btns *b)
{
	bool in_cert_modal = false;

	(void)refresh_mfi_cache();
	(void)refresh_adc_cache();
	(void)carthing_panel_probe(&hwinfo_panel_cache);

	draw_hwinfo();
	menu_btns_drain(b);

	while (1) {
		int dt;

		mdelay(5);

		if (btn_edge(&b->back) || ctrlc()) {
			/* Back leaves the modal first, then the screen. */
			if (in_cert_modal) {
				in_cert_modal = false;
				draw_hwinfo();
				continue;
			}
			return;
		}

		dt = wheel_poll_detents();
		if (dt != 0) {
			if (in_cert_modal) {
				int max = max_cert_scroll();

				cert_scroll_lines += dt;
				if (cert_scroll_lines < 0)
					cert_scroll_lines = 0;
				if (cert_scroll_lines > max)
					cert_scroll_lines = max;
				draw_cert_hex();
			} else {
				hwinfo_page = ((hwinfo_page + dt) % HWINFO_NPAGES
					       + HWINFO_NPAGES) % HWINFO_NPAGES;
				ensure_page_cache(hwinfo_page);
				draw_hwinfo();
			}
		}

		if (btn_edge(&b->sel)) {
			if (in_cert_modal) {
				(void)refresh_mfi_cache();
				draw_cert_hex();
			} else if (hwinfo_page == 2) {
				in_cert_modal = true;
				cert_scroll_lines = 0;
				draw_cert_hex();
			} else {
				if (hwinfo_page == 3)
					(void)refresh_chr_cache();
				else if (hwinfo_page == 4)
					(void)refresh_adc_cache();
				else if (hwinfo_page == 5)
					(void)refresh_qr_cache();
				draw_hwinfo();
			}
		}
	}
}
