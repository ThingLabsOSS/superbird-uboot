// SPDX-License-Identifier: GPL-2.0+
/*
 * Sitronix ST7701S MIPI-DSI panel driver — Spotify Car Thing variant
 * (vendor calls this `panel_type=lcd_8`).
 *
 * 480x800, 2-lane MIPI DSI, ~31.6 MHz pixel clock. The init command
 * sequence is the byte-for-byte copy from the vendor 2015.01 u-boot
 * (board/amlogic/superbird_production/lcd_extern.h, table
 * `ext_init_on_table_ST7701S`) — every other ST7701S variant in vendor
 * source has a different gamma/Vcom/GIP table, so this MUST stay
 * carthing-specific.
 *
 * Power-on sequence (from vendor `lcd_power_on_step_ST7701S`):
 *   1. VCC GPIO -> 1 (release), wait 1ms       [vendor uses active-low VCC]
 *   2. RESET GPIO -> 0 (assert), wait 10ms
 *   3. RESET GPIO -> 1 (release), wait 120ms
 *   4. Send DSI init table
 *   5. wait 200ms ("signal" step in vendor)
 *
 * Vendor wires:
 *   - LCD reset = GPIOZ_5 (active-low)
 *   - LCD power = GPIOZ_6 (active-low — driving it 0 = powered on)
 */

#include <backlight.h>
#include <dm.h>
#include <log.h>
#include <mipi_display.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <asm/gpio.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <power/regulator.h>

/*
 * Vendor's init sequence, verbatim. Format is `<type>, <len>, <payload...>`:
 *   - 0x05 = DCS short write, 0 params (cmd byte only)
 *   - 0x15 = DCS short write, 1 param (cmd + 1 data byte)
 *   - 0x39 = DCS long write (cmd + N-1 data bytes; `len` is total)
 *   - 0xfd = millisecond delay (one byte payload = ms)
 *   - 0xff = end of table
 */
static const u8 st7701s_carthing_init[] = {
	/*
	 * COMPLETE SHIPPING init, reverse-engineered from the stock BL33 and
	 * cross-checked across FOUR independent sources that all agree (2021
	 * BL33 binary, 2024 BL33 binary, this unit's live /lcd_extern/extern_7,
	 * another unit's uboot-booted.dtb). This REPLACES the table previously
	 * copied from vendor *source* (board/amlogic/superbird_production/
	 * lcd_extern.h), which had drifted from what actually ships — the
	 * drifted values were the root cause of ghosting/retention + lines.
	 * Differences vs that source-derived table are marked `<< FIX`.
	 *
	 * Structure mirrors stock exactly: the panel-level "wake + display on"
	 * (vendor dsi_init_on) first, THEN the big extern gamma/Vcom/GIP block
	 * (vendor ext_init_on). Two sleep-outs / two display-ons total — this is
	 * intentional and matches shipping; do not "clean it up". Panel-side
	 * MADCTL rotation (0x36) produced no visible effect on this variant and
	 * stock doesn't set it either, so we render native portrait; downstream
	 * (Linux drm rotation / vendor kernel OSD-plane rotate) handles landscape.
	 *
	 * Wire-fidelity note: the 2-byte writes are emitted as MIPI long (0x39)
	 * packets to mirror stock; mipi_dsi_dcs_write() downgrades the 1-data-byte
	 * ones to short (0x15) on the wire, which the ST7701S accepts. Leave as-is.
	 */
	/* ---- panel dsi_init_on (vendor mipi_init_on_table_ST7701S) ---- */
	0x05, 1,  0x11,					/* sleep out */
	0xfd, 1,  120,					/* >=120 ms after sleep-out (datasheet) */
	0x05, 1,  0x29,					/* display on */

	/* ---- extern init_on (vendor ext_init_on_table_ST7701S) ----
	 * Stock ext table starts straight at the BK3 select — NO extra
	 * `sleep-out + 50 ms` here (the source-derived table added one). << FIX
	 */
	0x39, 6,  0xff, 0x77, 0x01, 0x00, 0x00, 0x13,	/* CMD2BKSEL -> BK3 */
	0x39, 2,  0xef, 0x08,
	0x39, 6,  0xff, 0x77, 0x01, 0x00, 0x00, 0x10,	/* -> BK0 */
	0x39, 3,  0xc0, 0x63, 0x00,			/* LNSET (800 lines) */
	0x39, 3,  0xc1, 0x0a, 0x02,			/* PORCTRL */
	0x39, 3,  0xc2, 0x01, 0x07,			/* INVSET: NLINV=1 (2-dot), RTNI=7 -> 624 pclk/line  << FIX was 0x01,0x02 (RTNI=2/544, undercharge -> lines) */
	0x39, 2,  0xcc, 0x10,
	/* positive-gamma -- measured SHIPPING curve, no toe-lift (a +2 LSB
	 * toe-lift on gray 0/4/8 was tried + reverted: washed darks, no gain) */
	0x39, 17, 0xb0, 0x0a, 0x10, 0x96, 0x0e, 0x11, 0x06, 0x06,
		  0x0a, 0x08, 0x24, 0x07, 0x53, 0x11, 0x68, 0xad, 0xd1,
	/* negative-gamma -- shipping curve, symmetric with B0 (no toe-lift) */
	0x39, 17, 0xb1, 0x06, 0x8d, 0xd3, 0x09, 0x0d, 0x04, 0x04,
		  0x07, 0x07, 0x23, 0x05, 0x15, 0x14, 0xa7, 0xac, 0x4f,
	0x39, 6,  0xff, 0x77, 0x01, 0x00, 0x00, 0x11,	/* -> BK1 */
	0x39, 2,  0xb0, 0x5d,
	0x39, 2,  0xb1, 0x45,				/* VCOM = 0.96 V   << FIX was 0x38 (0.80 V -> DC bias -> ghosting/retention) */
	0x39, 2,  0xb2, 0x82,				/* VGH  = 12.5 V   << FIX was 0x84 (13.5 V; keep paired with VCOM above) */
	0x39, 2,  0xb3, 0x80,
	0x39, 2,  0xb5, 0x45,				/* VGL */
	0x39, 2,  0xb7, 0x85,
	0x39, 2,  0xb8, 0x21,
	0x39, 3,  0xb9, 0x10, 0x1f,
	0x39, 2,  0xbb, 0x03,
	0x39, 2,  0xbc, 0x3e,
	0x39, 2,  0xc1, 0x78,				/* (BK1 C1 = SPD1)  << FIX source added a stray C0 0x89 before this; stock has none */
	0x39, 2,  0xc2, 0x78,				/* (BK1 C2 = SPD2) */
	0x39, 2,  0xd0, 0x88,
	0xfd, 1,  8,					/* << FIX was 20 ms */
	/* GIP setting */
	0x39, 4,  0xe0, 0x00, 0x00, 0x02,
	0x39, 12, 0xe1, 0x04, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
		  0x00, 0x00, 0x20, 0x20,
	0x39, 13, 0xe2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00,		/* << FIX was len 14 (one extra 0x00) */
	0x39, 5,  0xe3, 0x00, 0x00, 0x33, 0x00,
	0x39, 3,  0xe4, 0x22, 0x00,
	0x39, 17, 0xe5, 0x04, 0x34, 0x9a, 0xa0, 0x06, 0x34, 0x9a,
		  0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* GIP gate timing  << FIX was af b3 -> 9a a0 (lines) */
	0x39, 5,  0xe6, 0x00, 0x00, 0x33, 0x00,
	0x39, 3,  0xe7, 0x22, 0x00,
	0x39, 17, 0xe8, 0x05, 0x34, 0x9a, 0xa0, 0x07, 0x34, 0x9a,
		  0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,	/* GIP gate timing  << FIX */
	0x39, 8,  0xeb, 0x02, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00,
	0x39, 3,  0xec, 0x00, 0x00,
	0x39, 17, 0xed, 0xfa, 0x45, 0x0b, 0xff, 0xff, 0xff, 0xff,
		  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xb0, 0x54, 0xaf,
	0x39, 7,  0xef, 0x08, 0x08, 0x08, 0x45, 0x3f, 0x54,	/* << FIX was 0x4b not 0x45, and placed BEFORE the E0..ED block; stock has it here, AFTER ED */
	0x39, 6,  0xff, 0x77, 0x01, 0x00, 0x00, 0x13,	/* -> BK3 */
	0x39, 3,  0xe8, 0x00, 0x0e,
	0x39, 6,  0xff, 0x77, 0x01, 0x00, 0x00, 0x00,	/* -> disable Command2 */
	0x39, 2,  0x11, 0x00,				/* sleep out (sent as [0x11,0x00], stock form) */
	0xfd, 1,  120,					/* 120 ms   << FIX was 30 ms (too short after sleep-out) */
	0x39, 6,  0xff, 0x77, 0x01, 0x00, 0x00, 0x13,	/* -> BK3 */
	0x39, 3,  0xe8, 0x00, 0x0c,
	0xfd, 1,  2,					/* << FIX was 5 ms */
	0x39, 3,  0xe8, 0x00, 0x00,
	0x39, 6,  0xff, 0x77, 0x01, 0x00, 0x00, 0x00,	/* -> disable Command2 */
	0x39, 2,  0x35, 0x00,				/* tear effect on */
	0x39, 2,  0x29, 0x00,				/* display on */
	0xfd, 1,  15,					/* << FIX was 30 ms */
	0xff, 0,					/* end (351 bytes of ext table, matches shipping) */
};

/*
 * NB: these are NOT the values in vendor's open-source `ext_lcd_config[lcd_8]`
 * (which says h_period=630, v_period=836, pclk=31.6MHz, vsync_len=4).
 *
 * They're the values the shipping firmware actually programs into ENCL on
 * a running stock unit, observed by dropping the device into vendor's
 * u-boot CLI and dumping registers. The open-source source has drifted
 * from what Spotify is actually shipping.
 *
 * Derived from these live register reads:
 *   ENCL_VIDEO_MAX_PXCNT   = 0x225  -> htotal     = 550
 *   ENCL_VIDEO_MAX_LNCNT   = 0x34d  -> vtotal     = 846
 *   ENCL_VIDEO_HAVON_BEGIN = 0x028  -> hsync_len+hbp = 40
 *   ENCL_VIDEO_HSO_END     = 0x00a  -> hsync_len  = 10  (so hbp = 30)
 *   ENCL_VIDEO_VAVON_BLINE = 0x01a  -> vsync_len+vbp = 26
 *   ENCL_VIDEO_VSO_ELINE   = 0x006  -> vsync_len  = 6   (so vbp = 20)
 *
 * pclk = htotal * vtotal * 60 Hz = 550 * 846 * 60 = 27.918 MHz.
 *
 * Full discussion in superbird-docs/uboot/spotify-carthing-display-notes.md.
 */
static const struct display_timing st7701s_carthing_timing = {
	.pixelclock.typ		= 27918000,
	.hactive.typ		= 480,
	.hfront_porch.typ	= 30,
	.hback_porch.typ	= 30,
	.hsync_len.typ		= 10,
	.vactive.typ		= 800,
	.vfront_porch.typ	= 20,
	.vback_porch.typ	= 20,
	.vsync_len.typ		= 6,
};

struct st7701s_carthing_priv {
	struct udevice *backlight;
	struct gpio_desc reset;
	struct gpio_desc vcc;
};

static int st7701s_send_init_table(struct mipi_dsi_device *dsi)
{
	const u8 *t = st7701s_carthing_init;
	int ret;

	while (*t != 0xff) {
		u8 type = t[0];
		u8 len = t[1];
		const u8 *payload = &t[2];

		switch (type) {
		case 0xfd:
			mdelay(payload[0]);
			break;
		case 0x05:	/* DCS short write 0 params */
			ret = mipi_dsi_dcs_write(dsi, payload[0], NULL, 0);
			if (ret < 0)
				return ret;
			break;
		case 0x15:	/* DCS short write 1 param */
			ret = mipi_dsi_dcs_write(dsi, payload[0], &payload[1], 1);
			if (ret < 0)
				return ret;
			break;
		case 0x39:	/* DCS long write */
			ret = mipi_dsi_dcs_write(dsi, payload[0], &payload[1],
						 len - 1);
			if (ret < 0)
				return ret;
			break;
		default:
			log_err("st7701s: unknown init type 0x%02x\n", type);
			return -EINVAL;
		}

		t += 2 + len;
	}

	return 0;
}

/*
 * Diagnostic: read back the ST7701S status registers via DCS once the init
 * table has been sent and we're still in command mode. This serves two ends:
 *
 *   1. It forces any still-pending command TX to fully complete (the read's
 *      bus-turnaround can't happen until the host is idle), doubling as a belt
 *      for the host-side quiesce in dw_mipi_dsi_set_mode().
 *   2. It gives a per-boot fingerprint for the rare (~1/150) bad DSI handoff
 *      that shifts the image + mangles colour. That failure is otherwise
 *      invisible before the OS adopts the link, so the log is the only cheap
 *      way to catch it.
 *
 * RDDPM (power mode) alone is NOT enough: on a confirmed bad-handoff boot
 * (2026-05-24) it still read a healthy 0x9c while the image was shifted +
 * miscoloured — so the corruption lives in a register RDDPM doesn't cover, or
 * in the panel's video-stream alignment (which no register reflects). We dump
 * the full status set so a glitched boot can be diffed against a known-good
 * baseline to pin which register (if any) is wrong.
 *
 * Healthy values for our config (ST7701S datasheet 12.2.8-12.2.13):
 *   RDDPM    0x0A = 0x9c  D7 booster / D4 sleep-out / D3 fixed-1 / D2 disp-on
 *   RDDMADCTL0x0B = 0x00  MADCTL not programmed (ML=D4, BGR=D3 clear)
 *   RDDCOLMOD0x0C = ----  VIPF[6:4] pixel format (log raw, baseline-compare)
 *   RDDIM    0x0D = 0x00  no inversion (D5), default gamma curve GCS[2:0]=0
 *   RDDSM    0x0E = 0x80  D7 tearing-effect-line on, D6 mode (we send TEON)
 *   RDDSDR   0x0F = 0xc0  D7 RLD + D6 FUND set: register-load & functionality OK
 * RDDPM != 0x9c and RDDSDR's self-diagnostic bits not both set log as
 * warnings; the rest are logged raw for cross-boot comparison. Non-fatal --
 * this only reports, it never blocks bring-up.
 */
#define ST7701S_RDDPM_HEALTHY	0x9c
#define ST7701S_RDDSDR_OK	0xc0	/* D7 RLD | D6 FUND both set */

static void st7701s_check_panel_state(struct mipi_dsi_device *dsi)
{
	static const struct { u8 cmd; const char *name; } regs[] = {
		{ MIPI_DCS_GET_POWER_MODE,        "RDDPM"     },
		{ MIPI_DCS_GET_ADDRESS_MODE,      "RDDMADCTL" },
		{ MIPI_DCS_GET_PIXEL_FORMAT,      "RDDCOLMOD" },
		{ MIPI_DCS_GET_DISPLAY_MODE,      "RDDIM"     },
		{ MIPI_DCS_GET_SIGNAL_MODE,       "RDDSM"     },
		{ MIPI_DCS_GET_DIAGNOSTIC_RESULT, "RDDSDR"    },
	};
	u8 vals[6] = { 0 };
	int i;

	for (i = 0; i < 6; i++) {
		u8 v = 0;

		if (mipi_dsi_dcs_read(dsi, regs[i].cmd, &v, 1) < 0)
			log_warning("st7701s: %s(0x%02x) read failed -- possible bad DSI handoff\n",
				    regs[i].name, regs[i].cmd);
		else
			vals[i] = v;
	}

	/* One compact per-boot line; build a baseline from healthy boots. */
	log_info("st7701s: panel state RDDPM=0x%02x MADCTL=0x%02x COLMOD=0x%02x IM=0x%02x SM=0x%02x SDR=0x%02x\n",
		 vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]);

	if (vals[0] != ST7701S_RDDPM_HEALTHY)
		log_warning("st7701s: RDDPM=0x%02x unexpected (want 0x%02x) -- panel power state off\n",
			    vals[0], ST7701S_RDDPM_HEALTHY);
	if ((vals[5] & ST7701S_RDDSDR_OK) != ST7701S_RDDSDR_OK)
		log_warning("st7701s: RDDSDR=0x%02x self-diagnostic fault (RLD/FUND not both set)\n",
			    vals[5]);
}

static int st7701s_carthing_enable_backlight(struct udevice *dev)
{
	struct st7701s_carthing_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct mipi_dsi_device *dsi = plat->device;
	int ret;

	/* Vendor lcd_power_on_step_ST7701S, step 1: VCC ON. */
	dm_gpio_set_value(&priv->vcc, 1);
	mdelay(1);

	/*
	 * Reset pulse, mirroring stock exactly: H(1ms) -> L(10ms) -> H(120ms).
	 * reset is GPIOD_ACTIVE_LOW, so logical 0 = released (pin high) and
	 * logical 1 = asserted (pin low).
	 */
	dm_gpio_set_value(&priv->reset, 0);	/* released (pin high), stock initial 1ms */
	mdelay(1);
	dm_gpio_set_value(&priv->reset, 1);	/* assert (pin low) */
	mdelay(10);
	dm_gpio_set_value(&priv->reset, 0);	/* release (pin high) */
	mdelay(120);

	ret = st7701s_send_init_table(dsi);
	if (ret) {
		log_err("st7701s: init table failed: %d\n", ret);
		return ret;
	}

	/* Diagnostic: fingerprint the panel state for bad-handoff detection. */
	st7701s_check_panel_state(dsi);

	if (priv->backlight) {
		ret = backlight_enable(priv->backlight);
		if (ret)
			log_warning("st7701s: backlight enable failed: %d\n", ret);
	}

	return 0;
}

static int st7701s_carthing_set_backlight(struct udevice *dev, int percent)
{
	struct st7701s_carthing_priv *priv = dev_get_priv(dev);

	if (!priv->backlight)
		return -ENODEV;
	return backlight_set_brightness(priv->backlight, percent);
}

static int st7701s_carthing_get_display_timing(struct udevice *dev,
					       struct display_timing *timing)
{
	memcpy(timing, &st7701s_carthing_timing, sizeof(*timing));
	return 0;
}

static int st7701s_carthing_of_to_plat(struct udevice *dev)
{
	struct st7701s_carthing_priv *priv = dev_get_priv(dev);
	int ret;

	ret = gpio_request_by_name(dev, "reset-gpios", 0, &priv->reset,
				   GPIOD_IS_OUT | GPIOD_ACTIVE_LOW);
	if (ret)
		return log_msg_ret("reset gpio", ret);

	ret = gpio_request_by_name(dev, "vcc-gpios", 0, &priv->vcc,
				   GPIOD_IS_OUT | GPIOD_ACTIVE_LOW);
	if (ret)
		return log_msg_ret("vcc gpio", ret);

	/* Backlight is optional at probe — may not exist yet. */
	uclass_get_device_by_phandle(UCLASS_PANEL_BACKLIGHT, dev, "backlight",
				     &priv->backlight);

	return 0;
}

static int st7701s_carthing_probe(struct udevice *dev)
{
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);

	plat->lanes = 2;
	plat->format = MIPI_DSI_FMT_RGB888;
	/*
	 * Despite the open-source vendor source (and Linux mainline) saying
	 * the ST7701S wants MIPI_DSI_CLOCK_NON_CONTINUOUS, a register dump
	 * from shipping firmware shows the carthing's actual panel runs
	 * with HS continuous (clk_always_hs=1 per `lcd info`, LPCLK_CTRL=0x1).
	 * Setting NON_CONTINUOUS here makes the LCD layer stay dark. See
	 * superbird-docs/uboot/spotify-carthing-display-notes.md.
	 */
	plat->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			   MIPI_DSI_MODE_LPM;

	return 0;
}

static const struct panel_ops st7701s_carthing_ops = {
	.enable_backlight	= st7701s_carthing_enable_backlight,
	.set_backlight		= st7701s_carthing_set_backlight,
	.get_display_timing	= st7701s_carthing_get_display_timing,
};

static const struct udevice_id st7701s_carthing_ids[] = {
	{ .compatible = "spotify,carthing-st7701s" },
	{ }
};

U_BOOT_DRIVER(sitronix_st7701s_carthing) = {
	.name		= "sitronix_st7701s_carthing",
	.id		= UCLASS_PANEL,
	.of_match	= st7701s_carthing_ids,
	.ops		= &st7701s_carthing_ops,
	.of_to_plat	= st7701s_carthing_of_to_plat,
	.probe		= st7701s_carthing_probe,
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
	.priv_auto	= sizeof(struct st7701s_carthing_priv),
};
