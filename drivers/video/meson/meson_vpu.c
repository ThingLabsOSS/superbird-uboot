// SPDX-License-Identifier: GPL-2.0
/*
 * Amlogic Meson Video Processing Unit driver
 *
 * Copyright (c) 2018 BayLibre, SAS.
 * Author: Neil Armstrong <narmstrong@baylibre.com>
 */

#include <backlight.h>
#include <display.h>
#include <dm.h>
#include <efi_loader.h>
#include <fdt_support.h>
#include <log.h>
#include <panel.h>
#include <part.h>
#include <video_bridge.h>
#include <linux/sizes.h>
#include <asm/arch/mem.h>
#include <asm/global_data.h>
#include <dm/device-internal.h>
#include <dm/uclass-internal.h>

#include "meson_vpu.h"
#include "meson_registers.h"
#include "simplefb_common.h"

#define MESON_VPU_OVERSCAN SZ_64K

/* Static variable for use in meson_vpu_rsv_fb() */
static struct meson_framebuffer {
	u64 base;
	u64 fb_size;
	unsigned int xsize;
	unsigned int ysize;
	enum meson_vpu_output out;
} meson_fb = { 0 };

bool meson_vpu_is_compatible(struct meson_vpu_priv *priv,
			     enum vpu_compatible family)
{
	enum vpu_compatible compat = dev_get_driver_data(priv->dev);

	return compat == family;
}

static int meson_vpu_setup_mode(struct udevice *dev, struct udevice *disp,
				struct udevice *bridge)
{
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct display_timing timing;
	struct udevice *panel = NULL;
	enum meson_vpu_output out = MESON_VPU_OUT_HDMI;
	int ret = 0;

	if (bridge) {
		/*
		 * The DSI bridge bound its panel as a DT child during bind.
		 * Probe it directly so we can pull its display_timing — we
		 * need that before setup_venc/setup_vclk, and the bridge's
		 * .attach (which we run later) will use the same timing.
		 */
		ret = uclass_first_device_err(UCLASS_PANEL, &panel);
		if (ret) {
			debug("%s: bridge present but no panel found: %d\n",
			      __func__, ret);
			goto cvbs;
		}

		ret = panel_get_display_timing(panel, &timing);
		if (ret) {
			debug("%s: panel_get_display_timing failed: %d\n",
			      __func__, ret);
			goto cvbs;
		}

		out = MESON_VPU_OUT_DSI;
		uc_priv->xsize = timing.hactive.typ;
		uc_priv->ysize = timing.vactive.typ;
		/* Car Thing panel is mounted portrait; we want the console
		 * to render landscape for the bootloader UI. */
		uc_priv->rot = 1; /* 90 degrees clockwise */
	} else if (disp) {
		ret = display_read_timing(disp, &timing);
		if (ret) {
			debug("%s: Failed to read timings\n", __func__);
			goto cvbs;
		}

		uc_priv->xsize = timing.hactive.typ;
		uc_priv->ysize = timing.vactive.typ;

		ret = display_enable(disp, 0, &timing);
		if (ret)
			goto cvbs;
	} else {
cvbs:
		/* CVBS has a fixed 720x480i (NTSC) and 720x576i (PAL) */
		out = MESON_VPU_OUT_CVBS;
		timing.flags = DISPLAY_FLAGS_INTERLACED;
		uc_priv->xsize = 720;
		uc_priv->ysize = 576;
	}

	uc_priv->bpix = VPU_MAX_LOG2_BPP;

	meson_fb.out = out;
	meson_fb.xsize = uc_priv->xsize;
	meson_fb.ysize = uc_priv->ysize;

	/* Move the framebuffer to the end of addressable ram */
	meson_fb.fb_size = ALIGN(meson_fb.xsize * meson_fb.ysize *
				 ((1 << VPU_MAX_LOG2_BPP) / 8) +
				 MESON_VPU_OVERSCAN, EFI_PAGE_SIZE);
	meson_fb.base = gd->bd->bi_dram[0].start +
			gd->bd->bi_dram[0].size - meson_fb.fb_size;

	/* Override the framebuffer address */
	uc_plat->base = meson_fb.base;

	meson_vpu_setup_plane(dev, timing.flags & DISPLAY_FLAGS_INTERLACED);
	meson_vpu_setup_venc(dev, &timing, out);
	meson_vpu_setup_vclk(dev, &timing, out);

	if (out == MESON_VPU_OUT_DSI) {
		/*
		 * VENC/VCLK are now feeding the DPI signal toward the DSI
		 * host. Bring the host up (configures DWC + dphy) and then
		 * fire the panel init sequence + switch into video mode.
		 *
		 * Failures here are non-fatal: the framebuffer is already
		 * configured and HDMI-style fallbacks are out of scope. We
		 * just log and continue so we still drop to a CLI prompt.
		 */
		ret = video_bridge_attach(bridge);
		if (ret)
			log_warning("DSI bridge attach failed: %d\n", ret);
		else {
			ret = video_bridge_set_backlight(bridge,
							 BACKLIGHT_DEFAULT);
			if (ret)
				log_warning("DSI panel enable failed: %d\n", ret);
			/*
			 * Swap ENCL out of the (still-gated) test-pattern path
			 * configured by meson_venc_mipi_dsi_mode_set() and
			 * enable it for the first time, so the VPU framebuffer
			 * clocks out to the panel. ENCL has stayed gated through
			 * PHY + panel DCS + video-mode DSI_PWR_UP — enabling it
			 * here lets the DW host catch ENCL's first VSYNC as its
			 * frame-0 (see meson_venc_mipi_dsi_mode_set() comment
			 * on why we deferred ENCL_VIDEO_EN=1 to this point).
			 */
			meson_venc_mipi_dsi_video_enable(dev);
		}
	}

	video_set_flush_dcache(dev, 1);

	return 0;
}

static const struct udevice_id meson_vpu_ids[] = {
	{ .compatible = "amlogic,meson-gxbb-vpu", .data = VPU_COMPATIBLE_GXBB },
	{ .compatible = "amlogic,meson-gxl-vpu", .data = VPU_COMPATIBLE_GXL },
	{ .compatible = "amlogic,meson-gxm-vpu", .data = VPU_COMPATIBLE_GXM },
	{ .compatible = "amlogic,meson-g12a-vpu", .data = VPU_COMPATIBLE_G12A },
	{ }
};

static int meson_vpu_probe(struct udevice *dev)
{
	struct meson_vpu_priv *priv = dev_get_priv(dev);
	struct udevice *disp, *bridge;
	int ret;

	/* Before relocation we don't need to do anything */
	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

	priv->dev = dev;

	priv->io_base = dev_remap_addr_index(dev, 0);
	if (!priv->io_base)
		return -EINVAL;

	priv->hhi_base = dev_remap_addr_index(dev, 1);
	if (!priv->hhi_base)
		return -EINVAL;

	priv->dmc_base = dev_remap_addr_index(dev, 2);
	if (!priv->dmc_base)
		return -EINVAL;

	meson_vpu_init(dev);

	/*
	 * Look for a DSI bridge first (panel output path), then fall back to
	 * a UCLASS_DISPLAY (HDMI). If neither is present, setup_mode lands on
	 * the CVBS fallback. Both lookups probe their target as a side effect.
	 */
	if (IS_ENABLED(CONFIG_VIDEO_BRIDGE) &&
	    !uclass_get_device(UCLASS_VIDEO_BRIDGE, 0, &bridge))
		return meson_vpu_setup_mode(dev, NULL, bridge);

	ret = uclass_get_device(UCLASS_DISPLAY, 0, &disp);

	return meson_vpu_setup_mode(dev, ret ? NULL : disp, NULL);
}

static int meson_vpu_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->size = VPU_MAX_WIDTH * VPU_MAX_HEIGHT *
		(1 << VPU_MAX_LOG2_BPP) / 8;

	return 0;
}

#if defined(CONFIG_VIDEO_DT_SIMPLEFB)
static void meson_vpu_setup_simplefb(void *fdt)
{
	const char *pipeline = NULL;
	u64 mem_start, mem_size;
	int offset, ret;

	switch (meson_fb.out) {
	case MESON_VPU_OUT_CVBS:
		pipeline = "vpu-cvbs";
		break;
	case MESON_VPU_OUT_DSI:
		pipeline = "vpu-dsi";
		break;
	case MESON_VPU_OUT_HDMI:
	default:
		pipeline = "vpu-hdmi";
		break;
	}

	offset = meson_simplefb_fdt_match(fdt, pipeline);
	if (offset < 0) {
		eprintf("Cannot setup simplefb: node not found\n");

		/* If simplefb is missing, add it as reserved memory */
		meson_board_add_reserved_memory(fdt, meson_fb.base,
						meson_fb.fb_size);

		return;
	}

	/*
	 * SimpleFB will try to iomap the framebuffer, so we can't use
	 * fdt_add_mem_rsv on the memory area. Instead, the FB is stored
	 * at the end of the RAM and we strip this portion from the kernel
	 * allowed region
	 */
	mem_start = gd->bd->bi_dram[0].start;
	mem_size = gd->bd->bi_dram[0].size - meson_fb.fb_size;
	ret = fdt_fixup_memory_banks(fdt, &mem_start, &mem_size, 1);
	if (ret) {
		eprintf("Cannot setup simplefb: Error reserving memory\n");
		return;
	}

	ret = fdt_setup_simplefb_node(fdt, offset, meson_fb.base,
				      meson_fb.xsize, meson_fb.ysize,
				      meson_fb.xsize * 4, "x8r8g8b8");
	if (ret)
		eprintf("Cannot setup simplefb: Error setting properties\n");
}
#endif

void meson_vpu_rsv_fb(void *fdt)
{
	if (!meson_fb.base || !meson_fb.xsize || !meson_fb.ysize)
		return;

#if defined(CONFIG_EFI_LOADER)
	efi_add_memory_map(meson_fb.base, meson_fb.fb_size,
			   EFI_RESERVED_MEMORY_TYPE);
#endif
#if defined(CONFIG_VIDEO_DT_SIMPLEFB)
	meson_vpu_setup_simplefb(fdt);
#endif
}

U_BOOT_DRIVER(meson_vpu) = {
	.name = "meson_vpu",
	.id = UCLASS_VIDEO,
	.of_match = meson_vpu_ids,
	.probe = meson_vpu_probe,
	.bind = meson_vpu_bind,
	.priv_auto	= sizeof(struct meson_vpu_priv),
	.flags  = DM_FLAG_PRE_RELOC | DM_FLAG_LEAVE_PD_ON,
};
