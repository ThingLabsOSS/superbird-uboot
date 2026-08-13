// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2016 Carlo Caione <carlo@caione.org>
 */

#include <clk.h>
#include <cpu_func.h>
#include <dm.h>
#include <fdtdec.h>
#include <malloc.h>
#include <pwrseq.h>
#include <mmc.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <linux/delay.h>
#include <linux/log2.h>
#include "meson_gx_mmc.h"

#if CONFIG_IS_ENABLED(DM_MMC)
bool meson_gx_mmc_is_compatible(struct udevice *dev,
				enum meson_gx_mmc_compatible family)
{
	enum meson_gx_mmc_compatible compat = dev_get_driver_data(dev);

	return compat == family;
}
#endif

static inline void *get_regbase(const struct mmc *mmc)
{
	struct meson_mmc_plat *pdata = mmc->priv;

	return pdata->regbase;
}

static inline uint32_t meson_read(struct mmc *mmc, int offset)
{
	return readl(get_regbase(mmc) + offset);
}

static inline void meson_write(struct mmc *mmc, uint32_t val, int offset)
{
	writel(val, get_regbase(mmc) + offset);
}

/*
 * Host-side clock phase / delay-line settings, applied on every
 * set_ios. Defaults reproduce the long-standing hardcoded behaviour;
 * meson_mmc_set_phase() lets a board sweep them to find the window a
 * given eMMC part actually needs. See meson_gx_mmc.h for the register
 * layout and why RX was never programmed.
 */
struct meson_mmc_phase meson_phase = {
	.core = MESON_PHASE_DEFAULT,
	.tx = MESON_PHASE_DEFAULT,
	.rx = MESON_PHASE_DEFAULT,
	.tx_delay = 0,
	.rx_delay = 0,
};

void meson_mmc_set_phase(const struct meson_mmc_phase *p)
{
	meson_phase = *p;
}

void meson_mmc_get_phase(struct meson_mmc_phase *p)
{
	*p = meson_phase;
}

static void meson_mmc_config_clock(struct mmc *mmc)
{
	uint32_t meson_mmc_clk = 0;
	unsigned int clk, clk_src, clk_div;

	if (!mmc->clock)
		return;

	/* TOFIX This should use the proper clock taken from DT */

	/* 1GHz / CLK_MAX_DIV = 15,9 MHz */
	if (mmc->clock > 16000000) {
		clk = SD_EMMC_CLKSRC_DIV2;
		clk_src = CLK_SRC_DIV2;
	} else {
		clk = SD_EMMC_CLKSRC_24M;
		clk_src = CLK_SRC_24M;
	}
	clk_div = DIV_ROUND_UP(clk, mmc->clock);

	/*
	 * SM1 SoCs doesn't work fine over 50MHz with CLK_CO_PHASE_180
	 * If CLK_CO_PHASE_270 is used, it's more stable than other.
	 * Other SoCs use CLK_CO_PHASE_180 by default.
	 * It needs to find what is a proper value about each SoCs.
	 */
	if (meson_phase.core == MESON_PHASE_DEFAULT) {
#if CONFIG_IS_ENABLED(DM_MMC)
		if (meson_gx_mmc_is_compatible(mmc->dev, MMC_COMPATIBLE_SM1))
			meson_mmc_clk |= CLK_CO_PHASE_270;
		else
			meson_mmc_clk |= CLK_CO_PHASE_180;
#else /* U-Boot SPL on GX SoCs */
		meson_mmc_clk |= CLK_CO_PHASE_180;
#endif
	} else {
		meson_mmc_clk |= meson_phase.core << CLK_CO_PHASE_SHIFT;
	}

	/* TX phase: 0 degrees historically (the comment below used to say
	 * 180, which the code never did). */
	meson_mmc_clk |= (meson_phase.tx == MESON_PHASE_DEFAULT ? 0 :
			  meson_phase.tx) << CLK_TX_PHASE_SHIFT;

	/*
	 * RX sampling phase and delay lines. Zero unless someone has tuned
	 * them — which reproduces the historic behaviour exactly, but is
	 * only correct by luck: it is one fixed sampling point for every
	 * eMMC part. Field units with a different flash part than the one
	 * this was implicitly tuned against fail to read at DDR52 while
	 * reading fine slowly, and fine under Linux, which tunes.
	 */
	if (meson_phase.rx != MESON_PHASE_DEFAULT)
		meson_mmc_clk |= meson_phase.rx << CLK_RX_PHASE_SHIFT;
	if (meson_phase.rx_delay)
		meson_mmc_clk |= (meson_phase.rx_delay <<
				  CLK_V3_RX_DELAY_SHIFT) &
				 CLK_V3_RX_DELAY_MASK;
	if (meson_phase.tx_delay)
		meson_mmc_clk |= (meson_phase.tx_delay <<
				  CLK_V3_TX_DELAY_SHIFT) &
				 CLK_V3_TX_DELAY_MASK;

	/* clock settings */
	meson_mmc_clk |= clk_src;
	meson_mmc_clk |= clk_div;

	meson_write(mmc, meson_mmc_clk, MESON_SD_EMMC_CLOCK);
}

#if CONFIG_IS_ENABLED(DM_MMC)
static int meson_dm_mmc_set_ios(struct udevice *dev)
{
	struct mmc *mmc = mmc_get_mmc_dev(dev);
#else /* U-Boot SPL */
static int meson_legacy_mmc_set_ios(struct mmc *mmc)
{
#endif
	uint32_t meson_mmc_cfg;

	meson_mmc_config_clock(mmc);

	meson_mmc_cfg = meson_read(mmc, MESON_SD_EMMC_CFG);

	meson_mmc_cfg &= ~CFG_BUS_WIDTH_MASK;
	if (mmc->bus_width == 1)
		meson_mmc_cfg |= CFG_BUS_WIDTH_1;
	else if (mmc->bus_width == 4)
		meson_mmc_cfg |= CFG_BUS_WIDTH_4;
	else if (mmc->bus_width == 8)
		meson_mmc_cfg |= CFG_BUS_WIDTH_8;
	else
		return -EINVAL;

	/* DDR mode bit — set when the mmc core has negotiated the eMMC
	 * into DDR52 (or any other DDR mode) via CMD6. mmc->ddr_mode is
	 * the framework's signal that bytes go out on both clock edges. */
	if (mmc->ddr_mode)
		meson_mmc_cfg |= CFG_DDR_EN;
	else
		meson_mmc_cfg &= ~CFG_DDR_EN;

	/* 512 bytes block length */
	meson_mmc_cfg &= ~CFG_BL_LEN_MASK;
	meson_mmc_cfg |= CFG_BL_LEN_512;

	/* Response timeout 256 clk */
	meson_mmc_cfg &= ~CFG_RESP_TIMEOUT_MASK;
	meson_mmc_cfg |= CFG_RESP_TIMEOUT_256;

	/* Command-command gap 16 clk */
	meson_mmc_cfg &= ~CFG_RC_CC_MASK;
	meson_mmc_cfg |= CFG_RC_CC_16;

	meson_write(mmc, meson_mmc_cfg, MESON_SD_EMMC_CFG);

	return 0;
}

static void meson_mmc_setup_cmd(struct mmc *mmc, struct mmc_data *data,
				struct mmc_cmd *cmd)
{
	uint32_t meson_mmc_cmd = 0, cfg;

	meson_mmc_cmd |= cmd->cmdidx << CMD_CFG_CMD_INDEX_SHIFT;

	if (cmd->resp_type & MMC_RSP_PRESENT) {
		if (cmd->resp_type & MMC_RSP_136)
			meson_mmc_cmd |= CMD_CFG_RESP_128;

		if (cmd->resp_type & MMC_RSP_BUSY)
			meson_mmc_cmd |= CMD_CFG_R1B;

		if (!(cmd->resp_type & MMC_RSP_CRC))
			meson_mmc_cmd |= CMD_CFG_RESP_NOCRC;
	} else {
		meson_mmc_cmd |= CMD_CFG_NO_RESP;
	}

	if (data) {
		cfg = meson_read(mmc, MESON_SD_EMMC_CFG);
		cfg &= ~CFG_BL_LEN_MASK;
		cfg |= ilog2(data->blocksize) << CFG_BL_LEN_SHIFT;
		meson_write(mmc, cfg, MESON_SD_EMMC_CFG);

		if (data->flags == MMC_DATA_WRITE)
			meson_mmc_cmd |= CMD_CFG_DATA_WR;

		meson_mmc_cmd |= CMD_CFG_DATA_IO | CMD_CFG_BLOCK_MODE |
				 data->blocks;
	}

	meson_mmc_cmd |= CMD_CFG_TIMEOUT_4S | CMD_CFG_OWNER |
			 CMD_CFG_END_OF_CHAIN;

	meson_write(mmc, meson_mmc_cmd, MESON_SD_EMMC_CMD_CFG);
}

static void meson_mmc_setup_addr(struct mmc *mmc, struct mmc_data *data)
{
	struct meson_mmc_plat *pdata = mmc->priv;
	unsigned int data_size;
	uint32_t data_addr = 0;

	if (data) {
		data_size = data->blocks * data->blocksize;

		if (data->flags == MMC_DATA_READ) {
			data_addr = (ulong) data->dest;
			invalidate_dcache_range(data_addr,
						data_addr + data_size);
		} else {
			/* Upstream allocated + memcpy'd into a fresh buffer
			 * on every write and never freed it — making each
			 * write a 256 KiB malloc + copy + leak. The src
			 * buffer is already cacheline-aligned for every
			 * caller in u-boot proper (memalign in fsg, in the
			 * block-cache layer, etc), so DMA directly from it
			 * after a dcache flush. */
			data_addr = (ulong) data->src;
			flush_dcache_range(data_addr, data_addr + data_size);
		}
	}

	meson_write(mmc, data_addr, MESON_SD_EMMC_CMD_DAT);
}

static void meson_mmc_read_response(struct mmc *mmc, struct mmc_cmd *cmd)
{
	if (cmd->resp_type & MMC_RSP_136) {
		cmd->response[0] = meson_read(mmc, MESON_SD_EMMC_CMD_RSP3);
		cmd->response[1] = meson_read(mmc, MESON_SD_EMMC_CMD_RSP2);
		cmd->response[2] = meson_read(mmc, MESON_SD_EMMC_CMD_RSP1);
		cmd->response[3] = meson_read(mmc, MESON_SD_EMMC_CMD_RSP);
	} else {
		cmd->response[0] = meson_read(mmc, MESON_SD_EMMC_CMD_RSP);
	}
}

#if CONFIG_IS_ENABLED(DM_MMC)
static int meson_dm_mmc_send_cmd(struct udevice *dev, struct mmc_cmd *cmd,
				 struct mmc_data *data)
{
	struct mmc *mmc = mmc_get_mmc_dev(dev);
#else /* U-Boot SPL */
static int meson_legacy_mmc_send_cmd(struct mmc *mmc, struct mmc_cmd *cmd,
				     struct mmc_data *data)
{
#endif
	struct meson_mmc_plat *pdata = mmc->priv;
	uint32_t status;
	ulong start;
	int ret = 0;

	/* max block size supported by chip is 512 byte */
	if (data && data->blocksize > 512)
		return -EINVAL;

	meson_mmc_setup_cmd(mmc, data, cmd);
	meson_mmc_setup_addr(mmc, data);

	meson_write(mmc, cmd->cmdarg, MESON_SD_EMMC_CMD_ARG);

	/* use 10s timeout */
	start = get_timer(0);
	do {
		status = meson_read(mmc, MESON_SD_EMMC_STATUS);
	} while(!(status & STATUS_END_OF_CHAIN) && get_timer(start) < 10000);

	if (!(status & STATUS_END_OF_CHAIN))
		ret = -ETIMEDOUT;
	else if (status & STATUS_RESP_TIMEOUT)
		ret = -ETIMEDOUT;
	else if (status & STATUS_ERR_MASK)
		ret = -EIO;

	meson_mmc_read_response(mmc, cmd);

	/* reset status bits */
	meson_write(mmc, STATUS_MASK, MESON_SD_EMMC_STATUS);

	return ret;
}

#if !CONFIG_IS_ENABLED(DM_MMC) /* Non-DM MMC driver for use in U-Boot SPL */
struct meson_mmc_plat mmc_plat[2];

static int meson_legacy_mmc_init(struct mmc *mmc)
{
	/* reset all status bits */
	meson_write(mmc, STATUS_MASK, MESON_SD_EMMC_STATUS);

	/* disable interrupts */
	meson_write(mmc, 0, MESON_SD_EMMC_IRQ_EN);

	return 0;
}

static const struct mmc_ops meson_mmc_ops = {
	.send_cmd	= meson_legacy_mmc_send_cmd,
	.set_ios	= meson_legacy_mmc_set_ios,
	.init		= meson_legacy_mmc_init,
};

struct mmc *meson_mmc_init(int mmc_no)
{
	struct meson_mmc_plat *pdata = &mmc_plat[mmc_no];
	struct mmc_config *cfg = &pdata->cfg;

	cfg->voltages = MMC_VDD_33_34 | MMC_VDD_32_33 |
			MMC_VDD_31_32 | MMC_VDD_165_195;
	cfg->host_caps = MMC_MODE_8BIT | MMC_MODE_4BIT;
	cfg->f_min = DIV_ROUND_UP(SD_EMMC_CLKSRC_24M, CLK_MAX_DIV);
	cfg->f_max = 6000000; /* 6 MHz */
	cfg->b_max = 127; /* max 128 - 1 block */
	cfg->name = "Meson SD/eMMC";
	cfg->ops = &meson_mmc_ops;

	if (mmc_no == 0) /* MMC1: SD card */
		pdata->regbase = (void *)0xd0072000;
	else if (mmc_no == 1) /* MMC2: eMMC */
		pdata->regbase = (void *)0xd0074000;

#if CONFIG_IS_ENABLED(MMC_PWRSEQ)
	/* Enable power if needed */
	ret = mmc_pwrseq_get_power(dev, cfg);
	if (!ret) {
		ret = pwrseq_set_power(cfg->pwr_dev, true);
		if (ret)
			return ret;
	}
#endif

	return mmc_create(cfg, pdata);
}

#else /* DM-based driver for use in U-Boot proper */
static const struct dm_mmc_ops meson_dm_mmc_ops = {
	.send_cmd = meson_dm_mmc_send_cmd,
	.set_ios = meson_dm_mmc_set_ios,
};

static int meson_mmc_of_to_plat(struct udevice *dev)
{
	struct meson_mmc_plat *pdata = dev_get_plat(dev);
	fdt_addr_t addr;

	addr = dev_read_addr(dev);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;

	pdata->regbase = (void *)addr;

	return 0;
}

static int meson_mmc_probe(struct udevice *dev)
{
	struct meson_mmc_plat *pdata = dev_get_plat(dev);
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);
	struct mmc *mmc = &pdata->mmc;
	struct mmc_config *cfg = &pdata->cfg;
	struct clk_bulk clocks;
	uint32_t val;
	int ret;

	/* Enable the clocks feeding the MMC controller */
	ret = clk_get_bulk(dev, &clocks);
	if (ret)
		return ret;

	ret = clk_enable_bulk(&clocks);
	if (ret)
		return ret;

	cfg->voltages = MMC_VDD_33_34 | MMC_VDD_32_33 |
			MMC_VDD_31_32 | MMC_VDD_165_195;
	cfg->host_caps = MMC_MODE_8BIT | MMC_MODE_4BIT |
			MMC_MODE_HS_52MHz | MMC_MODE_HS;
	cfg->f_min = DIV_ROUND_UP(SD_EMMC_CLKSRC_24M, CLK_MAX_DIV);
	cfg->f_max = 40000000; /* 40 MHz */
	cfg->b_max = 511; /* max 512 - 1 blocks */
	cfg->name = dev->name;

	/* Without this, the DT's `bus-width = <8>` and `cap-mmc-highspeed`
	 * properties never reach mmc-uclass and the framework stays at
	 * 1-bit even though the controller and eMMC support 8-bit.
	 *
	 * Strip HS200 / HS400 / HS400_ES since this driver doesn't yet
	 * implement input-sampling tuning (HS200) or data-strobe support
	 * (HS400). DDR52 is supported via the CFG_DDR_EN bit set in
	 * set_ios. f_max is capped at 52 MHz accordingly. */
	mmc_of_parse(dev, cfg);
	cfg->host_caps &= ~(MMC_CAP(MMC_HS_400) | MMC_CAP(MMC_HS_400_ES) |
			    MMC_CAP(MMC_HS_200));
	cfg->f_max = min_t(uint, cfg->f_max, 52000000U);

	if (IS_ENABLED(CONFIG_SPL_BUILD)) {
		cfg->host_caps &= ~(MMC_MODE_HS_52MHz | MMC_MODE_HS);
		cfg->f_max = 6000000; /* 6 MHz */
		cfg->b_max = 127; /* max 128 - 1 block */
	}

	mmc->priv = pdata;
	upriv->mmc = mmc;

	mmc_set_clock(mmc, cfg->f_min, MMC_CLK_ENABLE);

#if CONFIG_IS_ENABLED(MMC_PWRSEQ)
	/* Enable power if needed */
	ret = mmc_pwrseq_get_power(dev, cfg);
	if (!ret) {
		ret = pwrseq_set_power(cfg->pwr_dev, true);
		if (ret)
			return ret;
	}
#endif

	/* reset all status bits */
	meson_write(mmc, STATUS_MASK, MESON_SD_EMMC_STATUS);

	/* disable interrupts */
	meson_write(mmc, 0, MESON_SD_EMMC_IRQ_EN);

	/* enable auto clock mode */
	val = meson_read(mmc, MESON_SD_EMMC_CFG);
	val &= ~CFG_SDCLK_ALWAYS_ON;
	val |= CFG_AUTO_CLK;
	meson_write(mmc, val, MESON_SD_EMMC_CFG);

	return 0;
}

int meson_mmc_bind(struct udevice *dev)
{
	struct meson_mmc_plat *pdata = dev_get_plat(dev);

	return mmc_bind(dev, &pdata->mmc, &pdata->cfg);
}

static const struct udevice_id meson_mmc_match[] = {
	{ .compatible = "amlogic,meson-gx-mmc", .data = MMC_COMPATIBLE_GX },
	{ .compatible = "amlogic,meson-axg-mmc", .data = MMC_COMPATIBLE_GX },
	{ .compatible = "amlogic,meson-sm1-mmc", .data = MMC_COMPATIBLE_SM1 },
	{ /* sentinel */ }
};

U_BOOT_DRIVER(meson_mmc) = {
	.name = "meson_gx_mmc",
	.id = UCLASS_MMC,
	.of_match = meson_mmc_match,
	.ops = &meson_dm_mmc_ops,
	.probe = meson_mmc_probe,
	.bind = meson_mmc_bind,
	.of_to_plat = meson_mmc_of_to_plat,
	.plat_auto	= sizeof(struct meson_mmc_plat),
};
#endif /* CONFIG_IS_ENABLED(DM_MMC) */
