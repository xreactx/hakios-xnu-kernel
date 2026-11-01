/*
 * LinuxKPI's, with the one scoped guard it lacks that ath10k uses: guard(spinlock_bh) (wmi-tlv.c).
 */
#ifndef _LX_LINUX_SPINLOCK_H_
#define _LX_LINUX_SPINLOCK_H_

#include_next <linux/spinlock.h>

DEFINE_LOCK_GUARD_1(spinlock_bh,
    spinlock_t,
    spin_lock_bh(_T->lock),
    spin_unlock_bh(_T->lock))

#endif
