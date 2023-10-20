#ifndef PTP_XILINX_1588_H
#define PTP_XILINX_1588_H

/* Read/Write access to the registers */
#ifndef out_be32
#ifdef CONFIG_ARCH_ZYNQMP
#define in_be32(offset)		__raw_readl(offset)
#define out_be32(offset, val)	__raw_writel(val, offset)
#endif
#endif

#endif /* PTP_XILINX_1588_H */
