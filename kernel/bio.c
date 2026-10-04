#include "bio.h"
#include "blk.h"
#include "kalloc.h"
#include "memlayout.h"
#include "param.h"
#include "printk.h"

static struct {
    struct spinlock lock;
    struct buf      bufs[NBUF];
    struct buf      head;        /* sentinel of the circular LRU list */
} bc;

void bio_init(void)
{
    spin_init(&bc.lock, "bcache");
    bc.head.next = bc.head.prev = &bc.head;
    for (struct buf *b = bc.bufs; b < bc.bufs + NBUF; b++) {
        paddr_t pa = page_alloc();
        if (!pa) panic("bio_init: out of memory");
        b->data = P2V(pa);
        sleep_init(&b->lock, "buffer");
        b->next = bc.head.next;
        b->prev = &bc.head;
        bc.head.next->prev = b;
        bc.head.next = b;
    }
}

static void move_to_front(struct buf *b)
{
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bc.head.next;
    b->prev = &bc.head;
    bc.head.next->prev = b;
    bc.head.next = b;
}

static struct buf *bget(uint32_t dev, uint32_t blockno)
{
    spin_lock(&bc.lock);
    for (struct buf *b = bc.head.next; b != &bc.head; b = b->next)
        if (b->dev == dev && b->blockno == blockno && (b->valid || b->refcnt)) {
            b->refcnt++;
            spin_unlock(&bc.lock);
            sleep_lock(&b->lock);
            return b;
        }
    /* Not cached: recycle the least recently used idle buffer. */
    for (struct buf *b = bc.head.prev; b != &bc.head; b = b->prev)
        if (b->refcnt == 0) {
            b->dev = dev;
            b->blockno = blockno;
            b->valid = false;
            b->refcnt = 1;
            spin_unlock(&bc.lock);
            sleep_lock(&b->lock);
            return b;
        }
    panic("bget: all %d buffers busy", NBUF);
}

struct buf *bread(uint32_t dev, uint32_t blockno)
{
    struct buf *b = bget(dev, blockno);
    if (!b->valid) {
        blk_rw(b, false);
        b->valid = true;
    }
    return b;
}

void bwrite(struct buf *b)
{
    if (!sleep_held(&b->lock)) panic("bwrite: buffer not locked");
    blk_rw(b, true);
}

void brelse(struct buf *b)
{
    if (!sleep_held(&b->lock)) panic("brelse: buffer not locked");
    sleep_unlock(&b->lock);
    spin_lock(&bc.lock);
    if (--b->refcnt == 0) move_to_front(b);
    spin_unlock(&bc.lock);
}

void bpin(struct buf *b)
{
    spin_lock(&bc.lock);
    b->refcnt++;
    spin_unlock(&bc.lock);
}

void bunpin(struct buf *b)
{
    spin_lock(&bc.lock);
    b->refcnt--;
    spin_unlock(&bc.lock);
}
