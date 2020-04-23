/*
 * (C) Copyright 2017 Whitebox Systems / Northend Systems B.V.
 * S.J.R. van Schaik <stephan@whiteboxsystems.nl>
 * M.B.W. Wajer <merlijn@whiteboxsystems.nl>
 *
 * (C) Copyright 2017 Olimex Ltd..
 * Stefan Mavrodiev <stefan@olimex.com>
 *
 * Based on linux spi driver. Original copyright follows:
 * linux/drivers/spi/spi-sun4i.c
 *
 * Copyright (C) 2012 - 2014 Allwinner Tech
 * Pan Nan <pannan@allwinnertech.com>
 *
 * Copyright (C) 2014 Maxime Ripard
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <spi.h>
#include <errno.h>
#include <fdt_support.h>
#include <wait_bit.h>

#include <asm/bitops.h>
#include <asm/gpio.h>
#include <asm/io.h>

#include <asm/arch/clock.h>

#define SUN4I_FIFO_DEPTH	64

#define SUN4I_RXDATA_REG	0x00

#define SUN4I_TXDATA_REG	0x04

#define SUN4I_CTL_REG		0x08
#define SUN4I_CTL_ENABLE		BIT(0)
#define SUN4I_CTL_MASTER		BIT(1)
#define SUN4I_CTL_CPHA			BIT(2)
#define SUN4I_CTL_CPOL			BIT(3)
#define SUN4I_CTL_CS_ACTIVE_LOW		BIT(4)
#define SUN4I_CTL_LMTF			BIT(6)
#define SUN4I_CTL_TF_RST		BIT(8)
#define SUN4I_CTL_RF_RST		BIT(9)
#define SUN4I_CTL_XCH_MASK		0x0400
#define SUN4I_CTL_XCH			BIT(10)
#define SUN4I_CTL_CS_MASK		0x3000
#define SUN4I_CTL_CS(cs)		(((cs) << 12) & SUN4I_CTL_CS_MASK)
#define SUN4I_CTL_DHB			BIT(15)
#define SUN4I_CTL_CS_MANUAL		BIT(16)
#define SUN4I_CTL_CS_LEVEL		BIT(17)
#define SUN4I_CTL_TP			BIT(18)

#define SUN4I_INT_CTL_REG	0x0c
#define SUN4I_INT_CTL_RF_F34		BIT(4)
#define SUN4I_INT_CTL_TF_E34		BIT(12)
#define SUN4I_INT_CTL_TC		BIT(16)

#define SUN4I_INT_STA_REG	0x10

#define SUN4I_DMA_CTL_REG	0x14

#define SUN4I_WAIT_REG		0x18

#define SUN4I_CLK_CTL_REG	0x1c
#define SUN4I_CLK_CTL_CDR2_MASK		0xff
#define SUN4I_CLK_CTL_CDR2(div)		((div) & SUN4I_CLK_CTL_CDR2_MASK)
#define SUN4I_CLK_CTL_CDR1_MASK		0xf
#define SUN4I_CLK_CTL_CDR1(div)		(((div) & SUN4I_CLK_CTL_CDR1_MASK) << 8)
#define SUN4I_CLK_CTL_DRS		BIT(12)

#define SUN4I_MAX_XFER_SIZE		0xffffff

#define SUN4I_BURST_CNT_REG	0x20
#define SUN4I_BURST_CNT(cnt)		((cnt) & SUN4I_MAX_XFER_SIZE)

#define SUN4I_XMIT_CNT_REG	0x24
#define SUN4I_XMIT_CNT(cnt)		((cnt) & SUN4I_MAX_XFER_SIZE)

#define SUN4I_FIFO_STA_REG	0x28
#define SUN4I_FIFO_STA_RF_CNT_MASK	0x7f
#define SUN4I_FIFO_STA_RF_CNT_BITS	0
#define SUN4I_FIFO_STA_TF_CNT_MASK	0x7f
#define SUN4I_FIFO_STA_TF_CNT_BITS	16

#define SUN4I_SPI_MAX_RATE	24000000
#define SUN4I_SPI_MIN_RATE	3000
#define SUN4I_SPI_DEFAULT_RATE	1000000
#define SUN4I_SPI_TIMEOUT_US	1000000

/* sun4i spi register set */
struct sun4i_spi_regs {
	u32 rxdata;
	u32 txdata;
	u32 ctl;
	u32 intctl;
	u32 st;
	u32 dmactl;
	u32 wait;
	u32 cctl;
	u32 bc;
	u32 tc;
	u32 fifo_sta;
};

struct sun4i_spi_platdata {
	u32 base_addr;
	u32 max_hz;
};

struct sun4i_spi_priv {
	struct sun4i_spi_regs *regs;
	u32 freq;
	u32 mode;

	const u8 *tx_buf;
	u8 *rx_buf;
};

DECLARE_GLOBAL_DATA_PTR;

static inline void sun4i_spi_drain_fifo(struct sun4i_spi_priv *priv, int len)
{
	u8 byte;

	while (len--) {
		byte = readb(&priv->regs->rxdata);
		*priv->rx_buf++ = byte;
	}
}

static inline void sun4i_spi_fill_fifo(struct sun4i_spi_priv *priv, int len)
{
	u8 byte;

	while (len--) {
		byte = priv->tx_buf ? *priv->tx_buf++ : 0;
		writeb(byte, &priv->regs->txdata);
	}
}

static void sun4i_spi_set_cs(struct udevice *bus, u8 cs, bool enable)
{
	struct sun4i_spi_priv *priv = dev_get_priv(bus);
	u32 reg;

	reg = readl(&priv->regs->ctl);

	reg &= ~SUN4I_CTL_CS_MASK;
	reg |= SUN4I_CTL_CS(cs);

	if (enable)
		reg &= ~SUN4I_CTL_CS_LEVEL;
	else
		reg |= SUN4I_CTL_CS_LEVEL;

	writel(reg, &priv->regs->ctl);
}

static int sun4i_spi_parse_pins(struct udevice *dev)
{
	const void *fdt = gd->fdt_blob;
	const char *pin_name;
	const fdt32_t *list;
	u32 phandle;
	int drive, pull = 0, pin, i;
	int offset;
	int size;

	list = fdt_getprop(fdt, dev_of_offset(dev), "pinctrl-0", &size);
	if (!list) {
		printf("WARNING: sun4i_spi: cannot find pinctrl-0 node\n");
		return -EINVAL;
	}

	while (size) {
		phandle = fdt32_to_cpu(*list++);
		size -= sizeof(*list);

		offset = fdt_node_offset_by_phandle(fdt, phandle);
		if (offset < 0)
			return offset;

		drive = fdt_getprop_u32_default_node(fdt, offset, 0,
						     "drive-strength", 0);
		if (drive) {
			if (drive <= 10)
				drive = 0;
			else if (drive <= 20)
				drive = 1;
			else if (drive <= 30)
				drive = 2;
			else
				drive = 3;
		} else {
			drive = fdt_getprop_u32_default_node(fdt, offset, 0,
							     "allwinner,drive",
							      0);
			drive = min(drive, 3);
		}

		if (fdt_get_property(fdt, offset, "bias-disable", NULL))
			pull = 0;
		else if (fdt_get_property(fdt, offset, "bias-pull-up", NULL))
			pull = 1;
		else if (fdt_get_property(fdt, offset, "bias-pull-down", NULL))
			pull = 2;
		else
			pull = fdt_getprop_u32_default_node(fdt, offset, 0,
							    "allwinner,pull",
							     0);
		pull = min(pull, 2);

		for (i = 0; ; i++) {
			pin_name = fdt_stringlist_get(fdt, offset,
						      "pins", i, NULL);
			if (!pin_name) {
				pin_name = fdt_stringlist_get(fdt, offset,
							      "allwinner,pins",
							       i, NULL);
				if (!pin_name)
					break;
			}

			pin = name_to_gpio(pin_name);
			if (pin < 0)
				break;

			sunxi_gpio_set_cfgpin(pin, SUNXI_GPC_SPI0);
			sunxi_gpio_set_drv(pin, drive);
			sunxi_gpio_set_pull(pin, pull);
		}
	}
	return 0;
}

static inline void sun4i_spi_enable_clock(void)
{
	struct sunxi_ccm_reg *const ccm =
		(struct sunxi_ccm_reg *const)SUNXI_CCM_BASE;

	setbits_le32(&ccm->ahb_gate0, (1 << AHB_GATE_OFFSET_SPI0));
	writel((1 << 31), &ccm->spi0_clk_cfg);
}

static int sun4i_spi_ofdata_to_platdata(struct udevice *bus)
{
	struct sun4i_spi_platdata *plat = dev_get_platdata(bus);
	int node = dev_of_offset(bus);

	plat->base_addr = devfdt_get_addr(bus);
	plat->max_hz = fdtdec_get_int(gd->fdt_blob, node,
				      "spi-max-frequency",
				      SUN4I_SPI_DEFAULT_RATE);

	if (plat->max_hz > SUN4I_SPI_MAX_RATE)
		plat->max_hz = SUN4I_SPI_MAX_RATE;

	return 0;
}

static int sun4i_spi_probe(struct udevice *bus)
{
	struct sun4i_spi_platdata *plat = dev_get_platdata(bus);
	struct sun4i_spi_priv *priv = dev_get_priv(bus);

	sun4i_spi_enable_clock();
	sun4i_spi_parse_pins(bus);

	priv->regs = (struct sun4i_spi_regs *)(uintptr_t)plat->base_addr;
	priv->freq = plat->max_hz;

	return 0;
}

static int sun4i_spi_claim_bus(struct udevice *dev)
{
	struct sun4i_spi_priv *priv = dev_get_priv(dev->parent);

	writel(SUN4I_CTL_ENABLE | SUN4I_CTL_MASTER | SUN4I_CTL_TP |
	       SUN4I_CTL_CS_MANUAL | SUN4I_CTL_CS_ACTIVE_LOW,
	       &priv->regs->ctl);
	return 0;
}

static int sun4i_spi_release_bus(struct udevice *dev)
{
	struct sun4i_spi_priv *priv = dev_get_priv(dev->parent);
	u32 reg;

	reg = readl(&priv->regs->ctl);
	reg &= ~SUN4I_CTL_ENABLE;
	writel(reg, &priv->regs->ctl);

	return 0;
}

static int sun4i_spi_xfer(struct udevice *dev, unsigned int bitlen,
			  const void *dout, void *din, unsigned long flags)
{
	struct udevice *bus = dev->parent;
	struct sun4i_spi_priv *priv = dev_get_priv(bus);
	struct dm_spi_slave_platdata *slave_plat = dev_get_parent_platdata(dev);

	u32 len = bitlen / 8;
	u32 reg;
	u8 nbytes;
	int ret;

	priv->tx_buf = dout;
	priv->rx_buf = din;

	if (bitlen % 8) {
		debug("%s: non byte-aligned SPI transfer.\n", __func__);
		return -ENAVAIL;
	}

	if (flags & SPI_XFER_BEGIN)
		sun4i_spi_set_cs(bus, slave_plat->cs, true);

	reg = readl(&priv->regs->ctl);

	/* Reset FIFOs */
	writel(reg | SUN4I_CTL_RF_RST | SUN4I_CTL_TF_RST, &priv->regs->ctl);

	while (len) {
		/* Setup the transfer now... */
		nbytes = min(len, (u32)(SUN4I_FIFO_DEPTH - 1));

		/* Setup the counters */
		writel(SUN4I_BURST_CNT(nbytes), &priv->regs->bc);
		writel(SUN4I_XMIT_CNT(nbytes), &priv->regs->tc);

		/* Fill the TX FIFO */
		sun4i_spi_fill_fifo(priv, nbytes);

		/* Start the transfer */
		reg = readl(&priv->regs->ctl);
		writel(reg | SUN4I_CTL_XCH, &priv->regs->ctl);

		/* Wait transfer to complete */
		ret = wait_for_bit_le32(&priv->regs->ctl, SUN4I_CTL_XCH_MASK,
					false, SUN4I_SPI_TIMEOUT_US, false);
		if (ret) {
			printf("ERROR: sun4i_spi: Timeout transferring data\n");
			sun4i_spi_set_cs(bus, slave_plat->cs, false);
			return ret;
		}

		/* Drain the RX FIFO */
		sun4i_spi_drain_fifo(priv, nbytes);

		len -= nbytes;
	}

	if (flags & SPI_XFER_END)
		sun4i_spi_set_cs(bus, slave_plat->cs, false);

	return 0;
}

static int sun4i_spi_set_speed(struct udevice *dev, uint speed)
{
	struct sun4i_spi_platdata *plat = dev_get_platdata(dev);
	struct sun4i_spi_priv *priv = dev_get_priv(dev);
	unsigned int div;
	u32 reg;

	if (speed > plat->max_hz)
		speed = plat->max_hz;

	if (speed < SUN4I_SPI_MIN_RATE)
		speed = SUN4I_SPI_MIN_RATE;
	/*
	 * Setup clock divider.
	 *
	 * We have two choices there. Either we can use the clock
	 * divide rate 1, which is calculated thanks to this formula:
	 * SPI_CLK = MOD_CLK / (2 ^ (cdr + 1))
	 * Or we can use CDR2, which is calculated with the formula:
	 * SPI_CLK = MOD_CLK / (2 * (cdr + 1))
	 * Whether we use the former or the latter is set through the
	 * DRS bit.
	 *
	 * First try CDR2, and if we can't reach the expected
	 * frequency, fall back to CDR1.
	 */

	div = SUN4I_SPI_MAX_RATE / (2 * speed);
	reg = readl(&priv->regs->cctl);

	if (div <= (SUN4I_CLK_CTL_CDR2_MASK + 1)) {
		if (div > 0)
			div--;

		reg &= ~(SUN4I_CLK_CTL_CDR2_MASK | SUN4I_CLK_CTL_DRS);
		reg |= SUN4I_CLK_CTL_CDR2(div) | SUN4I_CLK_CTL_DRS;
	} else {
		div = __ilog2(SUN4I_SPI_MAX_RATE) - __ilog2(speed);
		reg &= ~((SUN4I_CLK_CTL_CDR1_MASK << 8) | SUN4I_CLK_CTL_DRS);
		reg |= SUN4I_CLK_CTL_CDR1(div);
	}

	priv->freq = speed;
	writel(reg, &priv->regs->cctl);

	return 0;
}

static int sun4i_spi_set_mode(struct udevice *dev, uint mode)
{
	struct sun4i_spi_priv *priv = dev_get_priv(dev);
	u32 reg;

	reg = readl(&priv->regs->ctl);
	reg &= ~(SUN4I_CTL_CPOL | SUN4I_CTL_CPHA);

	if (mode & SPI_CPOL)
		reg |= SUN4I_CTL_CPOL;

	if (mode & SPI_CPHA)
		reg |= SUN4I_CTL_CPHA;

	priv->mode = mode;
	writel(reg, &priv->regs->ctl);

	return 0;
}

static const struct dm_spi_ops sun4i_spi_ops = {
	.claim_bus		= sun4i_spi_claim_bus,
	.release_bus		= sun4i_spi_release_bus,
	.xfer			= sun4i_spi_xfer,
	.set_speed		= sun4i_spi_set_speed,
	.set_mode		= sun4i_spi_set_mode,
};

static const struct udevice_id sun4i_spi_ids[] = {
	{ .compatible = "allwinner,sun4i-a10-spi"  },
	{ }
};

U_BOOT_DRIVER(sun4i_spi) = {
	.name	= "sun4i_spi",
	.id	= UCLASS_SPI,
	.of_match	= sun4i_spi_ids,
	.ops	= &sun4i_spi_ops,
	.ofdata_to_platdata	= sun4i_spi_ofdata_to_platdata,
	.platdata_auto_alloc_size	= sizeof(struct sun4i_spi_platdata),
	.priv_auto_alloc_size	= sizeof(struct sun4i_spi_priv),
	.probe	= sun4i_spi_probe,
};
