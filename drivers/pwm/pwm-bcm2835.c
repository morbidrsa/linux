/*
 * BCM2835 PWM driver
 *
 * Derived from the Tegra PWM driver by NVIDIA Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pwm.h>

#define NPWM 2

/* Address Mappings (offsets) from Broadcom BCM2835 ARM Periperals Manual
 * Section 9.6 Page 141ff
 */
#define BCM2835_PWM_CTL		0x00 /* Control register */
#define BCM2835_PWM_STA		0x04 /* Status register */
#define BCM2835_PWM_DMAC	0x08 /* PWM DMA Configuration */
#define BCM2835_PWM_RNG1	0x10 /* PWM Channel 1 Range */
#define BCM2835_PWM_DAT1	0x14 /* PWM Channel 1 Data */
#define BCM2835_PWM_FIF1	0x18 /* PWM FIFO Input */
#define BCM2835_PWM_RNG2	0x20 /* PWM Channel 2 Range */
#define BCM2835_PWM_DAT2	0x24 /* PWM Channel 2 Data */

/* Control Register Bits */
#define BCM2835_PWM_CTL_PWEN1	BIT(0)	/* Channel 1 enable (RW) */
#define BCM2835_PWM_CTL_MODE1	BIT(1)	/* Channel 1 mode (RW) */
#define BCM2835_PWM_CTL_RPTL1	BIT(2)	/* Channel 1 repeat last data (RW) */
#define BCM2835_PWM_CTL_SBIT1	BIT(3)	/* Channel 1 silence bit (RW) */
#define BCM2835_PWM_CTL_POLA1	BIT(4)	/* Channel 1 polarity (RW) */
#define BCM2835_PWM_CTL_USEF1	BIT(5)	/* Channel 1 use FIFO (RW) */
#define BCM2835_PWM_CTL_CLRF1	BIT(6)	/* Channel 1 clear FIFO (RO) */
#define BCM2835_PWM_CTL_MSEN1	BIT(7)	/* Channel 1 M/S enable (RW) */
#define BCM2835_PWM_CTL_PWEN2	BIT(8)	/* Channel 2 enable (RW) */
#define BCM2835_PWM_CTL_MODE2	BIT(9)	/* Channel 2 mode (RW) */
#define BCM2835_PWM_CTL_RPTL2	BIT(10)	/* Channel 2 repeat last data (RW) */
#define BCM2835_PWM_CTL_SBIT2	BIT(11)	/* Channel 2 silence bit (RW) */
#define BCM2835_PWM_CTL_POLA2	BIT(12)	/* Channel 2 polarity (RW) */
#define BCM2835_PWM_CTL_USEF2	BIT(13)	/* Channel 2 use FIFO (RW) */
/* Bit 14 is reserved */
#define BCM2835_PWM_MSEN2	BIT(15)	/* Channel 2 M/S enable (RW) */
/* Bits 16 - 31 are reserved */

/* Status Register Bits */
#define BCM2835_PWM_STA_FULL1	BIT(0)	/* FIFO full flag (RW) */
#define BCM2835_PWM_STA_EMPT1	BIT(1)	/* FIFO empty flag (RW) */
#define BCM2835_PWM_STA_WERR1	BIT(2)	/* FIFO write error flag (RW) */
#define BCM2835_PWM_STA_RERR1	BIT(3)	/* FIFO read error flag (RW) */
#define BCM2835_PWM_STA_GAPO1	BIT(4)	/* Channel 1 gap occured (RW) */
#define BCM2835_PWM_STA_GAPO2	BIT(5)	/* Channel 2 gap occured (RW) */
#define BCM2835_PWM_STA_GAPO3	BIT(6)	/* Channel 3 gap occured (RW) */
#define BCM2835_PWM_STA_GAPO4	BIT(7)	/* Channel 4 gap occured (RW) */
#define BCM2835_PWM_STA_BERR	BIT(8)	/* Bus error flag (RW) */
#define BCM2835_PWM_STA_STA1	BIT(9)	/* Channel 1 state (RW) */
#define BCM2835_PWM_STA_STA2	BIT(10)	/* Channel 1 state (RW) */
#define BCM2835_PWM_STA_STA3	BIT(11)	/* Channel 1 state (RW) */
#define BCM2835_PWM_STA_STA4	BIT(12)	/* Channel 1 state (RW) */
/* Bits 13 - 31 are reserved */

struct bcm2835_pwm_dev {
	struct pwm_chip chip;
	struct device *dev;
	struct clk *clk;
	void __iomem *mmio_base;
};

static inline struct bcm2835_pwm_dev *to_bcm(struct pwm_chip *chip)
{
	return container_of(chip, struct bcm2835_pwm_dev, chip);
}

static inline u32 bcm2835_readl(struct bcm2835_pwm_dev *chip, unsigned int num)
{
	return readl(chip->mmio_base + num);
}

static inline void bcm2835_writel(struct bcm2835_pwm_dev *chip, unsigned int num,
				  unsigned long val)
{
	writel(val, chip->mmio_base + num);
}

static int bcm2835_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			      int duty_ns, int period_ns)
{
	struct bcm2835_pwm_dev *bcm = to_bcm(chip);

	if (WARN_ON(!bcm))
		return -ENODEV;

	if (duty_ns < 1) {
		dev_err(bcm->dev, "duty is out of range: %d < 1\n", duty_ns);
		return -ERANGE;
	}

	if (period_ns < 1) {
		dev_err(bcm->dev, "period is out of range: %d < 1\n",
			period_ns);
		return -ERANGE;
	}

	/* disable PWM */
	bcm2835_writel(bcm, BCM2835_PWM_CTL, 0);

	/* write period */
	bcm2835_writel(bcm, BCM2835_PWM_RNG1, period_ns);

	/* write duty */
	bcm2835_writel(bcm, BCM2835_PWM_DAT1, duty_ns);

	/* enable MSEN mode and start PWM */
	bcm2835_writel(bcm, BCM2835_PWM_CTL, 0x81);

	return 0;
}

static int bcm2835_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct bcm2835_pwm_dev *bcm = to_bcm(chip);
	int rc = 0;
	u32 val;

	if (WARN_ON(!bcm))
		return -ENODEV;

	rc = clk_prepare_enable(bcm->clk);
	if (rc < 0)
		return rc;

	val = bcm2835_readl(bcm, BCM2835_PWM_CTL);
	val |= BCM2835_PWM_CTL_PWEN1;
	bcm2835_writel(bcm, BCM2835_PWM_CTL, val);

	return 0;
}

static void bcm2835_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct bcm2835_pwm_dev *bcm = to_bcm(chip);
	u32 val;

	if (WARN_ON(!bcm))
		return;

	val = bcm2835_readl(bcm, BCM2835_PWM_CTL);
	val &= ~BCM2835_PWM_CTL_PWEN1;
	bcm2835_writel(bcm, BCM2835_PWM_CTL, val);

	clk_disable_unprepare(bcm->clk);
}

static int bcm2835_set_polarity(struct pwm_chip *chip, struct pwm_device *pwm,
				enum pwm_polarity polarity)
{
	struct bcm2835_pwm_dev *bcm = to_bcm(chip);
	u32 val;

	if (WARN_ON(!bcm))
		return -ENODEV;

	val = bcm2835_readl(bcm, BCM2835_PWM_CTL);

	if (polarity == PWM_POLARITY_NORMAL)
		val |= BCM2835_PWM_CTL_POLA1;
	else
		val &= ~BCM2835_PWM_CTL_POLA1;

	return 0;
}

static const struct pwm_ops bcm2835_pwm_ops = {
	.config  = bcm2835_pwm_config,
	.enable  = bcm2835_pwm_enable,
	.disable = bcm2835_pwm_disable,
	.set_polarity = bcm2835_set_polarity,
};

static int bcm2835_pwm_probe(struct platform_device *pdev)
{
	struct bcm2835_pwm_dev *bcm;
	struct resource *r;
	int ret;

	bcm = devm_kzalloc(&pdev->dev, sizeof(*bcm), GFP_KERNEL);
	if (!bcm)
		return -ENOMEM;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r) {
		dev_err(&pdev->dev, "no memory resource defined\n");
		return -ENODEV;
	}

	bcm->mmio_base = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(bcm->mmio_base))
		return PTR_ERR(bcm->mmio_base);

	bcm->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(bcm->clk))
		return PTR_ERR(bcm->clk);

	clk_prepare_enable(bcm->clk);

	bcm->chip.ops = &bcm2835_pwm_ops;
	bcm->chip.dev = &pdev->dev;
	bcm->chip.base = -1;
	bcm->chip.npwm = NPWM;

	ret = pwmchip_add(&bcm->chip);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, bcm);

	return 0;
}

static int bcm2835_pwm_remove(struct platform_device *pdev)
{
	struct bcm2835_pwm_dev *bcm = platform_get_drvdata(pdev);

	if (WARN_ON(!bcm))
		return -ENODEV;

	clk_disable_unprepare(bcm->clk);

	return pwmchip_remove(&bcm->chip);
}

static const struct of_device_id bcm2835_pwm_of_match[] = {
	{ .compatible = "brcm,bcm2835-pwm" },
	{},
};

MODULE_DEVICE_TABLE(of, bcm2835_pwm_of_match);

static struct platform_driver bcm2835_pwm_driver = {
	.probe		= bcm2835_pwm_probe,
	.remove		= bcm2835_pwm_remove,
	.driver	= {
		.name	= "pwm-bcm2835",
		.owner	= THIS_MODULE,
		.of_match_table = bcm2835_pwm_of_match,
	},
};

module_platform_driver(bcm2835_pwm_driver);

MODULE_AUTHOR("Johannes Thumshirn <morbidrsa@gmail.com>");
MODULE_DESCRIPTION("BCM2835 PWM driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pwm-bcm2835");
