/* fsck: check a wren file-system image on the host.
 *
 *   fsck [--dump] image
 *
 * If the log holds a committed transaction, fsck first applies it in memory
 * (the same redo rule the kernel uses at mount), so a crashed image is
 * checked in the state the kernel would recover it to.  It then verifies:
 *   - superblock geometry
 *   - every allocated inode: valid type, block pointers inside the data
 *     area, no block owned twice, no blocks past end of file
 *   - the directory tree from the root: "." and "..", entries name
 *     allocated inodes, every directory reachable exactly once
 *   - link counts equal the number of directory entries naming each inode
 *   - the free bitmap marks exactly the metadata and the owned blocks
 * --dump prints "path type size crc32c" for every file and directory, which
 * the crash tests compare with their model of the committed operations.
 * Exit status: 0 consistent, 1 inconsistent, 2 usage or I/O error. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wren/fsformat.h>
#include "../lib/crc32c.h"

static uint8_t *img;
static size_t img_blocks;
static struct superblock sb;
static int errors;
static uint32_t *owner;      /* inode owning each block (0 = none) */
static uint32_t *refs;       /* directory entries naming each inode */
static uint8_t *visited;     /* directory reached from the root */

#define ERR(...) do { errors++; if (errors <= 50) { fprintf(stderr, "fsck: "); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint8_t *block(uint32_t b) { return img + (size_t)b * BSIZE; }
static struct dinode *dinode(uint32_t i) { return (struct dinode *)block(IBLOCK(i, sb)) + i % IPB; }

static bool bit(uint32_t b) { return block(BBLOCK(b, sb))[(b % BPB) / 8] & (1u << (b % 8)); }

static int replay_log(void)
{
    struct log_header h;
    memcpy(&h, block(sb.logstart), sizeof h);
    if (h.magic != LOG_MAGIC) {
        ERR("log header has bad magic %08x", h.magic);
        return 0;
    }
    if (h.n == 0) return 0;
    if (h.n > LOG_DATA_BLOCKS || h.n >= sb.nlog) {
        printf("log: header claims %u blocks: not a committed transaction\n", h.n);
        return 0;
    }
    uint32_t crc = crc32c_update(0, &h.magic, sizeof h.magic);
    crc = crc32c_update(crc, &h.n, sizeof h.n);
    crc = crc32c_update(crc, &h.seq, sizeof h.seq);
    crc = crc32c_update(crc, h.block, h.n * sizeof h.block[0]);
    for (uint32_t i = 0; i < h.n; i++) crc = crc32c_update(crc, block(sb.logstart + 1 + i), BSIZE);
    bool in_range = true;
    for (uint32_t i = 0; i < h.n; i++) in_range &= h.block[i] >= sb.logstart + sb.nlog && h.block[i] < sb.size;
    if (crc != h.crc || !in_range) {
        printf("log: transaction %llu (%u blocks) is not committed (checksum mismatch): ignored\n",
               (unsigned long long)h.seq, h.n);
        return 0;
    }
    for (uint32_t i = 0; i < h.n; i++) memcpy(block(h.block[i]), block(sb.logstart + 1 + i), BSIZE);
    printf("log: replayed committed transaction %llu (%u blocks)\n", (unsigned long long)h.seq, h.n);
    return 1;
}

static bool data_block_ok(uint32_t b) { return b >= sb.datastart && b < sb.size; }

static void claim(uint32_t b, uint32_t inum, const char *what)
{
    if (!data_block_ok(b)) {
        ERR("inode %u: %s block %u outside the data area", inum, what, b);
        return;
    }
    if (owner[b]) ERR("block %u owned by inode %u and inode %u", b, owner[b], inum);
    else owner[b] = inum;
}

/* Visit every block of an inode in file order: fn(file_block_index, disk_block). */
typedef void (*block_fn)(uint32_t inum, uint32_t fbn, uint32_t b, void *ctx);

static void walk_blocks(uint32_t inum, block_fn fn, void *ctx, bool claim_meta)
{
    struct dinode *d = dinode(inum);
    for (uint32_t i = 0; i < NDIRECT; i++)
        if (d->addrs[i]) fn(inum, i, d->addrs[i], ctx);
    uint32_t ind = d->addrs[NDIRECT];
    if (ind) {
        if (claim_meta) claim(ind, inum, "indirect");
        if (data_block_ok(ind)) {
            const uint32_t *t = (const uint32_t *)block(ind);
            for (uint32_t i = 0; i < NINDIRECT; i++)
                if (t[i]) fn(inum, NDIRECT + i, t[i], ctx);
        }
    }
    uint32_t dind = d->addrs[NDIRECT + 1];
    if (dind) {
        if (claim_meta) claim(dind, inum, "double-indirect");
        if (data_block_ok(dind)) {
            const uint32_t *t = (const uint32_t *)block(dind);
            for (uint32_t i = 0; i < NINDIRECT; i++) {
                if (!t[i]) continue;
                if (claim_meta) claim(t[i], inum, "indirect");
                if (!data_block_ok(t[i])) continue;
                const uint32_t *u = (const uint32_t *)block(t[i]);
                for (uint32_t j = 0; j < NINDIRECT; j++)
                    if (u[j]) fn(inum, NDIRECT + NINDIRECT + i * NINDIRECT + j, u[j], ctx);
            }
        }
    }
}

static void check_data_block(uint32_t inum, uint32_t fbn, uint32_t b, void *ctx)
{
    (void)ctx;
    claim(b, inum, "data");
    uint32_t size = dinode(inum)->size;
    if ((uint64_t)fbn * BSIZE >= size) ERR("inode %u: block %u (file block %u) lies past its size %u", inum, b, fbn, size);
}

struct reader {
    uint8_t *buf;
    uint32_t size;
};

static void read_block_into(uint32_t inum, uint32_t fbn, uint32_t b, void *ctx)
{
    (void)inum;
    struct reader *r = ctx;
    if (!data_block_ok(b)) return;
    uint64_t off = (uint64_t)fbn * BSIZE;
    if (off >= r->size) return;
    uint32_t n = r->size - off < BSIZE ? (uint32_t)(r->size - off) : BSIZE;
    memcpy(r->buf + off, block(b), n);
}

/* The whole file in a malloc'd buffer (holes read as zeros). */
static uint8_t *read_file(uint32_t inum, uint32_t *size)
{
    struct reader r = {.size = dinode(inum)->size};
    r.buf = calloc(1, r.size ? r.size : 1);
    walk_blocks(inum, read_block_into, &r, false);
    *size = r.size;
    return r.buf;
}

static bool dump;

static void check_dir(uint32_t inum, uint32_t parent, const char *path, int depth)
{
    if (depth > 64) {
        ERR("%s: directory tree too deep (cycle?)", path);
        return;
    }
    if (visited[inum]) {
        ERR("%s: directory inode %u reachable twice", path, inum);
        return;
    }
    visited[inum] = 1;
    uint32_t size;
    uint8_t *data = read_file(inum, &size);
    if (size % sizeof(struct dirent)) ERR("%s: directory size %u not a multiple of %zu", path, size, sizeof(struct dirent));
    if (dump) printf("%s dir %u\n", path, size);
    bool dot = false, dotdot = false;
    for (uint32_t off = 0; off + sizeof(struct dirent) <= size; off += sizeof(struct dirent)) {
        struct dirent *de = (struct dirent *)(data + off);
        if (!de->inum) continue;
        char name[DIRSIZ + 1];
        memcpy(name, de->name, DIRSIZ);
        name[DIRSIZ] = 0;
        if (!name[0] || strchr(name, '/')) {
            ERR("%s: bad entry name at offset %u", path, off);
            continue;
        }
        if (de->inum >= sb.ninodes || dinode(de->inum)->type == DI_FREE) {
            ERR("%s/%s: names free or invalid inode %u", path, name, de->inum);
            continue;
        }
        refs[de->inum]++;
        if (!strcmp(name, ".")) {
            if (de->inum != inum) ERR("%s: \".\" is inode %u, expected %u", path, de->inum, inum);
            dot = true;
            continue;
        }
        if (!strcmp(name, "..")) {
            if (de->inum != parent) ERR("%s: \"..\" is inode %u, expected %u", path, de->inum, parent);
            dotdot = true;
            continue;
        }
        char child[1024];
        snprintf(child, sizeof child, "%s/%s", strcmp(path, "/") ? path : "", name);
        struct dinode *cd = dinode(de->inum);
        if (cd->type == DI_DIR) {
            check_dir(de->inum, inum, child, depth + 1);
        } else if (dump) {
            uint32_t fsz;
            uint8_t *fdata = read_file(de->inum, &fsz);
            printf("%s %s %u %08x\n", child, cd->type == DI_FILE ? "file" : "dev", fsz, crc32c_update(0, fdata, fsz));
            free(fdata);
        }
    }
    if (!dot || !dotdot) ERR("%s: missing \".\" or \"..\"", path);
    free(data);
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "--dump")) {
        dump = true;
        i++;
    }
    if (i + 1 != argc) {
        fprintf(stderr, "usage: fsck [--dump] image\n");
        return 2;
    }
    FILE *f = fopen(argv[i], "rb");
    if (!f) {
        perror(argv[i]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 2 * BSIZE || len % BSIZE) {
        fprintf(stderr, "fsck: %s: size %ld is not a whole number of blocks\n", argv[i], len);
        return 2;
    }
    img_blocks = (size_t)len / BSIZE;
    img = malloc((size_t)len);
    if (!img || fread(img, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "fsck: read error\n");
        return 2;
    }
    fclose(f);

    memcpy(&sb, block(1), sizeof sb);
    if (sb.magic != FS_MAGIC) {
        fprintf(stderr, "fsck: bad superblock magic\n");
        return 1;
    }
    uint32_t ninodeblocks = (sb.ninodes + IPB - 1) / IPB, nbitmap = (sb.size + BPB - 1) / BPB;
    if (sb.size > img_blocks || sb.logstart != 2 || sb.inodestart != sb.logstart + sb.nlog ||
        sb.bmapstart != sb.inodestart + ninodeblocks || sb.datastart != sb.bmapstart + nbitmap ||
        sb.nblocks != sb.size - sb.datastart || sb.nlog < 2) {
        fprintf(stderr, "fsck: superblock geometry is inconsistent\n");
        return 1;
    }
    replay_log();

    owner = calloc(sb.size, sizeof *owner);
    refs = calloc(sb.ninodes, sizeof *refs);
    visited = calloc(sb.ninodes, 1);
    uint32_t used_inodes = 0;
    for (uint32_t in = 1; in < sb.ninodes; in++) {
        struct dinode *d = dinode(in);
        if (d->type == DI_FREE) continue;
        used_inodes++;
        if (d->type > DI_DEVICE) {
            ERR("inode %u: bad type %u", in, d->type);
            continue;
        }
        if ((uint64_t)d->size > (uint64_t)MAXFILE_BLOCKS * BSIZE) ERR("inode %u: size %u too large", in, d->size);
        walk_blocks(in, check_data_block, NULL, true);
    }
    if (dinode(ROOTINO)->type != DI_DIR) {
        ERR("root inode is not a directory");
    } else {
        check_dir(ROOTINO, ROOTINO, "/", 0);
    }
    for (uint32_t in = 1; in < sb.ninodes; in++) {
        struct dinode *d = dinode(in);
        if (d->type == DI_FREE) continue;
        if (d->type == DI_DIR && !visited[in] && d->nlink) ERR("directory inode %u is not reachable from /", in);
        if (refs[in] != d->nlink) {
            if (refs[in] == 0 && d->nlink == 0)
                printf("warning: inode %u is an orphan (unlinked while open at the crash)\n", in);
            else
                ERR("inode %u: link count %u but %u directory entries", in, d->nlink, refs[in]);
        }
    }
    uint32_t used_blocks = 0;
    for (uint32_t b = 0; b < sb.size; b++) {
        bool in_use = b < sb.datastart || owner[b];
        if (bit(b) != in_use)
            ERR("block %u: bitmap says %s but it is %s", b, bit(b) ? "used" : "free", in_use ? "in use" : "unreferenced");
        used_blocks += bit(b);
    }
    if (errors) {
        fprintf(stderr, "fsck: %d inconsistencies\n", errors);
        return 1;
    }
    fprintf(dump ? stderr : stdout, "fsck: clean, %u/%u inodes, %u/%u blocks in use\n", used_inodes, sb.ninodes,
            used_blocks, sb.size);
    return 0;
}
