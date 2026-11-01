/* LinuxKPI's, with the device-managed memremap() ath10k's snoc.c maps the firmware's memory (MSA) with. */
#ifndef _LX_LINUX_IO_H_
#define _LX_LINUX_IO_H_
#include_next <linux/io.h>
#define devm_memremap(dev, offset, size, flags) memremap((offset), (size), (flags))
#endif
