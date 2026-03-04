// SPDX-License-Identifier: GPL-2.0-only
/*
 * MEN Chameleon Bus.
 *
 * Copyright (C) 2014 MEN Mikroelektronik GmbH (www.men.de)
 * Author: Andreas Werner <andreas.werner@men.de>
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/dmi.h>
#include <linux/mcb.h>
#include <linux/io.h>
#include "mcb-internal.h"

struct priv {
	struct mcb_bus *bus;
	struct resource *mem;
	void __iomem *base;
};

static int mcb_lpc_get_bars(void __iomem **base, struct chameleon_bar **cb,
			    struct device *pdev)
{
	struct chameleon_bar *c;
	int bar_count;
	__le32 reg;
	char __iomem *p = *base;
	int i;

	/*
	 * For those devices which are not connected
	 * to the PCI Bus (e.g. LPC) there is a bar
	 * descriptor located directly after the
	 * chameleon header. This header is comparable
	 * to a PCI header.
	 */
	reg = readl(*base);

	bar_count = BAR_CNT(reg);
	if (bar_count <= 0 || bar_count > CHAMELEON_BAR_MAX)
		return -ENODEV;

	c = kzalloc_objs(struct chameleon_bar, bar_count);
	if (!c)
		return -ENOMEM;

	/* skip reg1 */
	p += sizeof(__le32);

	for (i = 0; i < bar_count; i++) {
		c[i].addr = readl(p);
		c[i].size = readl(p + 4);

		p += sizeof(struct chameleon_bar);
	}
	*base += BAR_DESC_SIZE(bar_count);
	*cb = c;

	return bar_count;
}

static bool mcb_is_lpc_bar_iomapped(struct device *dev,
				      struct chameleon_bar *cb, int bar)
{
	if (cb[bar].addr & 0x01)
		return true;
	return false;
}

static struct chameleon_parse_ops lpc_parse_ops = {
	.is_bar_iomapped = mcb_is_lpc_bar_iomapped,
	.get_bars = mcb_lpc_get_bars,
};

static int mcb_lpc_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct priv *priv;
	int ret = 0, table_size;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!priv->mem) {
		dev_err(&pdev->dev, "No Memory resource\n");
		return -ENODEV;
	}

	res = devm_request_mem_region(&pdev->dev, priv->mem->start,
				      resource_size(priv->mem),
				      KBUILD_MODNAME);
	if (!res) {
		dev_err(&pdev->dev, "Failed to request IO memory\n");
		return -EBUSY;
	}

	priv->base = devm_ioremap(&pdev->dev, priv->mem->start,
				  resource_size(priv->mem));
	if (!priv->base) {
		dev_err(&pdev->dev, "Cannot ioremap\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, priv);

	priv->bus = mcb_alloc_bus(&pdev->dev);
	if (IS_ERR(priv->bus))
		return PTR_ERR(priv->bus);

	ret = chameleon_parse_cells(priv->bus, priv->base, &lpc_parse_ops);
	if (ret < 0) {
		goto out_mcb_bus;
	}

	table_size = ret;

	if (table_size < CHAM_HEADER_SIZE) {
		/* Release the previous resources */
		devm_iounmap(&pdev->dev, priv->base);
		devm_release_mem_region(&pdev->dev, priv->mem->start, resource_size(priv->mem));

		/* Then, allocate it again with the actual chameleon table size */
		res = devm_request_mem_region(&pdev->dev, priv->mem->start,
					      table_size,
					      KBUILD_MODNAME);
		if (!res) {
			dev_err(&pdev->dev, "Failed to request PCI memory\n");
			ret = -EBUSY;
			goto out_mcb_bus;
		}

		priv->base = devm_ioremap(&pdev->dev, priv->mem->start, table_size);
		if (!priv->base) {
			dev_err(&pdev->dev, "Cannot ioremap\n");
			ret = -ENOMEM;
			goto out_mcb_bus;
		}

		platform_set_drvdata(pdev, priv);
	}

	mcb_bus_add_devices(priv->bus);

	return 0;

out_mcb_bus:
	mcb_release_bus(priv->bus);
	return ret;
}

static void mcb_lpc_remove(struct platform_device *pdev)
{
	struct priv *priv = platform_get_drvdata(pdev);

	mcb_release_bus(priv->bus);
}

static struct platform_device *mcb_lpc_pdev;

static int mcb_lpc_create_platform_device(const struct dmi_system_id *id)
{
	struct resource *res = id->driver_data;
	int ret;

	mcb_lpc_pdev = platform_device_alloc("mcb-lpc", -1);
	if (!mcb_lpc_pdev)
		return -ENOMEM;

	ret = platform_device_add_resources(mcb_lpc_pdev, res, 1);
	if (ret)
		goto out_put;

	ret = platform_device_add(mcb_lpc_pdev);
	if (ret)
		goto out_put;

	return 0;

out_put:
	platform_device_put(mcb_lpc_pdev);
	return ret;
}

static struct resource sc24_fpga_resource = DEFINE_RES_MEM(0xe000e000, CHAM_HEADER_SIZE);
static struct resource sc31_fpga_resource = DEFINE_RES_MEM(0xf000e000, CHAM_HEADER_SIZE);

static struct platform_driver mcb_lpc_driver = {
	.driver		= {
		.name = "mcb-lpc",
	},
	.probe		= mcb_lpc_probe,
	.remove		= mcb_lpc_remove,
};

static const struct dmi_system_id mcb_lpc_dmi_table[] = {
	{
		.ident = "SC24",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MEN"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "14SC24"),
		},
		.driver_data = (void *)&sc24_fpga_resource,
		.callback = mcb_lpc_create_platform_device,
	},
	{
		.ident = "SC31",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MEN"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "14SC31"),
		},
		.driver_data = (void *)&sc31_fpga_resource,
		.callback = mcb_lpc_create_platform_device,
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, mcb_lpc_dmi_table);

static int __init mcb_lpc_init(void)
{
	if (!dmi_check_system(mcb_lpc_dmi_table))
		return -ENODEV;

	return platform_driver_register(&mcb_lpc_driver);
}

static void __exit mcb_lpc_exit(void)
{
	platform_device_unregister(mcb_lpc_pdev);
	platform_driver_unregister(&mcb_lpc_driver);
}

module_init(mcb_lpc_init);
module_exit(mcb_lpc_exit);

MODULE_AUTHOR("Andreas Werner <andreas.werner@men.de>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MCB over LPC support");
MODULE_IMPORT_NS("MCB");
