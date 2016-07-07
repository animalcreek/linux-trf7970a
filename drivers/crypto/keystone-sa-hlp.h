/*
 * Keystone crypto accelerator driver
 *
 * Copyright (C) 2015,2016 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors:	Sandeep Nair
 *		Vitaly Andrianov
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

#ifndef _KEYSTONE_SA_HLP_
#define _KEYSTONE_SA_HLP_

#include <linux/soc/ti/knav_dma.h>
#include <linux/regmap.h>

#define SA_PID_OFS		0
#define SA_CMD_STATUS_OFS	0x8
#define SA_PA_FLOWID_OFS	0x10
#define SA_CDMA_FLOWID_OFS	0x14
#define	SA_PA_ENG_ID_OFS	0x18
#define	SA_CDMA_ENG_ID_OFS	0x1C

/* Crypto driver instance data */
struct keystone_crypto_data {
	struct platform_device	*pdev;
	struct clk		*clk;
	struct regmap	*sa_regmap;
	u32		tx_submit_qid;
	u32		tx_compl_qid;
	u32		rx_compl_qid;
	const char	*rx_chan_name;
	const char	*tx_chan_name;
	u32		tx_queue_depth;
	u32		rx_queue_depths[KNAV_DMA_FDQ_PER_CHAN];
	u32		rx_pool_size;
	u32		rx_pool_region_id;
	u32		tx_pool_size;
	u32		tx_pool_region_id;

	/* Security context data */
	u16		sc_id_start;
	u16		sc_id_end;
	u16		sc_id;
	atomic_t	rx_dma_page_cnt; /* N buf from 2nd pool available */
	atomic_t	tx_dma_desc_cnt; /* Tx DMA desc-s available */
};

extern struct device *sa_ks2_dev;

#endif /* _KEYSTONE_SA_HLP_ */
