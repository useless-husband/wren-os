#include "fs.h"
#include "bio.h"
#include "log.h"
#include "param.h"
#include "printk.h"
#include "proc.h"
#include "vm.h"
#include <wren/errno.h>
#include <wren/stat.h>
#include "../lib/kstring.h"

static struct superblock sb;

const struct superblock *fs_super(void) { return &sb; }

void fs_init(uint32_t dev)
{
    struct buf *b = bread(dev, 1);
    memcpy(&sb, b->data, sizeof sb);
    brelse(b);
    if (sb.magic != FS_MAGIC) panic("fs: bad superblock magic %x", sb.magic);
    printk("fs: %u blocks, %u inodes, log %u blocks at %u\n", sb.size, sb.ninodes, sb.nlog, sb.logstart);
    log_init(dev, &sb);
}

/* ---------------------------------------------------------- blocks */

static void bzero_block(uint32_t dev, uint32_t bno)
{
    struct buf *b = bread(dev, bno);
    memset(b->data, 0, BSIZE);
    log_write(b);
    brelse(b);
}

/* Allocate a zeroed data block; 0 if the disk is full. */
static uint32_t balloc(uint32_t dev)
{
    for (uint32_t base = 0; base < sb.size; base += BPB) {
        struct buf *b = bread(dev, BBLOCK(base, sb));
        for (uint32_t bi = 0; bi < BPB && base + bi < sb.size; bi++) {
            uint8_t mask = (uint8_t)(1u << (bi % 8));
            if (b->data[bi / 8] & mask) continue;
            b->data[bi / 8] |= mask;
            log_write(b);
            brelse(b);
            bzero_block(dev, base + bi);
            return base + bi;
        }
        brelse(b);
    }
    return 0;
}

static void bfree(uint32_t dev, uint32_t bno)
{
    if (bno < sb.datastart || bno >= sb.size) panic("bfree: block %u outside the data area", bno);
    struct buf *b = bread(dev, BBLOCK(bno, sb));
    uint32_t bi = bno % BPB;
    uint8_t mask = (uint8_t)(1u << (bi % 8));
    if (!(b->data[bi / 8] & mask)) panic("bfree: block %u already free", bno);
    b->data[bi / 8] &= (uint8_t)~mask;
    log_write(b);
    brelse(b);
}

/* ---------------------------------------------------------- inodes */

static struct {
    struct spinlock lock;
    struct inode    inode[NINODE];
} icache;

static void icache_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    spin_init(&icache.lock, "icache");
    for (int i = 0; i < NINODE; i++) sleep_init(&icache.inode[i].lock, "inode");
}

static struct inode *iget(uint32_t dev, uint32_t inum)
{
    icache_init();
    spin_lock(&icache.lock);
    struct inode *empty = NULL;
    for (struct inode *ip = icache.inode; ip < icache.inode + NINODE; ip++) {
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
            ip->ref++;
            spin_unlock(&icache.lock);
            return ip;
        }
        if (!empty && ip->ref == 0) empty = ip;
    }
    if (!empty) panic("iget: inode cache full");
    empty->dev = dev;
    empty->inum = inum;
    empty->ref = 1;
    empty->valid = false;
    spin_unlock(&icache.lock);
    return empty;
}

struct inode *ialloc(uint32_t dev, uint16_t type)
{
    for (uint32_t inum = 1; inum < sb.ninodes; inum++) {
        struct buf *b = bread(dev, IBLOCK(inum, sb));
        struct dinode *dip = (struct dinode *)b->data + inum % IPB;
        if (dip->type == DI_FREE) {
            memset(dip, 0, sizeof *dip);
            dip->type = type;
            log_write(b);
            brelse(b);
            return iget(dev, inum);
        }
        brelse(b);
    }
    return NULL;
}

/* Write the in-memory inode back (through the log).  Caller holds ip->lock. */
void iupdate(struct inode *ip)
{
    struct buf *b = bread(ip->dev, IBLOCK(ip->inum, sb));
    struct dinode *dip = (struct dinode *)b->data + ip->inum % IPB;
    dip->type = ip->type;
    dip->major = ip->major;
    dip->nlink = ip->nlink;
    dip->size = ip->size;
    memcpy(dip->addrs, ip->addrs, sizeof ip->addrs);
    log_write(b);
    brelse(b);
}

struct inode *idup(struct inode *ip)
{
    spin_lock(&icache.lock);
    ip->ref++;
    spin_unlock(&icache.lock);
    return ip;
}

void ilock(struct inode *ip)
{
    if (!ip || ip->ref < 1) panic("ilock: bad inode");
    sleep_lock(&ip->lock);
    if (!ip->valid) {
        struct buf *b = bread(ip->dev, IBLOCK(ip->inum, sb));
        struct dinode *dip = (struct dinode *)b->data + ip->inum % IPB;
        ip->type = dip->type;
        ip->major = dip->major;
        ip->nlink = dip->nlink;
        ip->size = dip->size;
        memcpy(ip->addrs, dip->addrs, sizeof ip->addrs);
        brelse(b);
        ip->valid = true;
        if (ip->type == DI_FREE) panic("ilock: inode %u is free", ip->inum);
    }
}

void iunlock(struct inode *ip)
{
    if (!ip || !sleep_held(&ip->lock) || ip->ref < 1) panic("iunlock: bad inode");
    sleep_unlock(&ip->lock);
}

/* Drop a reference.  The last reference to an unlinked inode frees it on
 * disk, so iput() must be called inside a transaction. */
void iput(struct inode *ip)
{
    spin_lock(&icache.lock);
    if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
        /* No other pointer exists and no directory names it, so nobody can
         * find it meanwhile: the sleep lock is free. */
        spin_unlock(&icache.lock);
        sleep_lock(&ip->lock);
        itrunc(ip);
        ip->type = DI_FREE;
        iupdate(ip);
        ip->valid = false;
        sleep_unlock(&ip->lock);
        spin_lock(&icache.lock);
    }
    ip->ref--;
    spin_unlock(&icache.lock);
}

void iunlockput(struct inode *ip)
{
    iunlock(ip);
    iput(ip);
}

void stati(struct inode *ip, struct stat *st)
{
    st->dev = ip->dev;
    st->ino = ip->inum;
    st->type = ip->type;
    st->nlink = ip->nlink;
    st->size = ip->size;
}

/* ------------------------------------------------------- block map */

/* Entry `idx` of indirect block `ind`, allocating the target if asked.
 * Returns the block number, 0 if absent (or the disk is full). */
static uint32_t ind_entry(uint32_t dev, uint32_t ind, uint32_t idx, bool alloc)
{
    struct buf *b = bread(dev, ind);
    uint32_t *a = (uint32_t *)b->data;
    uint32_t r = a[idx];
    if (!r && alloc && (r = balloc(dev)) != 0) {
        a[idx] = r;
        log_write(b);
    }
    brelse(b);
    return r;
}

/* Disk block holding byte bn*BSIZE of the file; 0 if it has none. */
static uint32_t bmap(struct inode *ip, uint32_t bn, bool alloc)
{
    if (bn < NDIRECT) {
        if (!ip->addrs[bn] && alloc) ip->addrs[bn] = balloc(ip->dev);
        return ip->addrs[bn];
    }
    bn -= NDIRECT;
    int slot;
    uint32_t levels;
    if (bn < NINDIRECT) {
        slot = NDIRECT;
        levels = 1;
    } else if ((bn -= NINDIRECT) < NINDIRECT * NINDIRECT) {
        slot = NDIRECT + 1;
        levels = 2;
    } else {
        return 0;
    }
    if (!ip->addrs[slot]) {
        if (!alloc || !(ip->addrs[slot] = balloc(ip->dev))) return 0;
    }
    uint32_t blk = ip->addrs[slot];
    if (levels == 2) {
        blk = ind_entry(ip->dev, blk, bn / NINDIRECT, alloc);
        if (!blk) return 0;
        bn %= NINDIRECT;
    }
    return ind_entry(ip->dev, blk, bn, alloc);
}

/* Lock order is indirect block -> bitmap block, as in bmap()/balloc(). */
static void free_indirect(uint32_t dev, uint32_t ind, int depth)
{
    struct buf *b = bread(dev, ind);
    const uint32_t *entries = (const uint32_t *)b->data;
    for (uint32_t i = 0; i < NINDIRECT; i++) {
        if (!entries[i]) continue;
        if (depth > 1) free_indirect(dev, entries[i], depth - 1);
        else bfree(dev, entries[i]);
    }
    brelse(b);
    bfree(dev, ind);
}

/* Free all of the file's blocks.  Caller holds ip->lock, inside a transaction. */
void itrunc(struct inode *ip)
{
    for (int i = 0; i < NDIRECT; i++)
        if (ip->addrs[i]) {
            bfree(ip->dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    if (ip->addrs[NDIRECT]) free_indirect(ip->dev, ip->addrs[NDIRECT], 1);
    if (ip->addrs[NDIRECT + 1]) free_indirect(ip->dev, ip->addrs[NDIRECT + 1], 2);
    ip->addrs[NDIRECT] = ip->addrs[NDIRECT + 1] = 0;
    ip->size = 0;
    iupdate(ip);
}

/* ------------------------------------------------------- data */

static int copy_out(bool user, uint64_t dst, const void *src, size_t n)
{
    if (user) return copyout(&myproc()->vm, dst, src, n);
    memcpy((void *)dst, src, n);
    return 0;
}

static int copy_in(bool user, void *dst, uint64_t src, size_t n)
{
    if (user) return copyin(&myproc()->vm, dst, src, n);
    memcpy(dst, (const void *)src, n);
    return 0;
}

int readi(struct inode *ip, bool user, uint64_t dst, uint32_t off, uint32_t n)
{
    if (off > ip->size) return 0;
    if (n > ip->size - off) n = ip->size - off;
    uint32_t done = 0;
    while (done < n) {
        uint32_t bno = bmap(ip, off / BSIZE, false);
        uint32_t m = MIN(n - done, BSIZE - off % BSIZE);
        if (bno) {
            struct buf *b = bread(ip->dev, bno);
            int r = copy_out(user, dst, b->data + off % BSIZE, m);
            brelse(b);
            if (r < 0) return done ? (int)done : r;
        } else {                                  /* a hole reads as zeros */
            static const uint8_t zeros[512];
            for (uint32_t k = 0; k < m; k += MIN(m - k, sizeof zeros)) {
                int r = copy_out(user, dst + k, zeros, MIN(m - k, sizeof zeros));
                if (r < 0) return done ? (int)done : r;
            }
        }
        done += m;
        off += m;
        dst += m;
    }
    return (int)done;
}

/* Caller holds ip->lock and is inside a transaction that can absorb
 * n bytes (see MAXWRITE_TXN). */
int writei(struct inode *ip, bool user, uint64_t src, uint32_t off, uint32_t n)
{
    if ((uint64_t)off + n > (uint64_t)MAXFILE_BLOCKS * BSIZE || (uint64_t)off + n > UINT32_MAX) return -EFBIG;
    uint32_t done = 0;
    int err = 0;
    while (done < n) {
        uint32_t bno = bmap(ip, off / BSIZE, true);
        if (!bno) {
            err = -ENOSPC;
            break;
        }
        uint32_t m = MIN(n - done, BSIZE - off % BSIZE);
        struct buf *b = bread(ip->dev, bno);
        int r = copy_in(user, b->data + off % BSIZE, src, m);
        if (r < 0) {
            brelse(b);
            err = r;
            break;
        }
        log_write(b);
        brelse(b);
        done += m;
        off += m;
        src += m;
    }
    if (off > ip->size) ip->size = off;
    iupdate(ip);          /* bmap may have changed addrs even if size did not */
    return done ? (int)done : err;
}

/* ------------------------------------------------------ directories */

static bool name_eq(const char *a, const char *b)
{
    return strncmp(a, b, DIRSIZ) == 0;
}

/* Look name up in directory dp (locked).  Returns the inode unlocked, with
 * the entry's byte offset in *poff. */
struct inode *dirlookup(struct inode *dp, const char *name, uint32_t *poff)
{
    if (dp->type != DI_DIR) panic("dirlookup: not a directory");
    struct dirent de;
    for (uint32_t off = 0; off < dp->size; off += sizeof de) {
        if (readi(dp, false, (uint64_t)&de, off, sizeof de) != sizeof de) panic("dirlookup: read");
        if (de.inum && name_eq(name, de.name)) {
            if (poff) *poff = off;
            return iget(dp->dev, de.inum);
        }
    }
    return NULL;
}

int dirlink(struct inode *dp, const char *name, uint32_t inum)
{
    struct inode *ip = dirlookup(dp, name, NULL);
    if (ip) {
        iput(ip);
        return -EEXIST;
    }
    struct dirent de;
    uint32_t off;
    for (off = 0; off < dp->size; off += sizeof de) {
        if (readi(dp, false, (uint64_t)&de, off, sizeof de) != sizeof de) panic("dirlink: read");
        if (de.inum == 0) break;
    }
    memset(&de, 0, sizeof de);
    memcpy(de.name, name, strnlen(name, DIRSIZ));
    de.inum = inum;
    int r = writei(dp, false, (uint64_t)&de, off, sizeof de);
    return r == sizeof de ? 0 : (r < 0 ? r : -ENOSPC);
}

int dir_unlink(struct inode *dp, uint32_t off)
{
    struct dirent de;
    memset(&de, 0, sizeof de);
    return writei(dp, false, (uint64_t)&de, off, sizeof de) == sizeof de ? 0 : -EIO;
}

bool dir_empty(struct inode *dp)
{
    struct dirent de;
    for (uint32_t off = 2 * sizeof de; off < dp->size; off += sizeof de) {   /* skip . and .. */
        if (readi(dp, false, (uint64_t)&de, off, sizeof de) != sizeof de) panic("dir_empty: read");
        if (de.inum) return false;
    }
    return true;
}

/* ------------------------------------------------------------ paths */

/* Copy the next path component into name and return the rest of the path,
 * or NULL when there are no components left. */
static const char *next_component(const char *path, char *name)
{
    while (*path == '/') path++;
    if (!*path) return NULL;
    const char *start = path;
    while (*path && *path != '/') path++;
    size_t len = (size_t)(path - start);
    if (len > DIRSIZ) len = DIRSIZ;
    memcpy(name, start, len);
    name[len] = 0;
    while (*path == '/') path++;
    return path;
}

static struct inode *walk_path(const char *path, bool want_parent, char *name)
{
    struct inode *ip = path[0] == '/' ? iget(ROOTDEV, ROOTINO) : idup(myproc()->cwd);
    char tmp[DIRSIZ + 1];
    if (!name) name = tmp;
    while ((path = next_component(path, name)) != NULL) {
        ilock(ip);
        if (ip->type != DI_DIR) {
            iunlockput(ip);
            return NULL;
        }
        if (want_parent && *path == 0) {       /* stop one level early */
            iunlock(ip);
            return ip;
        }
        struct inode *next = dirlookup(ip, name, NULL);
        iunlockput(ip);
        if (!next) return NULL;
        ip = next;
    }
    if (want_parent) {       /* path had no components ("/") */
        iput(ip);
        return NULL;
    }
    return ip;
}

struct inode *namei(const char *path) { return walk_path(path, false, NULL); }
struct inode *nameiparent(const char *path, char *name) { return walk_path(path, true, name); }
