/* fswork: deterministic file-system workload for the crash tests.
 *
 *   fswork seed [crash_after_writes [torn_sectors [operations]]]
 *
 * Performs a seeded sequence of operations under /w.  Each operation is
 * announced with "OP <k> <description>" before it starts and "OK <k>" after
 * every system call in it has returned, so the test harness knows exactly
 * which operations were durable when the machine lost power and which one
 * was in flight.  With crash_after_writes > 0 the kernel powers off right
 * before that disk write (fault injection, SYS_crashctl).
 *
 * Operation formats (the harness replays them, see tests/crashmodel.py):
 *   OP k create PATH TAG SIZE     open(O_CREATE|O_TRUNC); write SIZE bytes of pattern TAG
 *   OP k append PATH TAG SIZE     open(O_APPEND); write
 *   OP k rewrite PATH TAG SIZE    open; write SIZE bytes at offset 0 (SIZE <= file size)
 *   OP k trunc PATH               open(O_TRUNC)
 *   OP k unlink PATH
 *   OP k link OLD NEW
 *   OP k mkdir PATH
 *   OP k rmdir PATH
 * Every write is at most 32 KiB, so it is a single (atomic) transaction. */
#include "lib/ulib.h"

#define NFILES 24
#define NDIRS 6
#define NOPS 48         /* default number of operations */

static struct rng rng;
static struct { bool used; uint32_t size; int links; } files[NFILES];
static bool dirs[NDIRS];
static uint8_t buf[32768];

static void pattern(uint8_t *p, uint32_t n, uint32_t tag)
{
    for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(((i * 2654435761u) + tag * 40503u) >> 11);
}

static void die(const char *what, long r)
{
    printf("FSWORK ERROR %s: %s\n", what, strerror((int)r));
    exit(1);
}

static void do_write(const char *path, int flags, uint32_t tag, uint32_t size)
{
    int fd = open(path, flags);
    if (fd < 0) die(path, fd);
    pattern(buf, size, tag);
    if (size) {
        long r = write(fd, buf, size);
        if (r != (long)size) die("write", r);
    }
    close(fd);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(2, "usage: fswork seed [crash_after_writes [torn_sectors]]\n");
        exit(2);
    }
    rng_seed(&rng, (uint64_t)atol(argv[1]) * 0x9e3779b97f4a7c15ull + 1);
    long crash_after = argc > 2 ? atol(argv[2]) : 0;
    long torn = argc > 3 ? atol(argv[3]) : 0;
    int nops = argc > 4 ? atoi(argv[4]) : NOPS;
    int r = mkdir("/w");
    if (r < 0 && r != -EEXIST) die("mkdir /w", r);
    long w0 = kstat(KSTAT_DISK_WRITES);
    if (crash_after > 0) crashctl(torn > 0 ? CRASH_TORN : CRASH_AFTER, crash_after, torn);
    printf("FSWORK START seed=%s\n", argv[1]);

    char p1[32], p2[32];
    for (int k = 0; k < nops; k++) {
        int f = (int)rng_range(&rng, NFILES);
        uint32_t tag = (uint32_t)rng_range(&rng, 1000000);
        uint32_t size = (uint32_t)rng_range(&rng, 24 * 1024) + 1;
        int kind = (int)rng_range(&rng, 10);
        snprintf(p1, sizeof p1, "/w/f%d", f);
        if (!files[f].used || kind < 3) {                        /* create (or recreate) */
            if (files[f].used && files[f].links > 1) kind = 9;   /* keep link bookkeeping simple */
            else {
                printf("OP %d create %s %u %u\n", k, p1, tag, size);
                do_write(p1, O_CREATE | O_WRONLY | O_TRUNC, tag, size);
                files[f].used = true;
                files[f].size = size;
                if (!files[f].links) files[f].links = 1;
                printf("OK %d\n", k);
                continue;
            }
        }
        if (kind == 3 || kind == 4) {
            size = size / 3 + 1;
            printf("OP %d append %s %u %u\n", k, p1, tag, size);
            do_write(p1, O_WRONLY | O_APPEND, tag, size);
            files[f].size += size;
        } else if (kind == 5 && files[f].size > 0) {
            size = (uint32_t)rng_range(&rng, files[f].size) + 1;
            if (size > sizeof buf) size = sizeof buf;
            printf("OP %d rewrite %s %u %u\n", k, p1, tag, size);
            do_write(p1, O_WRONLY, tag, size);
        } else if (kind == 6) {
            printf("OP %d trunc %s\n", k, p1);
            int fd = open(p1, O_WRONLY | O_TRUNC);
            if (fd < 0) die("trunc", fd);
            close(fd);
            files[f].size = 0;
        } else if (kind == 7 && files[f].links == 1) {
            printf("OP %d unlink %s\n", k, p1);
            if ((r = unlink(p1)) < 0) die("unlink", r);
            files[f].used = false;
            files[f].links = 0;
        } else if (kind == 8) {
            int d = (int)rng_range(&rng, NDIRS);
            snprintf(p2, sizeof p2, "/w/d%d", d);
            printf("OP %d %s %s\n", k, dirs[d] ? "rmdir" : "mkdir", p2);
            r = dirs[d] ? unlink(p2) : mkdir(p2);
            if (r < 0) die("mkdir/rmdir", r);
            dirs[d] = !dirs[d];
        } else {
            int g = (int)rng_range(&rng, NFILES);
            if (files[g].used || g == f) {                       /* need a free name */
                printf("OP %d trunc %s\n", k, p1);
                int fd = open(p1, O_WRONLY | O_TRUNC);
                if (fd < 0) die("trunc", fd);
                close(fd);
                files[f].size = 0;
            } else {
                snprintf(p2, sizeof p2, "/w/f%d", g);
                printf("OP %d link %s %s\n", k, p1, p2);
                if ((r = link(p1, p2)) < 0) die("link", r);
                /* Both names now refer to one inode.  To keep the model
                 * simple, the new name is never written or unlinked again. */
                files[g].used = true;
                files[g].links = 2;
                files[f].links = 2;
            }
        }
        printf("OK %d\n", k);
    }
    printf("FSWORK DONE writes=%ld\n", kstat(KSTAT_DISK_WRITES) - w0);
    exit(0);
}
