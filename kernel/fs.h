/* The on-disk file system: inodes, block maps, directories and paths.
 * Layout: include/wren/fsformat.h.  Every function that modifies disk
 * blocks must run inside log_begin_op()/log_end_op(). */
#ifndef KERNEL_FS_H
#define KERNEL_FS_H
#include "types.h"
#include "spinlock.h"
#include <wren/fsformat.h>

#define ROOTDEV 1

struct stat;

/* In-memory copy of an inode.  ref counts C pointers (protected by the
 * inode-cache lock); everything below `lock` is protected by the sleep lock. */
struct inode {
    uint32_t dev, inum;
    int      ref;
    struct sleeplock lock;
    bool     valid;          /* fields below loaded from disk */
    uint16_t type, major, nlink;
    uint32_t size;
    uint32_t addrs[NDIRECT + 2];
};

void fs_init(uint32_t dev);
const struct superblock *fs_super(void);

struct inode *ialloc(uint32_t dev, uint16_t type);   /* returned unlocked, ref 1; NULL if out of inodes */
struct inode *idup(struct inode *ip);
void ilock(struct inode *ip);
void iunlock(struct inode *ip);
void iput(struct inode *ip);
void iunlockput(struct inode *ip);
void iupdate(struct inode *ip);
void stati(struct inode *ip, struct stat *st);
void itrunc(struct inode *ip);                       /* free all data blocks */

/* user: dst/src are user addresses of the current process.  Return bytes
 * transferred or a negative error. */
int readi(struct inode *ip, bool user, uint64_t dst, uint32_t off, uint32_t n);
int writei(struct inode *ip, bool user, uint64_t src, uint32_t off, uint32_t n);

struct inode *dirlookup(struct inode *dp, const char *name, uint32_t *poff);
int  dirlink(struct inode *dp, const char *name, uint32_t inum);
bool dir_empty(struct inode *dp);
int  dir_unlink(struct inode *dp, uint32_t off);     /* clear the entry at off */

struct inode *namei(const char *path);
struct inode *nameiparent(const char *path, char *name);   /* name: DIRSIZ+1 bytes */

#endif
