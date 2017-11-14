/*
 * PRU IEP Driver
 *
 * Copyright (C) 2017 Texas Instruments Incorporated - http://www.ti.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef _TI_IEP_H_
#define _TI_IEP_H_

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clocksource.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/skbuff.h>
#include <linux/timecounter.h>

#define IEP_SYNC_EN                  BIT(0)
#define IEP_SYNC0_EN                 BIT(1)
#define IEP_CMP1_EN                  BIT(2)
#define IEP_CMP1_HIT                 BIT(1)

/* IEP reg offsets */
#define PRUSS_IEP_GLOBAL_CFG         0x00
#define PRUSS_IEP_COMPENSATION       0x08
#define PRUSS_IEP_SLOW_COMPENSATION  0x0C
#define PRUSS_IEP_COUNT_REG0         0x10
#define PRUSS_IEP_COUNT_REG1         0x14
#define PRUSS_IEP_CMP_CFG_REG        0x70
#define PRUSS_IEP_CMP_STAT_REG       0x74
#define PRUSS_IEP_CMP1_REG0          0x80
#define PRUSS_IEP_CMP1_REG1          0x84
#define PRUSS_IEP_SYNC_CTRL_REG      0x180
#define PRUSS_IEP_SYNC0_STAT_REG     0x188
#define PRUSS_IEP_SYNC_PWIDTH_REG    0x190
#define PRUSS_IEP_SYNC0_PERIOD_REG   0x194
#define PRUSS_IEP_SYNC_START_REG     0x19c

#define PRUSS_IEP_CMP_INC_MASK       0xfff00
#define PRUSS_IEP_CMP_INC_SHIFT      8

#define PRUSS_IEP_DEFAULT_INC        5
#define PRUSS_IEP_DEFAULT_CMP_INC    5
#define PRUSS_IEP_CLOCK_RATE         200000000

#define IEP_TC_DEFAULT_SHIFT         28
#define IEP_TC_DEFAULT_MULT          (5 << IEP_TC_DEFAULT_SHIFT)

#define IEP_GLOBAL_CFG_REG_MASK      0xfffff
#define IEP_GLOBAL_CFG_REG_VAL       0x00111

/* 10 ms width */
#define IEP_DEFAULT_PPS_WIDTH        (PRUSS_IEP_CLOCK_RATE / 100)

#define CTRL_CORE_PAD_VOUT1_D7       0x4a0035f8

/* 1ms pulse sync interval */
#define PULSE_SYNC_INTERVAL          1000000
#define TIMESYNC_SECONDS_COUNT_SIZE  6
#define PTP_TWO_STEP_ENABLE          1
#define TIMESYNC_ENABLE              1

struct iep {
	struct device *dev;
	void __iomem *sram;
	void __iomem *iep_reg;
	u32 __iomem  *pr2_sync0_mux;
	struct ptp_clock_info info;
	struct ptp_clock *ptp_clock;
	int phc_index;
	int ptp_tx_enable;
	int ptp_rx_enable;
	spinlock_t ptp_lock; /* serialize iep access */
	u32 cc_mult; /* for the nominal frequency */
	struct cyclecounter cc;
	struct timecounter tc;
	unsigned long ov_check_period;
	unsigned long ov_check_period_slow;
	u64 cmp1_last;
	int pps_enable;
	int pps_report_next_op;
	enum {
		DISABLE_SYNC0,
		ENABLE_SYNC0,
	} pps_ops[4];
};

void iep_reset_timestamp(struct iep *iep, u16 ts_ofs);
int iep_rx_timestamp(struct iep *iep, u16 ts_ofs, struct sk_buff *skb);
int iep_tx_timestamp(struct iep *iep, u16 ts_ofs, struct sk_buff *skb);
int iep_register(struct iep *iep);
void iep_unregister(struct iep *iep);
struct iep *iep_create(struct device *dev, void __iomem *sram,
		       void __iomem *iep_reg);
void iep_release(struct iep *iep);
#endif
