/*
 * Xilinx PTP : Linux driver for 1588 timer
 *
 * Author: Xilinx, Inc.
 *
 * 2014 (c) Xilinx, Inc. This file is licensed uner the terms of the GNU
 * General Public License version 2. This program is licensed "as is"
 * without any warranty of any kind, whether express or implied.
 *
 * This is a driver for xilinx processor sub-system (ps) ethernet device.
 * This driver is mainly used in Linux 2.6.30 and above
 *
 */
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/platform_device.h>
#include <linux/of_irq.h>

#include "ptp_xlnx_1588.h"
#define DRIVER_NAME		"xlnx_ptp_timer"

/* Register offset definitions */

#define XTIMER1588_RTC_OFFSET_NS         0x00000 /* RTC Nanoseconds Field Offset Register */
#define XTIMER1588_RTC_OFFSET_SEC_L      0x00008 /* RTC Seconds Field Offset Register - Low */
#define XTIMER1588_RTC_OFFSET_SEC_H      0x0000C /* RTC Seconds Field Offset Register - High */
#define XTIMER1588_RTC_INCREMENT  0x00010 /* RTC Increment */
#define XTIMER1588_CURRENT_RTC_NS         0x00014 /* Current TOD Nanoseconds - RO */
#define XTIMER1588_CURRENT_RTC_SEC_L      0x00018 /* Current TOD Seconds -Low RO  */
#define XTIMER1588_CURRENT_RTC_SEC_H      0x0001C /* Current TOD Seconds -High RO */
#define XTIMER1588_INTERRUPT      0x00020 /* Write to Bit 0 to clear the interrupt */
#define XTIMER1588_8KPULSE        0x00024 /* 8kHz Pulse Offset Register */
#define XTIMER1588_CF_L           0x0002C /* Correction Field - Low */
#define XTIMER1588_CF_H           0x00030 /* Correction Field - Low */

#define XTIMER1588_RTC_MASK  ((1 << 26) - 1)
#define XTIMER1588_INT_SHIFT 0
#define NANOSECOND_BITS 20
#define NANOSECOND_MASK ((1 << NANOSECOND_BITS) - 1)
#define SECOND_MASK ((1 << (32 - NANOSECOND_BITS)) - 1)
#define XTIMER1588_RTC_INCREMENT_SHIFT 20
#define PULSESIN1PPS 128

#define CLK_PERIOD	4 /* In nano seconds user needs to modify this based on the design */
#define FRACTIONOF_ONENSEC	1048576

struct xlnx_ptp_timer {
  struct                 device *dev;
  void __iomem          *baseaddr;
	struct ptp_clock      *ptp_clock;
	struct ptp_clock_info  ptp_clock_info;
	spinlock_t             reg_lock;
  int                    irq;
  int                    pps_enable;
  int                    countpulse;
};

void __iomem *timer_baseaddr;
extern int syncadj_time;
extern int followupadj_freq;

static void xlnx_tod_read(struct xlnx_ptp_timer *timer, struct timespec64 *ts)
{
    u32 sec, nsec;
	  nsec = in_be32(timer->baseaddr + XTIMER1588_CURRENT_RTC_NS);
	  sec = in_be32(timer->baseaddr + XTIMER1588_CURRENT_RTC_SEC_L);

	  ts->tv_sec = sec;
	  ts->tv_nsec = nsec;
}

static void xlnx_rtc_offset_write(struct xlnx_ptp_timer *timer,
                           const struct timespec64 *ts)
{
   out_be32((timer->baseaddr + XTIMER1588_RTC_OFFSET_SEC_H), 0 );
   out_be32((timer->baseaddr + XTIMER1588_RTC_OFFSET_SEC_L), (ts->tv_sec) );
   out_be32((timer->baseaddr + XTIMER1588_RTC_OFFSET_NS), ts->tv_nsec );
}

static void xlnx_rtc_offset_read(struct xlnx_ptp_timer *timer,
                          struct timespec64 *ts)
{
  ts->tv_sec = in_be32(timer->baseaddr + XTIMER1588_RTC_OFFSET_SEC_L);
  ts->tv_nsec = in_be32(timer->baseaddr + XTIMER1588_RTC_OFFSET_NS);
}

/*
 * PTP clock operations
 */

static int xlnx_ptp_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
  struct xlnx_ptp_timer *timer = container_of(ptp, struct xlnx_ptp_timer,
                                              ptp_clock_info);

	int neg_adj = 0;
	u64 freq;
	u32 diff, incval;

	incval = CLK_PERIOD * FRACTIONOF_ONENSEC;

	if (ppb < 0) {
	  neg_adj = 1;
	  ppb = -ppb;
	}

	freq = incval;
	freq *= ppb;
	diff = div_u64(freq, 1000000000ULL);

	incval = neg_adj ? (incval - diff) : (incval + diff);
	out_be32((timer->baseaddr + XTIMER1588_RTC_INCREMENT), incval);

	return 0;
}

static int xlnx_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	unsigned long flags;
	struct xlnx_ptp_timer *timer = container_of(ptp, struct xlnx_ptp_timer,
                                              ptp_clock_info);
	struct timespec64 offset, then = ns_to_timespec64(delta), now;
	u32 sign = 0, adj = CLK_PERIOD * FRACTIONOF_ONENSEC, adj1= 0;

	if (delta < 0) {
		delta = -delta;
		sign = 1;
	}

	spin_lock_irqsave(&timer->reg_lock, flags);
	if (delta > 0x3FFFFFFF) {
		xlnx_rtc_offset_read(timer, &offset);

		offset = timespec64_add(offset, then);
		xlnx_rtc_offset_write(timer, (const struct timespec64 *)&offset);
	} else {
		adj1 = delta;
		adj = sign ? (adj - adj1) : (adj + adj1);
		out_be32((timer->baseaddr + XTIMER1588_RTC_INCREMENT), adj);
	}
	spin_unlock_irqrestore(&timer->reg_lock, flags);

	return 0;
}

static int xlnx_ptp_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	unsigned long flags;
	struct xlnx_ptp_timer *timer = container_of(ptp, struct xlnx_ptp_timer,
                                              ptp_clock_info);

	spin_lock_irqsave(&timer->reg_lock, flags);

	xlnx_tod_read(timer, ts);

	spin_unlock_irqrestore(&timer->reg_lock, flags);
	return 0;
}

/**
 * xlnx_ptp_settime - Set the current time on the hardware clock
 * @ptp: ptp clock structure
 * @ts: timespec64 containing the new time for the cycle counter
 *
 * The seconds register is written first, then the nanosecond
 * The hardware loads the entire new value when a nanosecond register
 * is written
 **/
static int xlnx_ptp_settime(struct ptp_clock_info *ptp,
		                        const struct timespec64 *ts)
{
	struct xlnx_ptp_timer *timer = container_of(ptp, struct xlnx_ptp_timer,
                                              ptp_clock_info);
	struct timespec64 delta,tod;
	struct timespec64 offset;
	unsigned long flags;

	spin_lock_irqsave(&timer->reg_lock, flags);

	/* First zero the offset */
	offset.tv_sec = 0;
	offset.tv_nsec = 0;
	xlnx_rtc_offset_write(timer, &offset);

	/* Get the current timer value */
	xlnx_tod_read(timer, &tod);

	/* Subtract the current reported time from our desired time */
	delta = timespec64_sub((struct timespec64)*ts, tod);

	/* Don't write a negative offset */
	if (delta.tv_sec <= 0) {
		delta.tv_sec = 0;
		if (delta.tv_nsec < 0) {
			delta.tv_nsec = 0;
		}
	}

	xlnx_rtc_offset_write(timer, &delta);
	spin_unlock_irqrestore(&timer->reg_lock, flags);
	return 0;
}

static int xlnx_ptp_enable(struct ptp_clock_info *ptp,
			  struct ptp_clock_request *rq, int on)
{
	struct xlnx_ptp_timer *timer = container_of(ptp, struct xlnx_ptp_timer,
                                              ptp_clock_info);

	  switch (rq->type) {
		case PTP_ENABLE_PPS:
			timer->pps_enable = 1;
			return 0;
		default:
			break;
	}

	return -EOPNOTSUPP;
}

static struct ptp_clock_info xlnx_ptp_clock_info = {
	.owner    = THIS_MODULE,
	.name     = "Xilinx Timer",
	.max_adj  = 999999999,
	.n_ext_ts	= 0,
	.pps      = 1,
	.adjfreq  = xlnx_ptp_adjfreq,
	.adjtime  = xlnx_ptp_adjtime,
	.gettime64  = xlnx_ptp_gettime,
	.settime64 = xlnx_ptp_settime,
	.enable   = xlnx_ptp_enable,
};

/* module operations */

/**
 * xlnx_ptp_timer_isr - Interrupt Service Routine
 * @irq:               IRQ number
 * @priv:              pointer to the timer structure
 *
 * Returns: IRQ_HANDLED for all cases
 *
 * Handles the timer interrupt. The timer interrupt fires 128 times per
 * secound. When our count reaches 128 emit a PTP_CLOCK_PPS event
 */
static irqreturn_t xlnx_ptp_timer_isr(int irq, void *priv)
{
	struct xlnx_ptp_timer *timer = priv;
	struct ptp_clock_event event;

	event.type = PTP_CLOCK_PPS;
	++timer->countpulse;
	if (timer->countpulse >= PULSESIN1PPS) {
		timer->countpulse = 0;
		if ((timer->ptp_clock) && (timer->pps_enable)) {
			ptp_clock_event(timer->ptp_clock, &event);
		}
	}
out:
	out_be32((timer->baseaddr + XTIMER1588_INTERRUPT),  (1 << XTIMER1588_INT_SHIFT) );
	return IRQ_HANDLED;
}

static int xlnx_ptp_timer_remove(struct platform_device *pdev)
{
	struct xlnx_ptp_timer *timer = platform_get_drvdata(pdev);
	free_irq(timer->irq, timer->dev);
	if (timer->ptp_clock) {
		ptp_clock_unregister(timer->ptp_clock);
		timer->ptp_clock = NULL;
	}
	return 0;
}

static int xlnx_ptp_timer_request_irq(struct xlnx_ptp_timer *timer)
{
	int err;
	/* Enable interrupts */
	err = request_irq(timer->irq,
			  xlnx_ptp_timer_isr,
			  0,
			  DRIVER_NAME,
			  timer);
	if (err) {
		return err;
	}
	dev_info(timer->dev, "Acquired ptp_irq: 0x%x\n", timer->irq);
	return 0;
}

static int xlnx_ptp_timer_probe(struct platform_device *pdev)
{
	struct xlnx_ptp_timer *timer;
	struct resource *r_mem;
	int err = 0;
	u32 incval;
	struct timespec64 now, offset;

	timer = devm_kzalloc(&pdev->dev, sizeof(*timer), GFP_KERNEL);
	if (!timer) {
		dev_err(&pdev->dev, "could not allocated memory for private data\n");
		return -ENOMEM;
	}

	timer->dev = &pdev->dev;

	r_mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r_mem) {
		dev_err(&pdev->dev, "no IO resource defined\n");
		return -ENXIO;
  }

  timer->baseaddr = devm_ioremap_resource(&pdev->dev, r_mem);
  if (IS_ERR(timer->baseaddr)) {
	err = PTR_ERR(timer->baseaddr);
	return err;
  }
	timer_baseaddr = timer->baseaddr;

	timer->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (timer->irq <= 0) {
		dev_err(&pdev->dev, "could not determine Timer IRQ\n");
		err = -ENOMEM;
		return err;
	}

	spin_lock_init(&timer->reg_lock);

	timer->ptp_clock_info = xlnx_ptp_clock_info;

	timer->ptp_clock = ptp_clock_register(&timer->ptp_clock_info,
                                        &pdev->dev);

	if (IS_ERR(timer->ptp_clock)) {
		err = PTR_ERR(timer->ptp_clock);
		dev_err(&pdev->dev, "Failed to register ptp clock\n");
		goto out;
	}

	err = xlnx_ptp_timer_request_irq(timer);
	if (err)
		goto out;

	incval = CLK_PERIOD * FRACTIONOF_ONENSEC;
	printk("inval is %x\n\r", incval);
	out_be32((timer->baseaddr + XTIMER1588_RTC_INCREMENT), incval);

	platform_set_drvdata(pdev, timer);

	return 0;
free_irqs:
	free_irq(timer->irq, &pdev->dev);
out:
	timer->ptp_clock = NULL;
	return err;
}

EXPORT_SYMBOL(timer_baseaddr);

static struct of_device_id timer_1588_of_match[] = {
	{ .compatible = "xlnx,timer-1588-2.0", },
		{ /* end of table */ }
};
MODULE_DEVICE_TABLE(of, timer_1588_of_match);

static struct platform_driver xlnx_ptp_timer_driver = {
	.probe                = xlnx_ptp_timer_probe,
	.remove               = xlnx_ptp_timer_remove,
	.driver               = {
           .name = DRIVER_NAME,
           .owner = THIS_MODULE,
          .of_match_table = timer_1588_of_match,
  },
};

module_platform_driver(xlnx_ptp_timer_driver);

MODULE_AUTHOR("Xilinx, Inc.");
MODULE_DESCRIPTION("PTP Timer driver");
MODULE_LICENSE("GPL v2");
