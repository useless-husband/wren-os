/* virtio-blk over virtio-mmio, both transport versions:
 *   version 1 ("legacy"): QEMU's default; the queue is one contiguous area
 *     located through QueuePFN, with GuestPageSize/QueueAlign.
 *   version 2 ("modern"): LeapVM (and QEMU with force-legacy=false); three
 *     separate addresses, QueueReady, FEATURES_OK and VIRTIO_F_VERSION_1.
 * Spec: Virtual I/O Device (VIRTIO) 1.2, sections 2.7 (split virtqueues),
 * 4.2.2 (MMIO registers), 4.2.4 (legacy interface), 5.2 (block device).
 *
 * Requests are 3-descriptor chains (header, data, status).  Several can be
 * in flight; the submitter sleeps until the interrupt handler marks its
 * request done. */
#include "blk.h"
#include "bio.h"
#include "arch.h"
#include "gic.h"
#include "kalloc.h"
#include "memlayout.h"
#include "platform.h"
#include "printk.h"
#include "proc.h"
#include "psci.h"
#include "spinlock.h"
#include "stats.h"
#include <wren/fsformat.h>
#include "../lib/kstring.h"

#define R_MAGIC         0x000
#define R_VERSION       0x004
#define R_DEVICE_ID     0x008
#define R_DEV_FEATURES  0x010
#define R_DEV_FEAT_SEL  0x014
#define R_DRV_FEATURES  0x020
#define R_DRV_FEAT_SEL  0x024
#define R_GUEST_PAGE    0x028   /* legacy */
#define R_QUEUE_SEL     0x030
#define R_QUEUE_NUM_MAX 0x034
#define R_QUEUE_NUM     0x038
#define R_QUEUE_ALIGN   0x03c   /* legacy */
#define R_QUEUE_PFN     0x040   /* legacy */
#define R_QUEUE_READY   0x044
#define R_QUEUE_NOTIFY  0x050
#define R_INT_STATUS    0x060
#define R_INT_ACK       0x064
#define R_STATUS        0x070
#define R_QUEUE_DESC    0x080
#define R_QUEUE_AVAIL   0x090
#define R_QUEUE_USED    0x0a0
#define R_CONFIG        0x100

#define S_ACK           1
#define S_DRIVER        2
#define S_DRIVER_OK     4
#define S_FEATURES_OK   8
#define S_FAILED        128

#define F_BLK_FLUSH     (1ULL << 9)
#define F_VERSION_1     (1ULL << 32)

#define T_IN    0
#define T_OUT   1
#define T_FLUSH 4

#define D_NEXT  1
#define D_WRITE 2

#define QSIZE 64

struct vdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct vavail { uint16_t flags, idx, ring[QSIZE]; };
struct vused_elem { uint32_t id, len; };
struct vused { uint16_t flags, idx; struct vused_elem ring[QSIZE]; };

struct blk_req {
    uint32_t type, reserved;
    uint64_t sector;
};

static struct {
    uint64_t        regs;       /* VA of the MMIO window */
    int             version;
    bool            has_flush;
    uint64_t        capacity;   /* sectors */
    struct vdesc   *desc;
    struct vavail  *avail;
    struct vused   *used;
    bool            free[QSIZE];
    int             nfree;
    uint16_t        used_seen;
    struct spinlock lock;
    struct {            /* per head descriptor */
        struct blk_req hdr;
        volatile uint8_t status;
        volatile bool done;
    } req[QSIZE];
    uint64_t        crash_countdown;
    int             crash_torn;
} vb;

static uint32_t rd(uint32_t off) { return mmio_read32(vb.regs + off); }
static void wr(uint32_t off, uint32_t v) { mmio_write32(vb.regs + off, v); }

static void blk_intr(void *arg);

uint64_t blk_capacity(void) { return vb.capacity; }

static int setup_device(uint64_t base)
{
    vb.regs = (uint64_t)P2V(base);
    vb.version = (int)rd(R_VERSION);
    wr(R_STATUS, 0);
    wr(R_STATUS, S_ACK);
    wr(R_STATUS, S_ACK | S_DRIVER);

    wr(R_DEV_FEAT_SEL, 0);
    uint64_t feat = rd(R_DEV_FEATURES);
    if (vb.version >= 2) {
        wr(R_DEV_FEAT_SEL, 1);
        feat |= (uint64_t)rd(R_DEV_FEATURES) << 32;
    }
    uint64_t want = feat & F_BLK_FLUSH;
    if (vb.version >= 2) {
        if (!(feat & F_VERSION_1)) return -1;
        want |= F_VERSION_1;
    }
    vb.has_flush = want & F_BLK_FLUSH;
    wr(R_DRV_FEAT_SEL, 0);
    wr(R_DRV_FEATURES, (uint32_t)want);
    if (vb.version >= 2) {
        wr(R_DRV_FEAT_SEL, 1);
        wr(R_DRV_FEATURES, (uint32_t)(want >> 32));
        wr(R_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
        if (!(rd(R_STATUS) & S_FEATURES_OK)) return -1;
    } else {
        wr(R_GUEST_PAGE, PAGE_SIZE);
    }

    wr(R_QUEUE_SEL, 0);
    if (vb.version >= 2 && rd(R_QUEUE_READY)) return -1;
    if (rd(R_QUEUE_NUM_MAX) < QSIZE) return -1;
    wr(R_QUEUE_NUM, QSIZE);
    /* desc + avail in the first page, used ring at the next 4 KiB boundary. */
    paddr_t q = pages_alloc(1);
    if (!q) return -1;
    vb.desc = P2V(q);
    vb.avail = P2V(q + 16 * QSIZE);
    vb.used = P2V(q + PAGE_SIZE);
    if (vb.version >= 2) {
        paddr_t a = V2P(vb.avail), u = V2P(vb.used);
        wr(R_QUEUE_DESC, (uint32_t)q);
        wr(R_QUEUE_DESC + 4, (uint32_t)(q >> 32));
        wr(R_QUEUE_AVAIL, (uint32_t)a);
        wr(R_QUEUE_AVAIL + 4, (uint32_t)(a >> 32));
        wr(R_QUEUE_USED, (uint32_t)u);
        wr(R_QUEUE_USED + 4, (uint32_t)(u >> 32));
        wr(R_QUEUE_READY, 1);
    } else {
        wr(R_QUEUE_ALIGN, PAGE_SIZE);
        wr(R_QUEUE_PFN, (uint32_t)(q >> PAGE_SHIFT));
    }
    for (int i = 0; i < QSIZE; i++) vb.free[i] = true;
    vb.nfree = QSIZE;
    vb.capacity = (uint64_t)rd(R_CONFIG) | (uint64_t)rd(R_CONFIG + 4) << 32;
    wr(R_STATUS, rd(R_STATUS) | S_DRIVER_OK);
    return 0;
}

int blk_init(const struct platform *pf)
{
    spin_init(&vb.lock, "virtio-blk");
    for (int i = 0; i < pf->nvirtio; i++) {
        uint64_t regs = (uint64_t)P2V(pf->virtio_base[i]);
        if (mmio_read32(regs + R_MAGIC) != 0x74726976 || mmio_read32(regs + R_DEVICE_ID) != 2) continue;
        if (setup_device(pf->virtio_base[i]) != 0) {
            printk("virtio-blk at %p: setup failed\n", (void *)pf->virtio_base[i]);
            mmio_write32(regs + R_STATUS, S_FAILED);
            continue;
        }
        gic_enable(pf->virtio_intid[i], blk_intr, NULL);
        printk("virtio-blk: %p, %s transport, %lu KiB, flush %s\n", (void *)pf->virtio_base[i],
               vb.version >= 2 ? "modern" : "legacy", vb.capacity / 2, vb.has_flush ? "yes" : "no");
        return 0;
    }
    return -1;
}

static int alloc_desc(void)
{
    for (int i = 0; i < QSIZE; i++)
        if (vb.free[i]) {
            vb.free[i] = false;
            vb.nfree--;
            return i;
        }
    return -1;
}

static void free_chain(int head)
{
    for (int i = head;;) {
        int next = vb.desc[i].next, more = vb.desc[i].flags & D_NEXT;
        vb.desc[i] = (struct vdesc){0};
        vb.free[i] = true;
        vb.nfree++;
        if (!more) break;
        i = next;
    }
    proc_wakeup(&vb.free);
}

/* Submit one request and sleep until it completes.  data may be NULL (flush). */
static int submit(uint32_t type, uint64_t sector, void *data, uint32_t len, bool device_writes)
{
    spin_lock(&vb.lock);
    int need = data ? 3 : 2;
    while (vb.nfree < need) proc_sleep(&vb.free, &vb.lock);
    int d[3];
    for (int i = 0; i < need; i++) d[i] = alloc_desc();
    int head = d[0];
    vb.req[head].hdr = (struct blk_req){.type = type, .sector = sector};
    vb.req[head].status = 0xff;
    vb.req[head].done = false;
    vb.desc[d[0]] = (struct vdesc){V2P(&vb.req[head].hdr), sizeof(struct blk_req), D_NEXT, (uint16_t)d[1]};
    if (data) {
        vb.desc[d[1]] = (struct vdesc){V2P(data), len, (uint16_t)(D_NEXT | (device_writes ? D_WRITE : 0)),
                                       (uint16_t)d[2]};
    }
    vb.desc[d[need - 1]] = (struct vdesc){V2P((void *)&vb.req[head].status), 1, D_WRITE, 0};
    vb.avail->ring[vb.avail->idx % QSIZE] = (uint16_t)head;
    dmb_ish();                                /* descriptors before the index */
    vb.avail->idx++;
    dsb_sy();                                 /* index before the doorbell */
    wr(R_QUEUE_NOTIFY, 0);
    while (!vb.req[head].done) proc_sleep(&vb.req[head], &vb.lock);
    int status = vb.req[head].status;
    free_chain(head);
    spin_unlock(&vb.lock);
    return status;
}

static void blk_intr(void *arg)
{
    (void)arg;
    spin_lock(&vb.lock);
    wr(R_INT_ACK, rd(R_INT_STATUS) & 3);
    dmb_ish();
    while (vb.used_seen != __atomic_load_n(&vb.used->idx, __ATOMIC_ACQUIRE)) {
        int id = (int)vb.used->ring[vb.used_seen % QSIZE].id;
        if (id >= QSIZE) panic("virtio-blk: bad used id %d", id);
        vb.req[id].done = true;
        proc_wakeup(&vb.req[id]);
        vb.used_seen++;
    }
    spin_unlock(&vb.lock);
}

void blk_crash_arm(uint64_t nth, int torn_sectors)
{
    spin_lock(&vb.lock);
    vb.crash_countdown = nth;
    vb.crash_torn = torn_sectors;
    spin_unlock(&vb.lock);
}

void blk_rw(struct buf *b, bool write)
{
    uint64_t sector = (uint64_t)b->blockno * SECTORS_PER_BLOCK;
    uint32_t len = BSIZE;
    if (write) {
        stat_inc(STAT_DISK_WRITES);
        spin_lock(&vb.lock);
        bool crash = vb.crash_countdown && --vb.crash_countdown == 0;
        int torn = vb.crash_torn;
        spin_unlock(&vb.lock);
        if (crash) {
            if (torn > 0) {
                submit(T_OUT, sector, b->data, (uint32_t)MIN(torn, SECTORS_PER_BLOCK) * 512, false);
                printk("[crash] power cut during write of block %u (%d of %d sectors landed)\n", b->blockno,
                       torn, SECTORS_PER_BLOCK);
            } else {
                printk("[crash] power cut before write of block %u\n", b->blockno);
            }
            psci_system_off();
        }
    } else {
        stat_inc(STAT_DISK_READS);
    }
    int st = submit(write ? T_OUT : T_IN, sector, b->data, len, !write);
    if (st != 0) panic("virtio-blk: %s of block %u failed (status %d)", write ? "write" : "read", b->blockno, st);
}

void blk_flush(void)
{
    if (!vb.has_flush) return;
    int st = submit(T_FLUSH, 0, NULL, 0, false);
    if (st != 0) panic("virtio-blk: flush failed (status %d)", st);
}
