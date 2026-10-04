/* mkfs: build a wren file-system image on the host.
 *
 *   mkfs -o fs.img [-s blocks] [-i inodes] [/path/in/fs=host/file ...]
 *
 * Creates the root directory, /dev/console, and copies each host file to
 * the given path, creating parent directories as needed. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wren/fsformat.h>

static uint8_t *img;
static struct superblock sb;
static uint32_t next_free_block;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "mkfs: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static uint8_t *block(uint32_t b)
{
    if (b >= sb.size) die("block out of range", NULL);
    return img + (size_t)b * BSIZE;
}

static struct dinode *dinode(uint32_t inum) { return (struct dinode *)block(IBLOCK(inum, sb)) + inum % IPB; }

static uint32_t alloc_block(void)
{
    if (next_free_block >= sb.size) die("image full", NULL);
    uint32_t b = next_free_block++;
    block(BBLOCK(b, sb))[(b % BPB) / 8] |= (uint8_t)(1u << (b % 8));
    return b;
}

static uint32_t alloc_inode(uint16_t type)
{
    for (uint32_t i = ROOTINO; i < sb.ninodes; i++)
        if (dinode(i)->type == DI_FREE) {
            memset(dinode(i), 0, sizeof(struct dinode));
            dinode(i)->type = type;
            return i;
        }
    die("out of inodes", NULL);
    return 0;
}

/* Block number holding file block bn, allocating along the way. */
static uint32_t map_block(struct dinode *d, uint32_t bn)
{
    if (bn < NDIRECT) {
        if (!d->addrs[bn]) d->addrs[bn] = alloc_block();
        return d->addrs[bn];
    }
    bn -= NDIRECT;
    uint32_t slot = NDIRECT, levels = 1;
    if (bn >= NINDIRECT) {
        bn -= NINDIRECT;
        slot = NDIRECT + 1;
        levels = 2;
        if (bn >= NINDIRECT * NINDIRECT) die("file too large", NULL);
    }
    if (!d->addrs[slot]) d->addrs[slot] = alloc_block();
    uint32_t ind = d->addrs[slot];
    if (levels == 2) {
        uint32_t *t = (uint32_t *)block(ind);
        if (!t[bn / NINDIRECT]) t[bn / NINDIRECT] = alloc_block();
        ind = t[bn / NINDIRECT];
        bn %= NINDIRECT;
    }
    uint32_t *t = (uint32_t *)block(ind);
    if (!t[bn]) t[bn] = alloc_block();
    return t[bn];
}

static void append(uint32_t inum, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        struct dinode *d = dinode(inum);
        uint32_t off = d->size;
        uint32_t b = map_block(d, off / BSIZE);
        size_t m = BSIZE - off % BSIZE;
        if (m > n) m = n;
        memcpy(block(b) + off % BSIZE, p, m);
        dinode(inum)->size += (uint32_t)m;
        p += m;
        n -= m;
    }
}

static void add_entry(uint32_t dir, const char *name, uint32_t inum)
{
    struct dirent de;
    memset(&de, 0, sizeof de);
    if (strlen(name) > DIRSIZ) die("name too long", name);
    memcpy(de.name, name, strlen(name));
    de.inum = inum;
    append(dir, &de, sizeof de);
    dinode(inum)->nlink++;
}

static uint32_t lookup(uint32_t dir, const char *name)
{
    struct dinode *d = dinode(dir);
    for (uint32_t off = 0; off < d->size; off += sizeof(struct dirent)) {
        struct dirent *de = (struct dirent *)(block(map_block(d, off / BSIZE)) + off % BSIZE);
        if (de->inum && strncmp(de->name, name, DIRSIZ) == 0) return de->inum;
    }
    return 0;
}

static uint32_t make_dir(uint32_t parent)
{
    uint32_t inum = alloc_inode(DI_DIR);
    add_entry(inum, ".", inum);
    add_entry(inum, "..", parent);
    return inum;
}

/* Walk/create the directories of path; return the parent inode and set *leaf. */
static uint32_t make_parents(char *path, char **leaf)
{
    uint32_t dir = ROOTINO;
    char *p = path;
    while (*p == '/') p++;
    for (;;) {
        char *slash = strchr(p, '/');
        if (!slash) break;
        *slash = 0;
        uint32_t next = lookup(dir, p);
        if (!next) {
            next = make_dir(dir);
            add_entry(dir, p, next);
        } else if (dinode(next)->type != DI_DIR) {
            die("not a directory", p);
        }
        dir = next;
        p = slash + 1;
    }
    *leaf = p;
    return dir;
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    uint32_t nblocks = 16384, ninodes = 1024;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (i + 1 >= argc) die("missing value for option", argv[i]);
        if (!strcmp(argv[i], "-o")) out = argv[++i];
        else if (!strcmp(argv[i], "-s")) nblocks = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-i")) ninodes = (uint32_t)strtoul(argv[++i], NULL, 0);
        else die("unknown option", argv[i]);
    }
    if (!out) die("usage: mkfs -o image [-s blocks] [-i inodes] [/path=hostfile ...]", NULL);
    uint16_t probe = 1;
    if (*(uint8_t *)&probe != 1) die("this tool assumes a little-endian host", NULL);

    uint32_t nlog = 1 + LOG_DATA_BLOCKS;
    uint32_t ninodeblocks = (ninodes + IPB - 1) / IPB;
    uint32_t nbitmap = (nblocks + BPB - 1) / BPB;
    sb = (struct superblock){
        .magic = FS_MAGIC, .size = nblocks, .ninodes = ninodes, .nlog = nlog, .logstart = 2,
        .inodestart = 2 + nlog, .bmapstart = 2 + nlog + ninodeblocks,
        .datastart = 2 + nlog + ninodeblocks + nbitmap,
    };
    if (sb.datastart + 16 > nblocks) die("image too small", NULL);
    sb.nblocks = nblocks - sb.datastart;
    img = calloc(nblocks, BSIZE);
    if (!img) die("out of memory", NULL);
    memcpy(block(1), &sb, sizeof sb);
    struct log_header *lh = (struct log_header *)block(sb.logstart);
    lh->magic = LOG_MAGIC;
    for (uint32_t b = 0; b < sb.datastart; b++)   /* metadata blocks are always in use */
        block(BBLOCK(b, sb))[(b % BPB) / 8] |= (uint8_t)(1u << (b % 8));
    next_free_block = sb.datastart;

    uint32_t root = alloc_inode(DI_DIR);
    if (root != ROOTINO) die("root inode is not 1", NULL);
    add_entry(root, ".", root);
    add_entry(root, "..", root);
    uint32_t dev = make_dir(root);
    add_entry(root, "dev", dev);
    uint32_t console = alloc_inode(DI_DEVICE);
    dinode(console)->major = 1;
    add_entry(dev, "console", console);

    for (; i < argc; i++) {
        char *spec = strdup(argv[i]), *eq = strchr(spec, '=');
        if (!eq || spec[0] != '/') die("expected /path=hostfile, got", argv[i]);
        *eq = 0;
        FILE *f = fopen(eq + 1, "rb");
        if (!f) die("cannot open", eq + 1);
        char *leaf;
        uint32_t dir = make_parents(spec, &leaf);
        if (!*leaf || lookup(dir, leaf)) die("bad or duplicate path", argv[i]);
        uint32_t inum = alloc_inode(DI_FILE);
        add_entry(dir, leaf, inum);
        char buf[BSIZE];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) append(inum, buf, n);
        fclose(f);
        free(spec);
    }

    FILE *o = fopen(out, "wb");
    if (!o || fwrite(img, BSIZE, nblocks, o) != nblocks || fclose(o) != 0) die("cannot write", out);
    printf("mkfs: %s: %u blocks (%u data), %u inodes, %u used\n", out, nblocks, sb.nblocks, ninodes,
           next_free_block);
    return 0;
}
