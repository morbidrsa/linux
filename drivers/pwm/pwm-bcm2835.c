/*
 * BCM2835 PWM driver
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
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define BCM2835_PWM_CTL		0x00 /* Control register */

#define BCM2835_PWM_CTL_EN(x) (BIT((x) * 8))
#define BCM2835_PWM_RNG(x) (0x10 + (x) * 16)
#define BCM2835_PWM_DAT(x) (0x14 + (x) * 16)
#define BCM2835_PWM_POL(x) (0x10 + (x) * 0xff0)

struct bcm2835_pwm {
	struct pwm_chip chip;
	struct device *dev;
	struct clk *clk;
	void __iomem *mmio;
};

static inline struct bcm2835_pwm *to_bcm(struct pwm_chip *chip)
{
	return container_of(chip, struct bcm2835_pwm, chip);
}

static int bcm2835_pwm_config(struct pwm_chip *chip, struct pwm_device *pwm,
			      int duty_ns, int period_ns)
{
	struct bcm2835_pwm *bcm = to_bcm(chip);
	unsigned long rng;
	unsigned long clk;
	unsigned long dat;
	u32 ctl;

	clk = clk_get_rate(bcm->clk);

	ctl = readl(bcm->mmio + BCM2835_PWM_CTL);
	ctl &= ~BCM2835_PWM_CTL_EN(pwm->hwpwm);
	writel(ctl, bcm->mmio + BCM2835_PWM_CTL);

	rng = period_ns * clk;
	writel(rng, bcm->mmio + BCM2835_PWM_RNG(pwm->hwpwm));

	dat = rng * duty_ns / 100;
	writel(duty_ns, bcm->mmio + BCM2835_PWM_DAT(pwm->hwpwm));

	ctl |= BCM2835_PWM_CTL_EN(pwm->hwpwm);
	writel(ctl, bcm->mmio + BCM2835_PWM_CTL);

	return 0;
}

static int bcm2835_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct bcm2835_pwm *bcm = to_bcm(chip);
	int rc;
	u32 ctl;

	rc = clk_enable(bcm->clk);
	if (rc < 0)
		return rc;

	ctl = readl(bcm->mmio + BCM2835_PWM_CTL);
	ctl |= BCM2835_PWM_CTL_EN(pwm->hwpwm);
	writel(ctl, bcm->mmio + BCM2835_PWM_CTL);

	return 0;
}

static void bcm2835_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct bcm2835_pwm *bcm = to_bcm(chip);
	u32 ctl;

	ctl = readl(bcm->mmio + BCM2835_PWM_CTL);
	ctl &= ~BCM2835_PWM_CTL_EN(pwm->hwpwm);
	writel(ctl, bcm->mmio + BCM2835_PWM_CTL);

	clk_disable(bcm->clk);
}

static int bcm2835_set_polarity(struct pwm_chip *chip, struct pwm_device *pwm,
				enum pwm_polarity polarity)
{
	struct bcm2835_pwm *bcm = to_bcm(chip);
	u32 ctl;

	ctl = readl(bcm->mmio + BCM2835_PWM_CTL);

	if (polarity == PWM_POLARITY_NORMAL)
		ctl &= ~BCM2835_PWM_POL(pwm->hwpwm);
	else
		ctl |= BCM2835_PWM_POL(pwm->hwpwm);

	writel(ctl, bcm->mmio + BCM2835_PWM_CTL);

	return 0;
}

static const struct pwm_ops bcm2835_pwm_ops = {
	.owner = THIS_MODULE,
	.config = bcm2835_pwm_config,
	.enable = bcm2835_pwm_enable,
	.disable = bcm2835_pwm_disable,
	.set_polarity = bcm2835_set_polarity,
};

static int bcm2835_pwm_probe(struct platform_device *pdev)
{
	struct bcm2835_pwm *bcm;
	struct resource *r;
	int ret;

	bcm = devm_kzalloc(&pdev->dev, sizeof(*bcm), GFP_KERNEL);
	if (!bcm)
		return -ENOMEM;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	bcm->mmio = devm_ioremap_resource(&pdev->dev, r);
	if (IS_ERR(bcm->mmio))
		return PTR_ERR(bcm->mmio);

	bcm->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(bcm->clk))
		return PTR_ERR(bcm->clk);

	clk_prepare_enable(bcm->clk);
	if (IS_ERR(bcm->clk))
		return PTR_ERR(bcm->clk);

	bcm->chip.ops = &bcm2835_pwm_ops;
	bcm->chip.dev = &pdev->dev;
	bcm->chip.base = -1;
	bcm->chip.npwm = 2;

	platform_set_drvdata(pdev, bcm);

	ret = pwmchip_add(&bcm->chip);
	if (ret < 0)
		return ret;

	return 0;
}

static int bcm2835_pwm_remove(struct platform_device *pdev)
{
	struct bcm2835_pwm *bcm = platform_get_drvdata(pdev);

	clk_disable_unprepare(bcm->clk);

	return pwmchip_remove(&bcm->chip);
}

static const struct of_device_id bcm2835_pwm_of_match[] = {
	{ .compatible = "brcm,bcm2835-pwm" },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_pwm_of_match);

static struct platform_driver bcm2835_pwm_driver = {
	.probe = bcm2835_pwm_probe,
	.remove	= bcm2835_pwm_remove,
	.driver	= {
		.name = "bcm2835-pwm",
		.owner = THIS_MODULE,
		.of_match_table = bcm2835_pwm_of_match,
	},
};
module_platform_driver(bcm2835_pwm_driver);

MODULE_AUTHOR("Johannes Thumshirn <morbidrsa@gmail.com>");
MODULE_DESCRIPTION("BCM2835 PWM driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:bcm2835-pwm");
