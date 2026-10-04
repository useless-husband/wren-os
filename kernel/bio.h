/* Buffer cache: the only path between the file system and the disk.
 * A block has at most one buffer, and a buffer's sleep lock gives one
 * process at a time exclusive use of it. */
#ifndef KERNEL_BIO_H
#define KERNEL_BIO_H
#include "types.h"
#include "spinlock.h"

struct buf {
    bool             valid;      /* data holds the block's contents */
    uint32_t         dev, blockno;
    struct sleeplock lock;
    int              refcnt;     /* users + log pins; 0 = may be recycled */
    struct buf      *prev, *next;/* LRU list, most recently used first */
    uint8_t         *data;       /* BSIZE bytes, one physically contiguous page */
};

void bio_init(void);
struct buf *bread(uint32_t dev, uint32_t blockno);   /* locked, valid */
void bwrite(struct buf *b);                          /* caller holds b->lock */
void brelse(struct buf *b);
void bpin(struct buf *b);                            /* keep cached until bunpin (log) */
void bunpin(struct buf *b);

#endif
