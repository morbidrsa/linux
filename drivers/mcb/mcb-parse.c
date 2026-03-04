// SPDX-License-Identifier: GPL-2.0-only
#include <linux/types.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/mcb.h>

#include "mcb-internal.h"

#define for_each_chameleon_cell(dtype, p)	\
	for ((dtype) = get_next_dtype((p));	\
	     (dtype) != CHAMELEON_DTYPE_END;	\
	     (dtype) = get_next_dtype((p)))

static inline uint32_t get_next_dtype(void __iomem *p)
{
	uint32_t dtype;

	dtype = readl(p);
	return dtype >> 28;
}

static int chameleon_parse_bdd(struct mcb_bus *bus,
			struct chameleon_bar *cb,
			void __iomem *base)
{
	return 0;
}

static int chameleon_parse_gdd(struct mcb_bus *bus, struct chameleon_bar *cb,
			       void __iomem *base, int bar_count,
			       struct chameleon_parse_ops *cham_ops)
{
	struct chameleon_gdd __iomem *gdd =
		(struct chameleon_gdd __iomem *) base;
	struct mcb_device *mdev;
	u32 dev_mapbase;
	u32 offset;
	u32 size;
	int ret;
	__le32 reg1;
	__le32 reg2;

	mdev = mcb_alloc_dev(bus);
	if (!mdev)
		return -ENOMEM;

	reg1 = readl(&gdd->reg1);
	reg2 = readl(&gdd->reg2);
	offset = readl(&gdd->offset);
	size = readl(&gdd->size);

	mdev->id = GDD_DEV(reg1);
	mdev->rev = GDD_REV(reg1);
	mdev->var = GDD_VAR(reg1);
	mdev->bar = GDD_BAR(reg2);
	mdev->group = GDD_GRP(reg2);
	mdev->inst = GDD_INS(reg2);

	/*
	 * If the BAR is missing, dev_mapbase is zero, or if the
	 * device is IO mapped we just print a warning and go on with the
	 * next device, instead of completely stop the gdd parser
	 */
	if (mdev->bar > bar_count - 1) {
		pr_info("No BAR for 16z%03d\n", mdev->id);
		ret = 0;
		goto err;
	}

	dev_mapbase = cb[mdev->bar].addr;
	if (!dev_mapbase) {
		pr_info("BAR not assigned for 16z%03d\n", mdev->id);
		ret = 0;
		goto err;
	}

	if (cham_ops->is_bar_iomapped(bus->carrier, cb, mdev->bar)) {
		pr_info("IO mapped Device (16z%03d) not yet supported\n",
			mdev->id);
		ret = 0;
		goto err;
	}

	pr_debug("Found a 16z%03d\n", mdev->id);

	mdev->irq.start = GDD_IRQ(reg1);
	mdev->irq.end = GDD_IRQ(reg1);
	mdev->irq.flags = IORESOURCE_IRQ;

	mdev->mem.start = dev_mapbase + offset;

	mdev->mem.end = mdev->mem.start + size - 1;
	mdev->mem.flags = IORESOURCE_MEM;

	ret = mcb_device_register(bus, mdev);
	if (ret < 0)
		return ret;

	return 0;

err:
	mcb_free_dev(mdev);

	return ret;
}

int chameleon_parse_cells(struct mcb_bus *bus, void __iomem *base,
			  struct chameleon_parse_ops *cham_bus_ops)
{
	struct chameleon_fpga_header *header;
	struct chameleon_bar *cb;
	void __iomem *p = base;
	int num_cells = 0;
	uint32_t dtype;
	int bar_count;
	int ret;
	u32 hsize;
	u32 table_size;

	hsize = sizeof(struct chameleon_fpga_header);

	header = kzalloc(hsize, GFP_KERNEL);
	if (!header)
		return -ENOMEM;

	/* Extract header information */
	memcpy_fromio(header, p, hsize);
	/* We only support chameleon v2 at the moment */
	header->magic = le16_to_cpu(header->magic);
	if (header->magic != CHAMELEONV2_MAGIC) {
		pr_err("Unsupported chameleon version 0x%x\n",
				header->magic);
		ret = -ENODEV;
		goto free_header;
	}
	p += hsize;

	bus->revision = header->revision;
	bus->model = header->model;
	bus->minor = header->minor;
	memcpy(bus->name, header->filename, CHAMELEON_FILENAME_LEN);
	bus->name[CHAMELEON_FILENAME_LEN] = '\0';

	bar_count = cham_bus_ops->get_bars(&p, &cb, bus->carrier);
	if (bar_count < 0) {
		ret = bar_count;
		goto free_header;
	}

	for_each_chameleon_cell(dtype, p) {
		switch (dtype) {
		case CHAMELEON_DTYPE_GENERAL:
			ret = chameleon_parse_gdd(bus, cb, p, bar_count,
						  cham_bus_ops);
			if (ret < 0)
				goto free_bar;
			p += sizeof(struct chameleon_gdd);
			break;
		case CHAMELEON_DTYPE_BRIDGE:
			chameleon_parse_bdd(bus, cb, p);
			p += sizeof(struct chameleon_bdd);
			break;
		case CHAMELEON_DTYPE_END:
			break;
		default:
			pr_err("Invalid chameleon descriptor type 0x%x\n",
				dtype);
			ret = -EINVAL;
			goto free_bar;
		}
		num_cells++;
	}

	if (num_cells == 0) {
		ret = -EINVAL;
		goto free_bar;
	}

	table_size = p - base;
	pr_debug("%d cell(s) found. Chameleon table size: 0x%04x bytes\n", num_cells, table_size);
	kfree(cb);
	kfree(header);
	return table_size;

free_bar:
	kfree(cb);
free_header:
	kfree(header);

	return ret;
}
EXPORT_SYMBOL_NS_GPL(chameleon_parse_cells, "MCB");
