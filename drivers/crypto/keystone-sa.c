/*
 * Keystone crypto accelerator driver
 *
 * Copyright (C) 2015, 2016 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors:	Sandeep Nair
 *		Vitaly Andrianov
 *
 * Contributors:Tinku Mannan
 *		Hao Zhang
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/soc/ti/knav_dma.h>
#include "keystone-sa-hlp.h"

struct device *sa_ks2_dev;

static int sa_read_dtb(struct device_node *node,
		       struct keystone_crypto_data *dev_data)
{
	int i, ret = 0;
	struct device *dev = &dev_data->pdev->dev;
	u32 temp[2];

	ret = of_property_read_string(node, "ti,tx-channel",
				      &dev_data->tx_chan_name);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,tx-channel\" parameter\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(node, "ti,tx-queue-depth",
				       &dev_data->tx_queue_depth);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,tx-queue-depth\" parameter\n");
		return -EINVAL;
	}

	atomic_set(&dev_data->tx_dma_desc_cnt, dev_data->tx_queue_depth);

	ret = of_property_read_u32(node, "ti,tx-submit-queue",
				       &dev_data->tx_submit_qid);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,tx-submit-queue\" parameter\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(node, "ti,tx-completion-queue",
				       &dev_data->tx_compl_qid);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,tx-completion-queue\" parameter\n");
		return -EINVAL;
	}

	ret = of_property_read_string(node, "ti,rx-channel",
				      &dev_data->rx_chan_name);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,rx-channel\" parameter\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "ti,rx-queue-depth",
					 dev_data->rx_queue_depths,
					 KNAV_DMA_FDQ_PER_CHAN);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,rx-queue-depth\" parameter\n");
		return -EINVAL;
	}
	for (i = 0; i < KNAV_DMA_FDQ_PER_CHAN; i++)
		dev_dbg(dev, "rx-queue-depth[%d]= %u\n", i,
			dev_data->rx_queue_depths[i]);

	atomic_set(&dev_data->rx_dma_page_cnt, 0);

	ret = of_property_read_u32(node, "ti,rx-compl-queue",
				       &dev_data->rx_compl_qid);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,rx-compl-queue\" parameter\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(node, "ti,tx-pool", temp, 2);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,tx-pool\" parameter\n");
		return -EINVAL;
	}
	dev_data->tx_pool_size = temp[0];
	dev_data->tx_pool_region_id = temp[1];

	ret = of_property_read_u32_array(node, "ti,rx-pool", temp, 2);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,rx-pool\" parameter\n");
		return -EINVAL;
	}
	dev_data->rx_pool_size = temp[0];
	dev_data->rx_pool_region_id = temp[1];

	ret = of_property_read_u32_array(node, "ti,sc-id", temp, 2);
	if (ret < 0) {
		dev_err(dev, "missing \"ti,sc-id\" parameter\n");
		return -EINVAL;
	}
	dev_data->sc_id_start = temp[0];
	dev_data->sc_id_end = temp[1];
	dev_data->sc_id = dev_data->sc_id_start;

	dev_data->sa_regmap = syscon_regmap_lookup_by_phandle(node,
							      "syscon-subsys");

	if (IS_ERR(dev_data->sa_regmap)) {
		dev_err(dev, "syscon_regmap_lookup_by_phandle failed\n");
		return -EINVAL;
	}

	return 0;
}

static int keystone_crypto_remove(struct platform_device *pdev)
{
	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static int keystone_crypto_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = pdev->dev.of_node;
	struct keystone_crypto_data *dev_data;
	int ret;

	sa_ks2_dev = dev;

	dev_data = devm_kzalloc(dev, sizeof(*dev_data), GFP_KERNEL);
	if (!dev_data)
		return -ENOMEM;

	dev_data->pdev = pdev;

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "Failed to enable SA power-domain\n");
		pm_runtime_disable(dev);
		return ret;
	}

	/* Read configuration from device tree */
	ret = sa_read_dtb(node, dev_data);
	if (ret) {
		dev_err(dev, "Failed to get all relevant configurations from DTB...\n");
		return ret;
	}

	platform_set_drvdata(pdev, dev_data);

	dev_info(dev, "crypto accelerator enabled\n");
	return 0;
}

static const struct of_device_id of_match[] = {
	{ .compatible = "ti,netcp-sa-crypto", },
	{},
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver keystone_crypto_driver = {
	.probe	= keystone_crypto_probe,
	.remove	= keystone_crypto_remove,
	.driver	= {
		.name		= "keystone-crypto",
		.of_match_table	= of_match,
	},
};

module_platform_driver(keystone_crypto_driver);

MODULE_DESCRIPTION("Keystone crypto acceleration support.");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sandeep Nair");
MODULE_AUTHOR("Vitaly Andrianov");

