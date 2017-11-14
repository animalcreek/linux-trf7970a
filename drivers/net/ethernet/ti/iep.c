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

static inline u64 iep_get_cmp(struct iep *iep)
{
	u64 v;

	memcpy_fromio(&v, iep->iep_reg + PRUSS_IEP_CMP1_REG0, sizeof(v));
	return v;
}

static inline void iep_set_cmp(struct iep *iep, u64 v)
{
	memcpy_toio(iep->iep_reg + PRUSS_IEP_CMP1_REG0, &v, sizeof(v));
}

static inline cycle_t iep_get_count(struct iep *iep)
{
	u64 v;

	memcpy_fromio(&v, iep->iep_reg + PRUSS_IEP_COUNT_REG0, 8);
	return v;
}

static cycle_t iep_cc_read(const struct cyclecounter *cc)
{
	struct iep *iep = container_of(cc, struct iep, cc);

	return iep_get_count(iep);
}

/* Implementation is good for 1 sec or less */
static u64 iep_ns2cyc(struct iep *iep, u64 nsec)
{
	u64 dividend, cycles;

	WARN_ON(nsec > 1000000000ULL);

	dividend = nsec << iep->cc.shift;
	cycles = div_u64(dividend, iep->cc.mult);
	return cycles;
}

static int iep_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
	struct iep *iep = container_of(ptp, struct iep, info);
	u64 adj;
	u32 diff, mult, v;
	int neg_adj = 0;
	unsigned long flags;
	struct timespec64 ts;
	u64 ns_to_sec, cyc_to_sec;

	if (ppb < 0) {
		neg_adj = 1;
		ppb = -ppb;
	}
	mult = iep->cc_mult;
	adj = mult;
	adj *= ppb;
	diff = div_u64(adj, 1000000000ULL);

	spin_lock_irqsave(&iep->ptp_lock, flags);

	ts = ns_to_timespec64(timecounter_read(&iep->tc));

	iep->cc.mult = neg_adj ? mult - diff : mult + diff;

	if (iep->pps_enable) {
		ns_to_sec = NSEC_PER_SEC - ts.tv_nsec;
		cyc_to_sec = iep_ns2cyc(iep, ns_to_sec);

		/* +++TODO: fine tune the randomly fixed 10 ticks */
		/* if it's too late to update CMP1, skip it */
		if (cyc_to_sec >= 10) {
			/* if the previous HIT is not reported yet,
			 * skip update
			 */
			v = iep_read_reg(iep, PRUSS_IEP_CMP_STAT_REG);
			if (!(v & IEP_CMP1_HIT)) {
				iep->cmp1_last = iep->tc.cycle_last +
						 cyc_to_sec;

				/* update CMP reg */
				iep_set_cmp(iep, iep->cmp1_last);
			}
		}
	}

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

/* PPS */
static inline void iep_pps_stop(struct iep *iep)
{
	u32 v;

	/* disable sync0 */
	iep_write_reg(iep, PRUSS_IEP_SYNC_CTRL_REG, 0);

	/* clear sync0 status */
	iep_write_reg(iep, PRUSS_IEP_SYNC0_STAT_REG, 0);

	/* disable CMP1 */
	v = iep_read_reg(iep, PRUSS_IEP_CMP_CFG_REG);
	v &= ~IEP_CMP1_EN;
	iep_write_reg(iep, PRUSS_IEP_CMP_CFG_REG, v);

	/* clear CMP1 status */
	iep_write_reg(iep, PRUSS_IEP_CMP_STAT_REG, IEP_CMP1_HIT);
}

static inline void iep_pps_start(struct iep *iep)
{
	u32 v;

	/* enable CMP1 */
	v = iep_read_reg(iep, PRUSS_IEP_CMP_CFG_REG);
	v |= IEP_CMP1_EN;
	iep_write_reg(iep, PRUSS_IEP_CMP_CFG_REG, v);

	/* enable sync0 1-shot mode */
	iep_write_reg(iep, PRUSS_IEP_SYNC_CTRL_REG, IEP_SYNC0_EN | IEP_SYNC_EN);
}

static int iep_pps_enable(struct iep *iep, int on)
{
	unsigned long flags;
	struct timespec64 ts;
	u64 cyc_to_sec, ns_to_sec, cyc_per_sec, cyc_last2;

	on = (on ? 1 : 0);

	if (iep->pps_enable == on)
		return 0;

	iep->pps_enable = on;

	/* will stop after up coming pulse */
	if (!on)
		return 0;

	/* get current time and counter value */
	iep_gettime(&iep->info, &ts);

	spin_lock_irqsave(&iep->ptp_lock, flags);

	/* current iep ticks per sec */
	cyc_per_sec = iep_ns2cyc(iep, NSEC_PER_SEC);

	/* align cmp count to next sec boundary */
	ns_to_sec = NSEC_PER_SEC - ts.tv_nsec;
	cyc_to_sec = iep_ns2cyc(iep, ns_to_sec);
	iep->cmp1_last = iep->tc.cycle_last + cyc_to_sec;

	/* how many ticks has elapsed since last time */
	cyc_last2 = (u64)iep_get_count(iep);

	/* if it is too close to sec boundary, start 1 sec later */
	/* +++TODO: tune this randomly fixed 10 ticks allowance */
	if (iep->cmp1_last <= cyc_last2 + 10)
		iep->cmp1_last = iep->cmp1_last + cyc_per_sec;

	iep_set_cmp(iep, iep->cmp1_last);
	iep_pps_start(iep);

	spin_unlock_irqrestore(&iep->ptp_lock, flags);
	return 0;
}

static int iep_pps_init(struct iep *iep)
{
	u32 v;

	iep_pps_stop(iep);

	/* Following are one time configurations */

	/* config pulse width to 10 ms, ie 2000000 cycles */
	iep_write_reg(iep, PRUSS_IEP_SYNC_PWIDTH_REG, IEP_DEFAULT_PPS_WIDTH);

	/* set SYNC start to 0 */
	iep_write_reg(iep, PRUSS_IEP_SYNC_START_REG, 0);

	/* makes sure SYNC0 period is 0 */
	iep_write_reg(iep, PRUSS_IEP_SYNC0_PERIOD_REG, 0);

	/* +++TODO: pad config
	 * mux CTRL_CORE_PAD_VOUT1_D7(0x4A00_35F8) (TRM p4727)
	 *	VOUT1_D7_MUXMODE[0:3] = 0xA: pr2_edc_sync0_out
	 *		Expansion Connector:17 (schematic p33)
	 */
	v = readl_relaxed(iep->pr2_sync0_mux);
	v = (v & ~0xf) | 0xa;
	writel_relaxed(v, iep->pr2_sync0_mux);

	iep->pps_report_next_op = -1;
	iep->pps_enable = -1;

	return 0;
}

static int iep_enable(struct ptp_clock_info *ptp,
		      struct ptp_clock_request *rq, int on)
{
	struct iep *iep = container_of(ptp, struct iep, info);

	switch (rq->type) {
	case PTP_CLK_REQ_PPS:
		return iep_pps_enable(iep, on);
	default:
		break;
	}
	return -EOPNOTSUPP;
}

/* Returns whether a pps event is reported */
static bool iep_pps_report(struct iep *iep)
{
	struct ptp_clock_event pevent;
	u64 pps_cmp1, ns;
	u32 v, reported = 0;

	v = iep_read_reg(iep, PRUSS_IEP_CMP_STAT_REG);
	if (v & IEP_CMP1_HIT) {
		/* write 1 to clear CMP1 status */
		iep_write_reg(iep, PRUSS_IEP_CMP_STAT_REG, v);

		/* a pulse has occurred */
		pps_cmp1 = iep_get_cmp(iep);
		ns = timecounter_cyc2time(&iep->tc, pps_cmp1);
		pevent.type = PTP_CLOCK_PPSUSR;
		pevent.pps_times.ts_real = ns_to_timespec64(ns);
		ptp_clock_event(iep->ptp_clock, &pevent);

		++reported;

		/* need to keep SYNC0_EN & SYNC_EN for the PWIDTH time for
		 * otherwise ongoing pulse will be terminated. Remember
		 * we need to do this in the next check. If the check
		 * happens every 50ms, the latest to disable the sync0
		 * is 100ms after it happened, ie. a check happens right
		 * before the sync0, then is found out in the next
		 * check and is disabled in the check after the next.
		 */
		iep->pps_ops[++iep->pps_report_next_op] = DISABLE_SYNC0;
	}

	return reported;
}

static inline void iep_do_pps_report_post_ops(struct iep *iep)
{
	int i;

	for (i = 0; i <= iep->pps_report_next_op; i++) {
		switch (iep->pps_ops[i]) {
		case DISABLE_SYNC0:
			/* disable sync0 */
			iep_write_reg(iep, PRUSS_IEP_SYNC_CTRL_REG, 0);
			/* clear sync0 status */
			iep_write_reg(iep, PRUSS_IEP_SYNC0_STAT_REG, 0);
			break;

		case ENABLE_SYNC0:
			/* enable sync0 1-shot mode */
			iep_write_reg(iep, PRUSS_IEP_SYNC_CTRL_REG,
				      IEP_SYNC0_EN | IEP_SYNC_EN);
			break;
		}
	}

	iep->pps_report_next_op = -1;
}

static long iep_overflow_check(struct ptp_clock_info *ptp)
{
	struct iep *iep = container_of(ptp, struct iep, info);
	unsigned long delay = iep->ov_check_period;
	struct timespec64 ts;
	unsigned long flags;
	bool reported = false;
	u64 ns_to_sec, cyc_to_sec;

	spin_lock_irqsave(&iep->ptp_lock, flags);
	ts = ns_to_timespec64(timecounter_read(&iep->tc));

	if (iep->pps_enable >= 0) {
		if (!iep->pps_enable) {
			iep_pps_stop(iep);
			iep->pps_enable = -1;
		} else {
			if (iep->pps_report_next_op >= 0)
				/* perform ops left behind in last
				 *  overflow check
				 */
				iep_do_pps_report_post_ops(iep);

			reported = iep_pps_report(iep);
		}
	}

	if (!reported)
		goto done;

	/* load the next pulse */

	/* Do we need to get the updated time and counter again?
	 * Probably not. Just use the last one. ns to sec boundary
	 * will be larger to compensate.
	 */

	/* Align cmp count to next sec boundary. If overflow check is
	 * done every 50ms, the ns_to_sec  will be at least 950ms,
	 * ie. a check just happened right before the sync and is found
	 * out in the next check.
	 */
	ns_to_sec = NSEC_PER_SEC - ts.tv_nsec;
	cyc_to_sec = iep_ns2cyc(iep, ns_to_sec);
	iep->cmp1_last = iep->tc.cycle_last + cyc_to_sec;

	iep_set_cmp(iep, iep->cmp1_last);

	if (iep->pps_report_next_op >= 0)
		iep->pps_ops[++iep->pps_report_next_op] = ENABLE_SYNC0;

done:
	spin_unlock_irqrestore(&iep->ptp_lock, flags);

	pr_debug("iep overflow check at %lld.%09lu\n", ts.tv_sec, ts.tv_nsec);
	return (long)delay;
}

static struct ptp_clock_info iep_info = {
	.owner		= THIS_MODULE,
	.name		= "PRUSS timer",
	.max_adj	= 1000000,
	.n_ext_ts	= 0,
	.n_pins		= 0,
	.pps		= 1,
	.adjfreq	= iep_adjfreq,
	.adjtime	= iep_adjtime,
	.gettime64	= iep_gettime,
	.settime64	= iep_settime,
	.enable		= iep_enable,
	.do_aux_work	= iep_overflow_check,
};

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
	writel(IEP_DEFAULT_PPS_WIDTH, sram + TIMESYNC_SYNC0_WIDTH_OFFSET);

	/* Write CMP1 period for sync signal generation in PTP
	 * memory in shared RAM
	 */
	temp64 = PULSE_SYNC_INTERVAL;
	memcpy_toio(sram + TIMESYNC_CMP1_CMP_OFFSET, &temp64, sizeof(temp64));

	/* Write Sync0 period for sync signal generation in PTP
	 * memory in shared RAM
	 */
	writel(PULSE_SYNC_INTERVAL, sram + TIMESYNC_CMP1_PERIOD_OFFSET);

	/* Configures domainNumber list. Firmware supports 2 domains */
	writeb(0, sram + TIMESYNC_DOMAIN_NUMBER_LIST);
	writeb(0, sram + TIMESYNC_DOMAIN_NUMBER_LIST + 1);

	/* Configure 1-step/2-step */
	writeb(PTP_TWO_STEP_ENABLE, sram + DISABLE_SWITCH_SYNC_RELAY_OFFSET);

	/* Configures the setting to Link local frame without HSR tag */
	writeb(0, sram + LINK_LOCAL_FRAME_HAS_HSR_TAG);
	return 0;
}

static int iep_config(struct iep *iep)
{
	u32 val;

	/* Program compare 1 event value */
	iep_write_reg(iep, PRUSS_IEP_CMP1_REG0, PULSE_SYNC_INTERVAL);

	/* Program Sync 0 Width. The value in register is
	 * multiplied by 1 by HW
	 */
	iep_write_reg(iep, PRUSS_IEP_SYNC_PWIDTH_REG, IEP_DEFAULT_PPS_WIDTH);

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

	iep->pr2_sync0_mux = (u32 __iomem *)ioremap(CTRL_CORE_PAD_VOUT1_D7,
						    sizeof(u32));

	iep_pps_init(iep);
	iep_start(iep);
	iep->phc_index = ptp_clock_index(iep->ptp_clock);

	ptp_schedule_worker(iep->ptp_clock, iep->ov_check_period);
	return 0;
}

void iep_unregister(struct iep *iep)
{
	if (WARN_ON(!iep->ptp_clock))
		return;

	iep_pps_stop(iep);
	iep_stop(iep);
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
	iep->ov_check_period = msecs_to_jiffies(50);
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
