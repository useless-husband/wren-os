/* stress: hammer the kernel from many processes at once and verify every
 * result.  Meant for SMP: forks, exits, pipes, exec, COW faults, heap
 * growth and file writes all race with each other.
 *
 *   stress [seconds [workers [seed]]]
 *
 * Prints "STRESS OK ..." when every worker finished with correct results;
 * any wrong byte, unexpected error or crashed worker prints STRESS FAIL. */
#include "lib/ulib.h"

static uint8_t buf[16384], back[16384];

static void fail(int w, const char *what, long r)
{
    printf("STRESS FAIL worker %d: %s (%ld %s)\n", w, what, r, r < 0 ? strerror((int)r) : "");
    exit(1);
}

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)((i * 2654435761u + seed) >> 9);
}

static void op_file(int w, struct rng *r)
{
    char path[32];
    snprintf(path, sizeof path, "/s%d/f%d", w, (int)rng_range(r, 4));
    uint32_t seed = (uint32_t)rng_next(r);
    size_t n = (size_t)rng_range(r, sizeof buf) + 1;
    int pid = fork();
    if (pid < 0) fail(w, "fork", pid);
    if (pid == 0) {                       /* the child writes, the parent checks */
        fill(buf, n, seed);
        int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
        if (fd < 0 || write_all(fd, buf, n) != (long)n) exit(3);
        close(fd);
        exit(0);
    }
    int st;
    if (waitpid(pid, &st, 0) != pid || st != 0) fail(w, "file child", st);
    int fd = open(path, O_RDONLY);
    if (fd < 0) fail(w, "open", fd);
    long got = read(fd, back, sizeof back);
    close(fd);
    fill(buf, n, seed);
    if (got != (long)n || memcmp(buf, back, n) != 0) fail(w, "file contents", got);
    if (rng_range(r, 2)) {
        long u = unlink(path);
        if (u < 0) fail(w, "unlink", u);
    }
}

/* Byte p of the stream op_pipe sends: written in 1500-byte chunks, chunk at
 * offset s filled with fill(..., seed + s). */
static uint8_t stream_byte(uint32_t seed, size_t p)
{
    size_t s = p - p % 1500;
    uint32_t cs = seed + (uint32_t)s;
    return (uint8_t)(((p - s) * 2654435761u + cs) >> 9);
}

static void op_pipe(int w, struct rng *r)
{
    int to[2], from[2];
    if (pipe(to) < 0 || pipe(from) < 0) fail(w, "pipe", -1);
    size_t n = (size_t)rng_range(r, 60000) + 1;
    int pid = fork();
    if (pid < 0) fail(w, "fork", pid);
    if (pid == 0) {                       /* echo everything back, byte-complemented */
        close(to[1]);
        close(from[0]);
        uint8_t b[700];
        long k;
        while ((k = read(to[0], b, sizeof b)) > 0) {
            for (long i = 0; i < k; i++) b[i] = (uint8_t)~b[i];
            if (write_all(from[1], b, (size_t)k) != k) exit(4);
        }
        exit(0);
    }
    close(to[0]);
    close(from[1]);
    uint32_t seed = (uint32_t)rng_next(r);
    size_t sent = 0, got = 0;
    while (got < n) {                     /* interleave so neither pipe fills up */
        if (sent < n) {
            size_t m = MIN(n - sent, (size_t)1500);
            fill(buf, m, seed + (uint32_t)sent);
            if (write_all(to[1], buf, m) != (long)m) fail(w, "pipe write", -1);
            sent += m;
            if (sent == n) close(to[1]);
        }
        long k = read(from[0], back, MIN(sizeof back, n - got));
        if (k <= 0) fail(w, "pipe read", k);
        for (long i = 0; i < k; i++)
            if (back[i] != (uint8_t)~stream_byte(seed, got + (size_t)i)) fail(w, "pipe byte", (long)(got + (size_t)i));
        got += (size_t)k;
    }
    close(from[0]);
    int st;
    if (waitpid(pid, &st, 0) != pid || st != 0) fail(w, "pipe child", st);
}

static void op_exec(int w, struct rng *r)
{
    (void)r;
    int p[2];
    if (pipe(p) < 0) fail(w, "pipe", -1);
    int pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        char *argv[] = {"echo", "stress", 0};
        exec("/bin/echo", argv);
        exit(5);
    }
    close(p[1]);
    char out[16] = {0};
    long n = 0, k;
    while ((k = read(p[0], out + n, sizeof out - 1 - (size_t)n)) > 0) n += k;
    close(p[0]);
    int st;
    if (waitpid(pid, &st, 0) != pid || st != 0 || strcmp(out, "stress\n") != 0) fail(w, "exec echo", st);
}

static void op_memory(int w, struct rng *r)
{
    long pages = (long)rng_range(r, 64) + 1;
    uint8_t *p = sbrk(pages * 4096);
    if (sbrk_failed(p)) fail(w, "sbrk", (long)p);
    for (long i = 0; i < pages; i++) p[i * 4096] = (uint8_t)(i + w);
    int pid = fork();                     /* COW: both sides write and check */
    if (pid == 0) {
        for (long i = 0; i < pages; i++) p[i * 4096] = (uint8_t)(i * 3);
        for (long i = 0; i < pages; i++)
            if (p[i * 4096] != (uint8_t)(i * 3)) exit(6);
        exit(0);
    }
    for (long i = 0; i < pages; i++) p[i * 4096] ^= 0x5a;
    int st;
    if (waitpid(pid, &st, 0) != pid || st != 0) fail(w, "cow child", st);
    for (long i = 0; i < pages; i++)
        if (p[i * 4096] != (uint8_t)((i + w) ^ 0x5a)) fail(w, "cow parent page", i);
    sbrk(-pages * 4096);
}

static void worker(int w, long until, uint64_t seed)
{
    struct rng r;
    rng_seed(&r, seed * 1000003 + (uint64_t)w);
    char dir[16];
    snprintf(dir, sizeof dir, "/s%d", w);
    long m = mkdir(dir);
    if (m < 0 && m != -EEXIST) fail(w, "mkdir", m);
    long ops = 0;
    while (uptime() < until) {
        switch (rng_range(&r, 4)) {
        case 0: op_file(w, &r); break;
        case 1: op_pipe(w, &r); break;
        case 2: op_exec(w, &r); break;
        default: op_memory(w, &r); break;
        }
        ops++;
    }
    printf("worker %d: %ld operations\n", w, ops);
    exit(0);
}

int main(int argc, char **argv)
{
    long seconds = argc > 1 ? atol(argv[1]) : 10;
    int ncpu = (int)kstat(KSTAT_NCPU);
    int workers = argc > 2 ? atoi(argv[2]) : 3 * ncpu;
    uint64_t seed = argc > 3 ? (uint64_t)atol(argv[3]) : 1;
    long free0 = kstat(KSTAT_FREE_PAGES), t0 = uptime();
    printf("stress: %d workers on %d cpu(s) for %ld s, seed %lu\n", workers, ncpu, seconds, seed);
    for (int w = 0; w < workers; w++) {
        int pid = fork();
        if (pid < 0) {
            printf("STRESS FAIL fork worker: %s\n", strerror(pid));
            exit(1);
        }
        if (pid == 0) worker(w, t0 + seconds * 1000, seed);
    }
    int bad = 0;
    for (int w = 0; w < workers; w++) {
        int st;
        wait(&st);
        if (st != 0) bad++;
    }
    long leaked = free0 - kstat(KSTAT_FREE_PAGES);
    if (bad) printf("STRESS FAIL: %d worker(s) failed\n", bad);
    else printf("STRESS OK: %d workers, %ld ms, %ld context switches, %ld pages not returned\n", workers,
                uptime() - t0, kstat(KSTAT_CTX_SWITCHES), leaked);
    exit(bad != 0);
}
