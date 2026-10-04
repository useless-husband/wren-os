/* Block device interface (implemented by the virtio-blk driver). */
#ifndef KERNEL_BLK_H
#define KERNEL_BLK_H
#include "types.h"

struct platform;
struct buf;

int  blk_init(const struct platform *pf);  /* 0 if a virtio-blk device was found */
void blk_rw(struct buf *b, bool write);     /* synchronous: sleeps until the device is done */
void blk_flush(void);                       /* write barrier: everything written so far is durable */
uint64_t blk_capacity(void);                /* in 512-byte sectors */

/* Crash injection for the crash-consistency tests: power off right before
 * the nth disk write from now; if torn_sectors > 0, that write is cut short
 * after torn_sectors sectors instead of being dropped. */
void blk_crash_arm(uint64_t nth, int torn_sectors);

#endif
