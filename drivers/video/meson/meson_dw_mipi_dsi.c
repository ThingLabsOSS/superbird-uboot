// SPDX-License-Identifier: GPL-2.0+
/*
 * Amlogic Meson G12A DesignWare MIPI-DSI host wrapper.
 *
 * The G12A MIPI-DSI block is the Synopsys DesignWare DSI host (handled by
 * drivers/video/dw_mipi_dsi.c) sitting inside a thin Amlogic glue wrapper
 * that owns:
 *
 *  - top-level reset (RESET_MIPI_DSI_HOST)
 *  - top-level sysclk/pixclk gates (MIPI_DSI_TOP_CLK_CNTL)
 *  - DPI color-mode register that bridges VENC pixel format into the DSI
 *    host (MIPI_DSI_TOP_CNTL)
 *  - HHI PLL config feeding the dphy bit clock and ENCL pixel clock
 *
 * Mapping in the SoC:
 *   - DT reg @ 0xffd07000, 4 KiB. The DWC IP lives at offsets 0x000..0x0f4;
 *     the Amlogic TOP wrapper registers live at offsets 0x3c0..0x3f4 in the
 *     same window. We bind the generic dw_mipi_dsi child driver to the same
 *     ofnode, so both drivers ioremap the same base.
 *
 * Wiring relative to dw_mipi_dsi.c (the IP layer):
 *   - We register ourselves as UCLASS_VIDEO_BRIDGE.
 *   - On .attach we look up the panel, fetch its display timing, ask the
 *     dw_mipi_dsi child to bring up the link via dsi_host_init() with our
 *     phy_ops (this is where the HHI clocks and the dphy are programmed).
 *   - On .set_backlight we call panel_enable_backlight() (panel driver
 *     pushes its init sequence onto the DSI host that's now live in
 *     command mode) and then dsi_host_enable() to switch to video mode.
 *
 * Reference: drivers/gpu/drm/meson/meson_dw_mipi_dsi.c in mainline Linux.
 * Closest u-boot analogue: drivers/video/stm32/stm32_dsi.c.
 */

#define LOG_CATEGORY UCLASS_VIDEO_BRIDGE

#include <dm.h>
#include <dsi_host.h>
#include <generic-phy.h>
#include <log.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <reset.h>
#include <video.h>
#include <video_bridge.h>
#include <asm/io.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>
#include <dm/lists.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <phy-mipi-dphy.h>

/* --- TOP wrapper registers (offsets within the shared 4 KiB window) --- */
#define MIPI_DSI_TOP_SW_RESET			0x3c0
#define  MIPI_DSI_TOP_SW_RESET_DWC		BIT(0)
#define  MIPI_DSI_TOP_SW_RESET_INTR		BIT(1)
#define  MIPI_DSI_TOP_SW_RESET_DPI		BIT(2)
#define  MIPI_DSI_TOP_SW_RESET_TIMING		BIT(3)

#define MIPI_DSI_TOP_CLK_CNTL			0x3c4
#define  MIPI_DSI_TOP_CLK_SYSCLK_EN		BIT(0)
#define  MIPI_DSI_TOP_CLK_PIXCLK_EN		BIT(1)

#define MIPI_DSI_TOP_CNTL			0x3c8
#define  MIPI_DSI_TOP_DPI_COLOR_MODE		GENMASK(23, 20)
#define  MIPI_DSI_TOP_IN_COLOR_MODE		GENMASK(18, 16)
#define  MIPI_DSI_TOP_COMP2_SEL			GENMASK(13, 12)
#define  MIPI_DSI_TOP_COMP1_SEL			GENMASK(11, 10)
#define  MIPI_DSI_TOP_COMP0_SEL			GENMASK(9, 8)
#define  MIPI_DSI_TOP_HSYNC_INVERT		BIT(5)
#define  MIPI_DSI_TOP_VSYNC_INVERT		BIT(4)

#define MIPI_DSI_TOP_MEM_PD			0x3f4

/* DPI color formats matching the TOP_CNTL.dpi_color_mode field. */
#define DPI_COLOR_18BIT_CFG_2			4
#define DPI_COLOR_24BIT				5

/* VENC input data width matching TOP_CNTL.in_color_mode. */
#define VENC_IN_COLOR_24B			1
#define VENC_IN_COLOR_18B			2

struct meson_dw_mipi_dsi_priv {
	void __iomem			*base;
	struct reset_ctl		top_rst;
	struct phy			dphy;

	struct mipi_dsi_device		device;
	struct udevice			*panel;
	struct udevice			*dsi_host;

	/* PHY config cached between get_lane_mbps and init. */
	struct phy_configure_opts_mipi_dphy dphy_cfg;
	unsigned int			lane_mbps;
	struct display_timing		timings;
};

/* --- HHI clock setup (STUB for D3, real values land in D4 alongside VPU) --- *
 *
 * What needs to happen here, derived from vendor lcd.c and Linux mainline:
 *
 *   1. GP0_PLL (or HIFI_PLL): set to ~1.5 GHz so a /2 div yields the dphy
 *      bit-clock target (~760 MHz for 31.6 MHz pclk * 24bpp / 2 lanes).
 *   2. HHI_MIPI_CNTL0/1/2: enable the analog mipi dphy, pick mipi_dsi_pll
 *      output as bit clock, configure /2 divider.
 *   3. HHI_VID_PLL_CLK_DIV: route GP0_PLL down to CTS_ENCL at the pixel rate.
 *   4. HHI_VIID_CLK_DIV / _CNTL: select that as the ENCL parent and enable.
 *   5. HHI_VID_CLK_CNTL2[3]: ungate ENCL.
 *
 * For D3 we *only* care that the wrapper structurally compiles and probes;
 * D4 (meson_vpu DSI output path) will own the actual register writes since
 * they're inseparable from the VENC/VCLK side of the bring-up.
 */
static void meson_dw_mipi_dsi_setup_clocks(struct udevice *dev,
					   unsigned long bit_clk_rate,
					   unsigned long px_clk_rate)
{
	dev_dbg(dev,
		"clock setup STUB: bit_clk=%lu Hz, px_clk=%lu Hz (real HHI writes land in D4)\n",
		bit_clk_rate, px_clk_rate);
}

/* --- Wrapper hardware bring-up --- */

static void meson_dw_mipi_dsi_hw_init(struct meson_dw_mipi_dsi_priv *priv)
{
	const u32 sw_reset_mask = MIPI_DSI_TOP_SW_RESET_DWC |
				  MIPI_DSI_TOP_SW_RESET_INTR |
				  MIPI_DSI_TOP_SW_RESET_DPI |
				  MIPI_DSI_TOP_SW_RESET_TIMING;
	const u32 clk_cntl_mask = MIPI_DSI_TOP_CLK_SYSCLK_EN |
				  MIPI_DSI_TOP_CLK_PIXCLK_EN;
	u32 v;

	/* Pulse all sub-block resets inside the DWC + Amlogic wrapper. */
	v = readl(priv->base + MIPI_DSI_TOP_SW_RESET);
	writel(v | sw_reset_mask, priv->base + MIPI_DSI_TOP_SW_RESET);
	writel(v & ~sw_reset_mask, priv->base + MIPI_DSI_TOP_SW_RESET);

	/* Enable manual sysclk + pixclk gates (DWC IP doesn't auto-gate). */
	v = readl(priv->base + MIPI_DSI_TOP_CLK_CNTL);
	writel(v | clk_cntl_mask, priv->base + MIPI_DSI_TOP_CLK_CNTL);

	/* Take the host RAM out of power-down. */
	writel(0, priv->base + MIPI_DSI_TOP_MEM_PD);
}

/* --- phy_ops passed into dw_mipi_dsi --- */

static int meson_dw_mipi_dsi_phy_get_lane_mbps(void *priv_data,
					       struct display_timing *timings,
					       u32 lanes, u32 format,
					       unsigned int *lane_mbps)
{
	struct mipi_dsi_device *device = priv_data;
	struct udevice *dev = device->dev;
	struct meson_dw_mipi_dsi_priv *priv = dev_get_priv(dev);
	int bpp;

	bpp = mipi_dsi_pixel_format_to_bpp(format);
	if (bpp < 0) {
		dev_err(dev, "unsupported MIPI DSI format 0x%x\n", format);
		return bpp;
	}

	/*
	 * Compute the default mipi-dphy config (hs_clk_rate, lp/hs timings,
	 * etc.) from the desired pixel rate. We stash the full opts on priv
	 * so phy_init can pass them to generic_phy_configure() unchanged.
	 */
	phy_mipi_dphy_get_default_config(timings->pixelclock.typ, bpp, lanes,
					 &priv->dphy_cfg);

	/*
	 * Override to match what the shipping carthing firmware uses: the
	 * theoretical-minimum bit rate (pclk * bpp / lanes ~= 335 Mbps) gives
	 * the ST7701S HS receivers no margin and the LCD layer stays dark.
	 * Vendor runs the dphy at FCLK_DIV3 = 666.67 MHz per lane, ~2x the
	 * minimum. We hardcode 666_666_666 to match meson_vclk_setup_dsi_lcd8.
	 * The DSI host's VID_HLINE_TIME / dphy CLK_TIM / HS_TIM values are
	 * all derived from this, so they have to agree with the actual rate.
	 */
	priv->dphy_cfg.hs_clk_rate = 666666666UL;

	priv->lane_mbps = DIV_ROUND_UP(priv->dphy_cfg.hs_clk_rate, 1000000UL);
	priv->timings = *timings;
	*lane_mbps = priv->lane_mbps;

	dev_dbg(dev, "pclk %u Hz, %u lanes, %d bpp -> hs_clk %lu Hz (%u Mbps/lane)\n",
		(unsigned int)timings->pixelclock.typ, lanes, bpp,
		priv->dphy_cfg.hs_clk_rate, priv->lane_mbps);

	return 0;
}

static int meson_dw_mipi_dsi_phy_init(void *priv_data)
{
	struct mipi_dsi_device *device = priv_data;
	struct udevice *dev = device->dev;
	struct meson_dw_mipi_dsi_priv *priv = dev_get_priv(dev);
	u32 dpi_data_format, venc_data_width;
	int ret;

	switch (device->format) {
	case MIPI_DSI_FMT_RGB888:
		dpi_data_format = DPI_COLOR_24BIT;
		venc_data_width = VENC_IN_COLOR_24B;
		break;
	case MIPI_DSI_FMT_RGB666:
		dpi_data_format = DPI_COLOR_18BIT_CFG_2;
		venc_data_width = VENC_IN_COLOR_18B;
		break;
	default:
		dev_err(dev, "unsupported DSI pixel format 0x%x\n",
			device->format);
		return -EINVAL;
	}

	/* HHI clock setup happens here once D4 lands; D3 logs the targets. */
	meson_dw_mipi_dsi_setup_clocks(dev, priv->dphy_cfg.hs_clk_rate,
				       priv->timings.pixelclock.typ);

	/*
	 * Tell the TOP wrapper how to translate VENC pixels into a DPI stream.
	 * COMP*_SEL=0/1/2 is the default identity mapping (R->comp0, G->comp1,
	 * B->comp2), matching Linux mainline.
	 */
	/*
	 * HSYNC_INVERT|VSYNC_INVERT match what vendor's shipping firmware
	 * writes (TOP_CNTL=0x00512430 observed live). Linux mainline's
	 * meson_dw_mipi_dsi.c doesn't set these bits — but the ST7701S on the
	 * carthing needs them to interpret the VENC-side sync polarity
	 * correctly. Without it the panel sees malformed frame timing and
	 * shows black.
	 */
	writel(FIELD_PREP(MIPI_DSI_TOP_DPI_COLOR_MODE, dpi_data_format) |
	       FIELD_PREP(MIPI_DSI_TOP_IN_COLOR_MODE, venc_data_width) |
	       FIELD_PREP(MIPI_DSI_TOP_COMP2_SEL, 2) |
	       FIELD_PREP(MIPI_DSI_TOP_COMP1_SEL, 1) |
	       FIELD_PREP(MIPI_DSI_TOP_COMP0_SEL, 0) |
	       MIPI_DSI_TOP_HSYNC_INVERT |
	       MIPI_DSI_TOP_VSYNC_INVERT,
	       priv->base + MIPI_DSI_TOP_CNTL);

	/* Push the computed dphy config (hs_clk_rate + timings) at the phy. */
	ret = generic_phy_configure(&priv->dphy, &priv->dphy_cfg);
	if (ret) {
		dev_err(dev, "dphy configure failed: %d\n", ret);
		return ret;
	}

	ret = generic_phy_power_on(&priv->dphy);
	if (ret) {
		dev_err(dev, "dphy power-on failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int meson_dw_mipi_dsi_phy_get_timing(void *priv_data,
					    unsigned int lane_mbps,
					    struct mipi_dsi_phy_timing *timing)
{
	/*
	 * Match Linux mainline's default-branch values. The Linux driver has
	 * a narrower "fast" set for hdisplay in {240, 768, 1920, 2560}; the
	 * carthing's 480-wide panel falls into the default bucket either way.
	 */
	timing->clk_lp2hs = 37;
	timing->clk_hs2lp = 135;
	timing->data_lp2hs = 50;
	timing->data_hs2lp = 3;
	return 0;
}

static void meson_dw_mipi_dsi_phy_get_esc_clk_rate(void *priv_data,
						   unsigned int *esc_clk_rate)
{
	*esc_clk_rate = 4; /* MHz; same as Linux */
}

static const struct mipi_dsi_phy_ops meson_dw_mipi_dsi_phy_ops = {
	.init			= meson_dw_mipi_dsi_phy_init,
	.get_lane_mbps		= meson_dw_mipi_dsi_phy_get_lane_mbps,
	.get_timing		= meson_dw_mipi_dsi_phy_get_timing,
	.get_esc_clk_rate	= meson_dw_mipi_dsi_phy_get_esc_clk_rate,
};

/* --- VIDEO_BRIDGE ops --- */

static int meson_dw_mipi_dsi_attach(struct udevice *dev)
{
	struct meson_dw_mipi_dsi_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *mplat;
	int ret;

	ret = uclass_first_device_err(UCLASS_PANEL, &priv->panel);
	if (ret) {
		dev_err(dev, "no MIPI DSI panel found: %d\n", ret);
		return ret;
	}

	/* Hand the panel a back-pointer to our DSI device for DCS writes. */
	mplat = dev_get_plat(priv->panel);
	mplat->device = &priv->device;
	priv->device.lanes = mplat->lanes;
	priv->device.format = mplat->format;
	priv->device.mode_flags = mplat->mode_flags;

	ret = panel_get_display_timing(priv->panel, &priv->timings);
	if (ret) {
		ret = ofnode_decode_display_timing(dev_ofnode(priv->panel),
						   0, &priv->timings);
		if (ret) {
			dev_err(dev, "failed to read panel display timing: %d\n",
				ret);
			return ret;
		}
	}

	ret = uclass_get_device(UCLASS_DSI_HOST, 0, &priv->dsi_host);
	if (ret) {
		dev_err(dev, "no DSI host found: %d\n", ret);
		return ret;
	}

	ret = dsi_host_init(priv->dsi_host, &priv->device, &priv->timings,
			    priv->device.lanes, &meson_dw_mipi_dsi_phy_ops);
	if (ret) {
		dev_err(dev, "dsi_host_init failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int meson_dw_mipi_dsi_set_backlight(struct udevice *dev, int percent)
{
	struct meson_dw_mipi_dsi_priv *priv = dev_get_priv(dev);
	int ret;

	/*
	 * panel_enable_backlight() drives the panel's reset/power GPIOs,
	 * sends the DSI init command sequence (DCS writes flow through us
	 * via dw_mipi_dsi_host_transfer), and enables the backlight.
	 */
	ret = panel_enable_backlight(priv->panel);
	if (ret) {
		dev_err(dev, "panel enable failed: %d\n", ret);
		return ret;
	}

	/* Switch the DSI host from command mode into video mode. */
	ret = dsi_host_enable(priv->dsi_host);
	if (ret) {
		dev_err(dev, "dsi_host_enable failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct video_bridge_ops meson_dw_mipi_dsi_ops = {
	.attach		= meson_dw_mipi_dsi_attach,
	.set_backlight	= meson_dw_mipi_dsi_set_backlight,
};

/* --- DM plumbing --- */

static int meson_dw_mipi_dsi_bind(struct udevice *dev)
{
	int ret;

	/*
	 * Pattern stolen from drivers/video/stm32/stm32_dsi.c: bind the
	 * generic dw_mipi_dsi child to the SAME ofnode as our parent so the
	 * DWC IP and the TOP wrapper share the same `reg` window. Then let
	 * DM walk the panel subnode so the panel driver also binds.
	 */
	ret = device_bind_driver_to_node(dev, "dw_mipi_dsi", "dsihost",
					 dev_ofnode(dev), NULL);
	if (ret)
		return ret;

	return dm_scan_fdt_dev(dev);
}

static int meson_dw_mipi_dsi_of_to_plat(struct udevice *dev)
{
	struct meson_dw_mipi_dsi_priv *priv = dev_get_priv(dev);
	int ret;

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base) {
		dev_err(dev, "missing reg in DT\n");
		return -EINVAL;
	}

	ret = reset_get_by_name(dev, "top", &priv->top_rst);
	if (ret) {
		dev_err(dev, "failed to get 'top' reset: %d\n", ret);
		return ret;
	}

	ret = generic_phy_get_by_name(dev, "dphy", &priv->dphy);
	if (ret) {
		dev_err(dev, "failed to get dphy: %d\n", ret);
		return ret;
	}

	priv->device.dev = dev;

	return 0;
}

static int meson_dw_mipi_dsi_probe(struct udevice *dev)
{
	struct meson_dw_mipi_dsi_priv *priv = dev_get_priv(dev);
	int ret;

	ret = reset_assert(&priv->top_rst);
	if (ret)
		return ret;
	udelay(20);
	ret = reset_deassert(&priv->top_rst);
	if (ret)
		return ret;

	/*
	 * Init the dphy: this asserts/deasserts the dphy reset internally
	 * and brings the analog phy out of shutdown. It does NOT yet apply
	 * any hs_clk_rate (that happens later in phy_init/configure).
	 */
	ret = generic_phy_init(&priv->dphy);
	if (ret) {
		dev_err(dev, "dphy init failed: %d\n", ret);
		return ret;
	}

	meson_dw_mipi_dsi_hw_init(priv);

	return 0;
}

static const struct udevice_id meson_dw_mipi_dsi_ids[] = {
	{ .compatible = "amlogic,meson-g12a-dw-mipi-dsi" },
	{ }
};

U_BOOT_DRIVER(meson_dw_mipi_dsi) = {
	.name		= "meson_dw_mipi_dsi",
	.id		= UCLASS_VIDEO_BRIDGE,
	.of_match	= meson_dw_mipi_dsi_ids,
	.bind		= meson_dw_mipi_dsi_bind,
	.of_to_plat	= meson_dw_mipi_dsi_of_to_plat,
	.probe		= meson_dw_mipi_dsi_probe,
	.ops		= &meson_dw_mipi_dsi_ops,
	.priv_auto	= sizeof(struct meson_dw_mipi_dsi_priv),
};
