/* On-disk layout of the wren file system.  Shared by the kernel, the host
 * mkfs and the host fsck, so it uses only fixed-width types.
 *
 *   block 0        unused (boot block)
 *   block 1        superblock
 *   log            1 header block + LOG_DATA_BLOCKS data blocks
 *   inode table    NINODES / IPB blocks
 *   free bitmap    one bit per block of the whole disk
 *   data           everything else
 *
 * All multi-byte fields are little-endian (both the target and every host
 * we build on are little-endian; mkfs checks this at run time). */
#ifndef WREN_FSFORMAT_H
#define WREN_FSFORMAT_H
#include <stdint.h>

#define BSIZE        4096          /* block size in bytes */
#define SECTORS_PER_BLOCK (BSIZE / 512)

#define FS_MAGIC     0x4e455257u   /* "WREN" */
#define LOG_MAGIC    0x474f4c57u   /* "WLOG" */

#define ROOTINO      1             /* root directory inode number */

/* Largest number of distinct blocks one file-system operation may dirty,
 * and how many operations' worth of space the log reserves. */
#define MAXOPBLOCKS  16
#define LOG_DATA_BLOCKS (MAXOPBLOCKS * 4)
/* Largest write() that is one transaction (and therefore atomic): it may
 * span 9 data blocks, plus the inode, 2 bitmap and 3 indirect blocks. */
#define MAXWRITE_TXN (8 * BSIZE)

struct superblock {
    uint32_t magic;
    uint32_t size;        /* total blocks on disk */
    uint32_t nblocks;     /* data blocks */
    uint32_t ninodes;
    uint32_t nlog;        /* log blocks including the header */
    uint32_t logstart;
    uint32_t inodestart;
    uint32_t bmapstart;
    uint32_t datastart;
};

/* Log header.  A transaction is committed exactly when a header with
 * n > 0 and a matching checksum is on disk.  The checksum covers the header
 * fields and the n logged blocks, so a header that reached the disk ahead of
 * (or without) its data blocks is detected and ignored at recovery. */
struct log_header {
    uint32_t magic;
    uint32_t n;                         /* blocks in the committed txn; 0 = empty */
    uint64_t seq;                       /* transaction sequence number */
    uint32_t crc;                       /* crc32c of header fields + data blocks */
    uint32_t block[LOG_DATA_BLOCKS];    /* home block number of each logged block */
};

#define NDIRECT   11
#define NINDIRECT (BSIZE / sizeof(uint32_t))
#define MAXFILE_BLOCKS (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)

/* Inode types (same values as struct stat.type). */
#define DI_FREE   0
#define DI_DIR    1
#define DI_FILE   2
#define DI_DEVICE 3

struct dinode {
    uint16_t type;
    uint16_t major;       /* device number for DI_DEVICE */
    uint16_t nlink;
    uint16_t pad;
    uint32_t size;        /* bytes */
    uint32_t addrs[NDIRECT + 2];   /* direct, single indirect, double indirect */
};

#define IPB (BSIZE / sizeof(struct dinode))     /* inodes per block */
#define IBLOCK(i, sb) ((i) / IPB + (sb).inodestart)
#define BPB (BSIZE * 8)                          /* bitmap bits per block */
#define BBLOCK(b, sb) ((b) / BPB + (sb).bmapstart)

#define DIRSIZ 28
struct dirent {
    uint32_t inum;        /* 0 = free slot */
    char     name[DIRSIZ];/* NUL-padded; not terminated when exactly DIRSIZ long */
};

_Static_assert(sizeof(struct dinode) == 64, "dinode must be 64 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");
_Static_assert(sizeof(struct log_header) <= BSIZE, "log header must fit in a block");
_Static_assert(BSIZE % sizeof(struct dinode) == 0, "inodes must not straddle blocks");

#endif
