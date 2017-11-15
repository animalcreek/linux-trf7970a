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
#include "icss_time_sync.h"
#include "iep.h"

/* 25% duty cycle */
#define PULSE_SYNC_WIDTH    (PULSE_SYNC_INTERVAL / PTP_SYNC0_PERIOD_DIVIDER)
/* 1ms pulse sync interval */
#define PULSE_SYNC_INTERVAL          1000000
#define TIMESYNC_SECONDS_COUNT_SIZE  6
#define PTP_TWO_STEP_ENABLE          1
#define TIMESYNC_ENABLE              1

static inline u32 iep_read_reg(struct iep *iep, unsigned int reg)
{
	return readl_relaxed(iep->iep_reg + reg);
}

static inline void iep_write_reg(struct iep *iep, unsigned int reg, u32 val)
{
	writel_relaxed(val, iep->iep_reg + reg);
}

static inline
void iep_set_reg(struct iep *iep, unsigned int reg, u32 mask, u32 set)
{
	u32 val;

	val = iep_read_reg(iep, reg);
	val &= ~mask;
	val |= (set & mask);
	iep_write_reg(iep, reg, val);
}

static cycle_t iep_cc_read(const struct cyclecounter *cc)
{
	struct iep *iep = container_of(cc, struct iep, cc);
	u64 val = 0;

	memcpy_fromio(&val, iep->iep_reg + PRUSS_IEP_COUNT_REG0, 8);

	return val;
}

static int iep_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
	struct iep *iep = container_of(ptp, struct iep, info);
	u64 adj;
	u32 diff, mult;
	int neg_adj = 0;
	unsigned long flags;

	if (ppb < 0) {
		neg_adj = 1;
		ppb = -ppb;
	}
	mult = iep->cc_mult;
	adj = mult;
	adj *= ppb;
	diff = div_u64(adj, 1000000000ULL);

	spin_lock_irqsave(&iep->ptp_lock, flags);

	timecounter_read(&iep->tc);

	iep->cc.mult = neg_adj ? mult - diff : mult + diff;

	spin_unlock_irqrestore(&iep->ptp_lock, flags);

	return 0;
}

static int iep_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	struct iep *iep = container_of(ptp, struct iep, info);
	unsigned long flags;

	spin_lock_irqsave(&iep->ptp_lock, flags);
	timecounter_adjtime(&iep->tc, delta);
	spin_unlock_irqrestore(&iep->ptp_lock, flags);

	return 0;
}

static int iep_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	struct iep *iep = container_of(ptp, struct iep, info);
	unsigned long flags;
	u64 ns;

	spin_lock_irqsave(&iep->ptp_lock, flags);
	ns = timecounter_read(&iep->tc);
	spin_unlock_irqrestore(&iep->ptp_lock, flags);

	*ts = ns_to_timespec64(ns);

	return 0;
}

static int iep_settime(struct ptp_clock_info *ptp, const struct timespec64 *ts)
{
	struct iep *iep = container_of(ptp, struct iep, info);
	unsigned long flags;
	u64 ns;

	ns = timespec64_to_ns(ts);

	spin_lock_irqsave(&iep->ptp_lock, flags);
	timecounter_init(&iep->tc, &iep->cc, ns);
	spin_unlock_irqrestore(&iep->ptp_lock, flags);

	return 0;
}

static int iep_enable(struct ptp_clock_info *ptp,
		      struct ptp_clock_request *rq, int on)
{
	return -EOPNOTSUPP;
}

static struct ptp_clock_info iep_info = {
	.owner		= THIS_MODULE,
	.name		= "PRUSS timer",
	.max_adj	= 1000000,
	.n_ext_ts	= 0,
	.n_pins		= 0,
	.pps		= 0,
	.adjfreq	= iep_adjfreq,
	.adjtime	= iep_adjtime,
	.gettime64	= iep_gettime,
	.settime64	= iep_settime,
	.enable		= iep_enable,
};

static void iep_overflow_check(struct work_struct *work)
{
	struct iep *iep = container_of(work, struct iep, overflow_work.work);
	struct timespec64 ts;
	unsigned long flags;

	spin_lock_irqsave(&iep->ptp_lock, flags);
	ts = ns_to_timespec64(timecounter_read(&iep->tc));
	spin_unlock_irqrestore(&iep->ptp_lock, flags);
	pr_debug("iep overflow check at %lld.%09lu\n", ts.tv_sec, ts.tv_nsec);
	schedule_delayed_work(&iep->overflow_work, iep->ov_check_period);
}

void iep_reset_timestamp(struct iep *iep, u16 ts_ofs)
{
	memset_io(iep->sram + ts_ofs, 0, sizeof(u64));
}

int iep_rx_timestamp(struct iep *iep, u16 ts_ofs, struct sk_buff *skb)
{
	struct skb_shared_hwtstamps *ssh;
	void __iomem *sram = iep->sram;
	u64 ns, cycles;

	/* get timestamp */
	memcpy_fromio(&cycles, sram + ts_ofs, sizeof(cycles));
	memset_io(sram + ts_ofs, 0, sizeof(cycles));

	if (!cycles)
		return -ENOENT;

	ns = timecounter_cyc2time(&iep->tc, cycles);

	ssh = skb_hwtstamps(skb);
	memset(ssh, 0, sizeof(*ssh));
	ssh->hwtstamp = ns_to_ktime(ns);

	return 0;
}

int iep_tx_timestamp(struct iep *iep, u16 ts_ofs, struct sk_buff *skb)
{
	struct skb_shared_hwtstamps ssh;
	void __iomem *sram = iep->sram;
	u64 ns, cycles;

	/* get timestamp */
	memcpy_fromio(&cycles, sram + ts_ofs, sizeof(cycles));
	memset_io(sram + ts_ofs, 0, sizeof(cycles));

	if (!cycles)
		return -ENOENT;

	ns = timecounter_cyc2time(&iep->tc, cycles);

	/* pass timestamp to upper */
	memset(&ssh, 0, sizeof(ssh));
	ssh.hwtstamp = ns_to_ktime(ns);
	skb_tstamp_tx(skb, &ssh);

	return 0;
}

/* +++TODO: why rtos use memcpy for writing 4B? */
static int iep_dram_init(struct iep *iep)
{
	void __iomem *sram = iep->sram;
	u64 temp64;

	writew(0, sram + MII_RX_CORRECTION_OFFSET);
	writew(0, sram + MII_TX_CORRECTION_OFFSET);

	/*Set seconds value to 0*/
	memset_io(sram + TIMESYNC_SECONDS_COUNT_OFFSET, 0,
		  TIMESYNC_SECONDS_COUNT_SIZE);

	/* Initialize RCF to 1 (Linux N/A) */
	writel(1 * 1024, sram + TIMESYNC_TC_RCF_OFFSET);

	/* This flag will be set and cleared by firmware */
	/* Write Sync0 period for sync signal generation in PTP
	 * memory in shared RAM
	 */
	writel(PULSE_SYNC_WIDTH, sram + TIMESYNC_SYNC0_WIDTH_OFFSET);

	/* Write CMP1 period for sync signal generation in PTP
	 * memory in shared RAM
	 */
	temp64 = PULSE_SYNC_INTERVAL;
	memcpy_fromio(sram + TIMESYNC_CMP1_CMP_OFFSET, &temp64, sizeof(temp64));

	/* Write Sync0 period for sync signal generation in PTP
	 * memory in shared RAM
	 */
	writel(PULSE_SYNC_INTERVAL, sram + TIMESYNC_CMP1_PERIOD_OFFSET);

	/* Configures domainNumber list. Firmware supports 2 domains */
	writeb(0, sram + TIMESYNC_DOMAIN_NUMBER_LIST);
	writeb(0, sram + TIMESYNC_DOMAIN_NUMBER_LIST + 1);

	/* Configure 1-step/2-step (Linux: N/A) */
	writeb(PTP_TWO_STEP_ENABLE, sram + DISABLE_SWITCH_SYNC_RELAY_OFFSET);

	/* Configures the setting to Link local frame without HSR tag */
	writeb(0, sram + LINK_LOCAL_FRAME_HAS_HSR_TAG);
	return 0;
}

static int iep_init_ecap(struct iep *iep)
{
	/* +++TODO: enable dra7.dtsi/ecap0{} */
	return 0;
}

static int iep_config(struct iep *iep)
{
	u32 val;

	/* Program compare 1 event value */
	iep_write_reg(iep, PRUSS_IEP_CMP1_REG0, PULSE_SYNC_INTERVAL);

	/* Program Sync 0 Width. The value in register is
	 * multiplied by 5 by HW
	 */
	iep_write_reg(iep, PRUSS_IEP_SYNC_PWIDTH_REG,
		      PULSE_SYNC_WIDTH * 5);

	/* Program Sync 0 Start cycles */
	iep_write_reg(iep, PRUSS_IEP_SYNC_START_REG, 0);

	/* Enable Sync 0 in one-shot mode */
	iep_write_reg(iep, PRUSS_IEP_SYNC_CTRL_REG,
		      IEP_SYNC0_EN | IEP_SYNC_EN);

	/* Enable compare 1 */
	val = iep_read_reg(iep, PRUSS_IEP_CMP_CFG_REG);
	val |= IEP_CMP1_EN;

	/* Reset IEP count to 0 before enabling compare config regs
	 * This ensures that we don't hit CMP1 with a large value in IEP
	 */
	iep_write_reg(iep, PRUSS_IEP_COUNT_REG0, 0);
	iep_write_reg(iep, PRUSS_IEP_COUNT_REG1, 0);

	iep_write_reg(iep, PRUSS_IEP_CMP_CFG_REG, val);
	return 0;
}

static inline void iep_start(struct iep *iep)
{
	/* disable fw background task */
	writeb(0, iep->sram + TIMESYNC_CTRL_VAR_OFFSET);
	iep->ptp_tx_enable = TIMESYNC_ENABLE;
	iep->ptp_rx_enable = TIMESYNC_ENABLE;
}

static inline void iep_stop(struct iep *iep)
{
	iep->ptp_tx_enable = 0;
	iep->ptp_rx_enable = 0;
}

int iep_register(struct iep *iep)
{
	int err;

	iep_dram_init(iep);

	iep_init_ecap(iep);

	/*   TimeSync_edmaConfig +++TODO */

	iep_config(iep);

	iep_set_reg(iep, PRUSS_IEP_GLOBAL_CFG,
		    IEP_GLOBAL_CFG_REG_MASK, IEP_GLOBAL_CFG_REG_VAL);

	timecounter_init(&iep->tc, &iep->cc, ktime_to_ns(ktime_get_real()));

	iep->ptp_clock = ptp_clock_register(&iep->info, iep->dev);
	if (IS_ERR(iep->ptp_clock)) {
		err = PTR_ERR(iep->ptp_clock);
		iep->ptp_clock = NULL;
		return err;
	}

	iep_start(iep);

	iep->phc_index = ptp_clock_index(iep->ptp_clock);

	schedule_delayed_work(&iep->overflow_work, iep->ov_check_period);
	return 0;
}

void iep_unregister(struct iep *iep)
{
	iep_stop(iep);
	cancel_delayed_work_sync(&iep->overflow_work);
	ptp_clock_unregister(iep->ptp_clock);
	iep->ptp_clock = NULL;
}

struct iep *iep_create(struct device *dev, void __iomem *sram,
		       void __iomem *iep_reg)
{
	struct iep *iep;

	iep = devm_kzalloc(dev, sizeof(*iep), GFP_KERNEL);
	if (!iep)
		return ERR_PTR(-ENOMEM);

	iep->dev = dev;
	iep->sram = sram;
	iep->iep_reg = iep_reg;
	spin_lock_init(&iep->ptp_lock);
	INIT_DELAYED_WORK(&iep->overflow_work, iep_overflow_check);
	iep->ov_check_period = 5 * HZ;
	iep->ov_check_period_slow = iep->ov_check_period;

	iep->cc.shift = IEP_TC_DEFAULT_SHIFT;
	iep->cc.mult = IEP_TC_DEFAULT_MULT;
	iep->cc.read = iep_cc_read;
	iep->cc.mask = CLOCKSOURCE_MASK(64);
	iep->info = iep_info;

	/* save cc.mult original value as it can be modified
	 * by iep_adjfreq().
	 */
	iep->cc_mult = iep->cc.mult;

	return iep;
}
