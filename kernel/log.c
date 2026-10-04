/* Commit protocol (one transaction may hold many concurrent operations):
 *
 *   1. copy every dirty block to the log area            -- write, flush
 *   2. write the header: n, seq, block numbers and a CRC  -- write, flush
 *      over the header fields and all logged data.  This single block write
 *      is the commit point.
 *   3. write every block to its home location             -- write, flush
 *   4. clear the header (n = 0)                           -- write
 *
 * Recovery (mount): if the header has n > 0 and its CRC matches the logged
 * data, redo step 3; otherwise the transaction never committed and is
 * ignored.  The CRC makes recovery safe even when the device reorders or
 * tears writes, and makes step 4 an optimisation rather than a requirement:
 * a stale header whose log area was partly overwritten by the next
 * transaction fails its CRC and is never replayed.
 */
#include "log.h"
#include "bio.h"
#include "blk.h"
#include "printk.h"
#include "proc.h"
#include "spinlock.h"
#include "stats.h"
#include <wren/fsformat.h>
#include "../lib/crc32c.h"
#include "../lib/kstring.h"

static struct {
    struct spinlock lock;
    uint32_t dev, start, size;          /* header at start, data at start+1.. */
    uint32_t fs_size;
    int      outstanding;               /* operations between begin_op and end_op */
    bool     committing;
    uint64_t seq;                       /* transaction being built */
    uint64_t done_seq;                  /* newest transaction known to be on disk */
    uint32_t n;                         /* blocks in the open transaction */
    uint32_t block[LOG_DATA_BLOCKS];
} lg;

static uint32_t header_crc(const struct log_header *h)
{
    uint32_t c = crc32c_update(0, &h->magic, sizeof h->magic);
    c = crc32c_update(c, &h->n, sizeof h->n);
    c = crc32c_update(c, &h->seq, sizeof h->seq);
    return crc32c_update(c, h->block, h->n * sizeof h->block[0]);
}

static void write_header(uint32_t n, uint64_t seq, const uint32_t *blocks, uint32_t crc)
{
    struct buf *b = bread(lg.dev, lg.start);
    struct log_header *h = (struct log_header *)b->data;
    memset(b->data, 0, BSIZE);
    h->magic = LOG_MAGIC;
    h->n = n;
    h->seq = seq;
    if (n) memcpy(h->block, blocks, n * sizeof blocks[0]);
    h->crc = crc;
    bwrite(b);
    brelse(b);
}

static void recover(void)
{
    struct buf *hb = bread(lg.dev, lg.start);
    struct log_header h;
    memcpy(&h, hb->data, sizeof h);
    brelse(hb);
    lg.seq = (h.magic == LOG_MAGIC ? h.seq : 0) + 1;
    if (h.magic != LOG_MAGIC || h.n == 0) return;

    bool ok = h.n <= LOG_DATA_BLOCKS && h.n < lg.size;
    for (uint32_t i = 0; ok && i < h.n; i++)
        ok = h.block[i] >= lg.start + lg.size && h.block[i] < lg.fs_size;
    uint32_t crc = ok ? header_crc(&h) : 0;
    for (uint32_t i = 0; ok && i < h.n; i++) {
        struct buf *lb = bread(lg.dev, lg.start + 1 + i);
        crc = crc32c_update(crc, lb->data, BSIZE);
        brelse(lb);
    }
    if (!ok || crc != h.crc) {
        printk("log: discarding uncommitted transaction %lu (%u blocks)\n", h.seq, h.n);
    } else {
        for (uint32_t i = 0; i < h.n; i++) {
            struct buf *lb = bread(lg.dev, lg.start + 1 + i);
            struct buf *home = bread(lg.dev, h.block[i]);
            memcpy(home->data, lb->data, BSIZE);
            bwrite(home);
            brelse(home);
            brelse(lb);
        }
        blk_flush();
        printk("log: replayed committed transaction %lu (%u blocks)\n", h.seq, h.n);
    }
    write_header(0, h.seq, NULL, 0);
    blk_flush();
}

void log_init(uint32_t dev, const struct superblock *sb)
{
    spin_init(&lg.lock, "log");
    lg.dev = dev;
    lg.start = sb->logstart;
    lg.size = sb->nlog;
    lg.fs_size = sb->size;
    if (lg.size < 2 || lg.size - 1 < LOG_DATA_BLOCKS) panic("log: region too small (%u blocks)", lg.size);
    recover();
}

static void commit(uint64_t seq)
{
    uint32_t n = lg.n;   /* stable: no operation can join while committing */
    if (n == 0) return;
    struct log_header h = {.magic = LOG_MAGIC, .n = n, .seq = seq};
    memcpy(h.block, lg.block, n * sizeof h.block[0]);
    uint32_t crc = header_crc(&h);
    for (uint32_t i = 0; i < n; i++) {                      /* 1. log the blocks */
        struct buf *from = bread(lg.dev, lg.block[i]);      /* pinned: a cache hit */
        struct buf *to = bread(lg.dev, lg.start + 1 + i);
        memcpy(to->data, from->data, BSIZE);
        crc = crc32c_update(crc, to->data, BSIZE);
        bwrite(to);
        brelse(to);
        brelse(from);
    }
    blk_flush();
    write_header(n, seq, lg.block, crc);                    /* 2. commit point */
    blk_flush();
    for (uint32_t i = 0; i < n; i++) {                      /* 3. install */
        struct buf *b = bread(lg.dev, lg.block[i]);
        bwrite(b);
        bunpin(b);
        brelse(b);
    }
    blk_flush();
    write_header(0, seq, NULL, 0);                          /* 4. retire */
    lg.n = 0;
    stat_inc(STAT_LOG_COMMITS);
}

void log_begin_op(void)
{
    spin_lock(&lg.lock);
    for (;;) {
        if (lg.committing) proc_sleep(&lg, &lg.lock);
        else if (lg.n + (uint32_t)(lg.outstanding + 1) * MAXOPBLOCKS > LOG_DATA_BLOCKS)
            proc_sleep(&lg, &lg.lock);      /* could overflow the log: wait for a commit */
        else break;
    }
    lg.outstanding++;
    spin_unlock(&lg.lock);
}

void log_end_op(void)
{
    spin_lock(&lg.lock);
    uint64_t mine = lg.seq;
    if (lg.outstanding <= 0 || lg.committing) panic("log_end_op: outstanding %d committing %d", lg.outstanding,
                                                    lg.committing);
    bool do_commit = --lg.outstanding == 0;
    if (do_commit) {
        lg.committing = true;
    } else {
        proc_wakeup(&lg);                   /* begin_op may be waiting for log space */
        while (lg.done_seq < mine) proc_sleep(&lg, &lg.lock);   /* durable before we return */
    }
    spin_unlock(&lg.lock);
    if (do_commit) {
        commit(mine);                       /* sleeps on disk I/O; no lock held */
        spin_lock(&lg.lock);
        lg.committing = false;
        lg.done_seq = mine;
        lg.seq = mine + 1;
        proc_wakeup(&lg);
        spin_unlock(&lg.lock);
    }
}

void log_write(struct buf *b)
{
    spin_lock(&lg.lock);
    if (lg.outstanding < 1) panic("log_write outside a transaction");
    uint32_t i;
    for (i = 0; i < lg.n; i++)
        if (lg.block[i] == b->blockno) break;               /* absorbed: already logged */
    if (i == lg.n) {
        if (lg.n >= LOG_DATA_BLOCKS || lg.n >= lg.size - 1) panic("log_write: transaction too big");
        lg.block[lg.n++] = b->blockno;
        bpin(b);
    }
    spin_unlock(&lg.lock);
}
