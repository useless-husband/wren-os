/* bench: micro-benchmarks of the kernel's basic operations.
 * Prints "BENCH <name> <value> <unit>" lines; the timer is CNTVCT_EL0. */
#include "lib/ulib.h"

static double ns_since(uint64_t t0) { return (double)(cycles() - t0) * 1e9 / (double)cycles_freq(); }

static void report(const char *name, double value, const char *unit)
{
    long whole = (long)value, frac = (long)((value - (double)whole) * 10);
    printf("BENCH %-22s %ld.%ld %s\n", name, whole, frac < 0 ? -frac : frac, unit);
}

static void bench_syscall(void)
{
    enum { N = 200000 };
    uint64_t t0 = cycles();
    for (int i = 0; i < N; i++) getpid();
    report("syscall_getpid", ns_since(t0) / N, "ns");
}

static void bench_ctxswitch(void)
{
    enum { N = 20000 };
    int a[2], b[2];
    pipe(a);
    pipe(b);
    int pid = fork();
    char c = 0;
    if (pid == 0) {
        for (int i = 0; i < N; i++) {
            read(a[0], &c, 1);
            write(b[1], &c, 1);
        }
        exit(0);
    }
    uint64_t t0 = cycles();
    for (int i = 0; i < N; i++) {
        write(a[1], &c, 1);
        read(b[0], &c, 1);
    }
    double ns = ns_since(t0);
    wait(0);
    report("pipe_roundtrip", ns / N, "ns");
    report("ctx_switch_via_pipe", ns / N / 2, "ns");
}

static void bench_fork(long heap_pages, const char *name)
{
    enum { N = 300 };
    char *p = 0;
    if (heap_pages) {
        p = sbrk(heap_pages * 4096);
        for (long i = 0; i < heap_pages; i++) p[i * 4096] = 1;
    }
    uint64_t t0 = cycles();
    for (int i = 0; i < N; i++) {
        int pid = fork();
        if (pid == 0) exit(0);
        waitpid(pid, 0, 0);
    }
    report(name, ns_since(t0) / N / 1000, "us");
    if (heap_pages) sbrk(-heap_pages * 4096);
}

static void bench_exec(void)
{
    enum { N = 100 };
    uint64_t t0 = cycles();
    for (int i = 0; i < N; i++) {
        int pid = fork();
        if (pid == 0) {
            char *argv[] = {"sleep", "0", 0};
            exec("/bin/sleep", argv);
            exit(1);
        }
        waitpid(pid, 0, 0);
    }
    report("fork_exec_wait", ns_since(t0) / N / 1000, "us");
}

static void bench_faults(void)
{
    enum { PAGES = 4096 };                      /* 16 MiB */
    char *p = sbrk(PAGES * 4096L);
    uint64_t t0 = cycles();
    for (long i = 0; i < PAGES; i++) p[i * 4096] = 1;
    report("lazy_page_fault", ns_since(t0) / PAGES, "ns");
    int pid = fork();
    if (pid == 0) {
        uint64_t t1 = cycles();
        for (long i = 0; i < PAGES; i++) p[i * 4096] = 2;
        report("cow_page_fault", ns_since(t1) / PAGES, "ns");
        exit(0);
    }
    wait(0);
    sbrk(-PAGES * 4096L);
}

static void bench_fs(void)
{
    static char buf[32768];
    const long total = 8L << 20;
    memset(buf, 'x', sizeof buf);
    unlink("/benchfile");
    long w0 = kstat(KSTAT_DISK_WRITES), c0 = kstat(KSTAT_LOG_COMMITS);
    int fd = open("/benchfile", O_CREATE | O_WRONLY | O_TRUNC);
    uint64_t t0 = cycles();
    for (long off = 0; off < total; off += sizeof buf) write(fd, buf, sizeof buf);
    close(fd);
    double ns = ns_since(t0);
    report("fs_write_8MiB", (double)total / (1 << 20) / (ns / 1e9), "MiB/s");
    report("fs_write_disk_writes", (double)(kstat(KSTAT_DISK_WRITES) - w0), "blocks");
    report("fs_write_commits", (double)(kstat(KSTAT_LOG_COMMITS) - c0), "txns");
    fd = open("/benchfile", O_RDONLY);
    t0 = cycles();
    long n, got = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) got += n;
    close(fd);
    report("fs_read_8MiB", (double)got / (1 << 20) / (ns_since(t0) / 1e9), "MiB/s");
    t0 = cycles();
    enum { SMALL = 200 };
    char name[32];
    for (int i = 0; i < SMALL; i++) {
        snprintf(name, sizeof name, "/bs%d", i);
        fd = open(name, O_CREATE | O_WRONLY);
        write(fd, buf, 100);
        close(fd);
    }
    for (int i = 0; i < SMALL; i++) {
        snprintf(name, sizeof name, "/bs%d", i);
        unlink(name);
    }
    report("fs_create_write_unlink", ns_since(t0) / SMALL / 1000, "us");
    unlink("/benchfile");
}

int main(int argc, char **argv)
{
    printf("bench: %ld cpu(s), counter %lu Hz\n", kstat(KSTAT_NCPU), cycles_freq());
    bool all = argc < 2;
    for (int i = 1; i < argc || all; i++) {
        const char *w = all ? "all" : argv[i];
        bool any = strcmp(w, "all") == 0;
        if (any || !strcmp(w, "syscall")) bench_syscall();
        if (any || !strcmp(w, "ctx")) bench_ctxswitch();
        if (any || !strcmp(w, "fork")) {
            bench_fork(0, "fork_exit_wait");
            bench_fork(1024, "fork_4MiB_heap");
        }
        if (any || !strcmp(w, "exec")) bench_exec();
        if (any || !strcmp(w, "fault")) bench_faults();
        if (any || !strcmp(w, "fs")) bench_fs();
        if (all) break;
    }
    printf("BENCH done\n");
    exit(0);
}
