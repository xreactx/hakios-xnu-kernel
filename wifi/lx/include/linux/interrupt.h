/* LinuxKPI's, with the one flag ath10k's snoc.c asks for: interrupts that stay masked until enabled. */
#ifndef _LX_LINUX_INTERRUPT_H_
#define _LX_LINUX_INTERRUPT_H_
#include_next <linux/interrupt.h>
#ifndef IRQF_NO_AUTOEN
#define IRQF_NO_AUTOEN          0x00080000
#endif
#endif
