/* usertests: the kernel's system-level test suite, run inside wren-os.
 *
 *   usertests            run everything
 *   usertests -q         skip the slow tests
 *   usertests name...    run only the named tests
 *
 * Every test runs in its own child process, so a test that crashes or
 * hangs on a bug cannot take the others down.  A test fails by calling
 * fail() (exit status 1) or by being killed by the kernel (status -1). */
#include "lib/ulib.h"
#include <wren/fsformat.h>

#define NOFILE_GUESS 64

static int ncpu;
static char scratch[256];

#define fail(...) do { \
        fprintf(2, "    FAIL %s:%d: ", __FILE__, __LINE__); fprintf(2, __VA_ARGS__); fprintf(2, "\n"); exit(1); \
    } while (0)
#define check(c) do { if (!(c)) fail("check failed: %s", #c); } while (0)
#define check_eq(a, b) do { long a_ = (long)(a), b_ = (long)(b); \
        if (a_ != b_) fail("%s == %ld, expected %s == %ld", #a, a_, #b, b_); } while (0)

/* Run fn in a child and return its exit status. */
static int child_status(void (*fn)(void))
{
    int pid = fork();
    if (pid < 0) fail("fork: %s", strerror(pid));
    if (pid == 0) {
        fn();
        exit(0);
    }
    int st = 0;
    check_eq(waitpid(pid, &st, 0), pid);
    return st;
}

static void expect_killed(void (*fn)(void), const char *what)
{
    int st = child_status(fn);
    if (st != -1) fail("%s: child exited with %d, expected to be killed", what, st);
}

static void write_file(const char *path, const void *data, size_t n)
{
    int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
    if (fd < 0) fail("open %s: %s", path, strerror(fd));
    check_eq(write_all(fd, data, n), (long)n);
    close(fd);
}

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)((i * 2654435761u + seed) >> 13);
}

/* ================================================================ processes */

static void t_fork_wait(void)
{
    int pids[10];
    for (int i = 0; i < 10; i++) {
        pids[i] = fork();
        check(pids[i] >= 0);
        if (pids[i] == 0) exit(i + 3);
    }
    int seen = 0;
    for (int i = 0; i < 10; i++) {
        int st, pid = wait(&st);
        check(pid > 0);
        for (int k = 0; k < 10; k++)
            if (pids[k] == pid) {
                check_eq(st, k + 3);
                seen |= 1 << k;
            }
    }
    check_eq(seen, 0x3ff);
    check_eq(wait(0), -ECHILD);
}

static void t_fork_exhaust(void)
{
    int n = 0;
    for (;; n++) {
        int pid = fork();
        if (pid < 0) {
            check_eq(pid, -EAGAIN);
            break;
        }
        if (pid == 0) {
            sleep(200);
            exit(0);
        }
        if (n > 200) fail("process table never filled");
    }
    check(n > 10);
    for (int i = 0; i < n; i++) check(wait(0) > 0);
    check_eq(wait(0), -ECHILD);
    int pid = fork();                     /* slots were really freed */
    check(pid >= 0);
    if (pid == 0) exit(0);
    check_eq(wait(0), pid);
}

static void t_orphans(void)
{
    int pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 5; i++)
            if (fork() == 0) {
                sleep(50);
                exit(0);
            }
        exit(7);                          /* grandchildren are re-parented to init */
    }
    int st;
    check_eq(waitpid(pid, &st, 0), pid);
    check_eq(st, 7);
    check_eq(wait(0), -ECHILD);
}

static void t_wait_nohang(void)
{
    check_eq(waitpid(-1, 0, WNOHANG), -ECHILD);
    int pid = fork();
    if (pid == 0) {
        sleep(300);
        exit(4);
    }
    check_eq(waitpid(pid, 0, WNOHANG), 0);
    int st;
    check_eq(waitpid(pid, &st, 0), pid);
    check_eq(st, 4);
}

static void t_getppid(void)
{
    int me = getpid();
    int fds[2];
    check_eq(pipe(fds), 0);
    int pid = fork();
    if (pid == 0) {
        int pp = getppid();
        write(fds[1], &pp, sizeof pp);
        exit(0);
    }
    int pp = 0;
    check_eq(read(fds[0], &pp, sizeof pp), sizeof pp);
    check_eq(pp, me);
    wait(0);
}

static void sleeper(void) { sleep(1000000); }
static void spinner(void) { for (volatile long i = 0;; i++) {} }

static void t_kill(void)
{
    void (*victims[])(void) = {sleeper, spinner};
    for (int v = 0; v < 2; v++) {
        int pid = fork();
        if (pid == 0) {
            victims[v]();
            exit(0);
        }
        sleep(30);
        check_eq(kill(pid), 0);
        int st;
        check_eq(waitpid(pid, &st, 0), pid);
        check_eq(st, -1);
    }
    check_eq(kill(99999), -ESRCH);
}

static void t_kill_pipe_reader(void)
{
    int fds[2];
    check_eq(pipe(fds), 0);
    int pid = fork();
    if (pid == 0) {
        char c;
        read(fds[0], &c, 1);              /* blocks forever: we keep the write end open */
        exit(0);
    }
    sleep(30);
    check_eq(kill(pid), 0);
    int st;
    check_eq(waitpid(pid, &st, 0), pid);
    check_eq(st, -1);
}

static void t_preempt(void)
{
    /* A child that never makes a system call must not stop us from running,
     * even on one CPU: the timer interrupt preempts it. */
    int pid = fork();
    if (pid == 0) spinner();
    long t0 = uptime();
    sleep(50);
    check(uptime() - t0 < 5000);
    kill(pid);
    wait(0);
}

static void t_sleep_time(void)
{
    long t0 = uptime();
    check_eq(sleep(120), 0);
    long dt = uptime() - t0;
    if (dt < 120 || dt > 3000) fail("sleep(120) took %ld ms", dt);
}

/* ===================================================================== exec */

static void t_exec_args(void)
{
    int fds[2];
    check_eq(pipe(fds), 0);
    int pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        char *argv[] = {"echo", "alpha", "beta gamma", "", "z", 0};
        exec("/bin/echo", argv);
        exit(99);
    }
    close(fds[1]);
    char buf[64] = {0};
    long n = 0, r;
    while ((r = read(fds[0], buf + n, sizeof buf - 1 - (size_t)n)) > 0) n += r;
    close(fds[0]);
    int st;
    wait(&st);
    check_eq(st, 0);
    if (strcmp(buf, "alpha beta gamma  z\n") != 0) fail("echo printed '%s'", buf);
}

static void t_exec_errors(void)
{
    char *argv[] = {"x", 0};
    check_eq(exec("/no/such/file", argv), -ENOENT);
    check_eq(exec("/motd.txt", argv), -ENOEXEC);          /* not an ELF file */
    check_eq(exec("/bin", argv), -EACCES);                /* a directory */
    check_eq(exec("/bin/echo", (char **)0x10), -EFAULT);  /* argv in unmapped memory */
    char *bad[] = {"echo", (char *)0xffff000040000000UL, 0};
    check_eq(exec("/bin/echo", bad), -EFAULT);           /* kernel address as a string */
    /* still alive and intact after all of that */
    check_eq(getpid() > 0, 1);
}

static void t_exec_toobig_argv(void)
{
    static char *argv[40];
    for (int i = 0; i < 39; i++) argv[i] = "a";
    argv[39] = 0;
    check_eq(exec("/bin/echo", argv), -E2BIG);
}

/* =================================================================== memory */

static void t_sbrk_basic(void)
{
    char *a = sbrk(0);
    char *b = sbrk(3 * 4096 + 100);
    check(a == b);
    check(sbrk(0) == a + 3 * 4096 + 100);
    for (int i = 0; i < 3 * 4096 + 100; i++) a[i] = (char)i;
    for (int i = 0; i < 3 * 4096 + 100; i++) check(a[i] == (char)i);
    check(sbrk(-(3 * 4096 + 100)) == a + 3 * 4096 + 100);
    check(sbrk(0) == a);
    check(sbrk_failed(sbrk(-(1L << 40))));                   /* below the heap start */
}

static char *freed;
static void touch_freed(void) { freed[0] = 1; }

static void t_sbrk_shrink_unmaps(void)
{
    freed = sbrk(8192);
    freed[0] = 1;
    sbrk(-8192);
    expect_killed(touch_freed, "access below the shrunk break");
}

static void t_sbrk_lazy(void)
{
    long before = kstat(KSTAT_FREE_PAGES);
    long size = 256L << 20;                                 /* bigger than RAM on small configs */
    char *p = sbrk(size);
    check(!sbrk_failed(p));
    long after_sbrk = kstat(KSTAT_FREE_PAGES);
    check(before - after_sbrk < 4);                         /* nothing allocated yet */
    for (long off = 0; off < size; off += 16L << 20) p[off] = 1;   /* touch 16 pages */
    long touched = after_sbrk - kstat(KSTAT_FREE_PAGES);
    if (touched < 16 || touched > 64) fail("touching 16 pages used %ld pages", touched);
    check_eq(p[16L << 20], 1);
    check_eq(p[4096], 0);                                   /* untouched memory reads as zero */
    sbrk(-size);
}

static void oom_child(void)
{
    for (;;) {
        char *p = sbrk(1 << 20);
        if (sbrk_failed(p)) exit(2);
        for (int i = 0; i < (1 << 20); i += 4096) p[i] = 1;   /* the kernel kills us when RAM runs out */
    }
}

static void t_out_of_memory(void)
{
    long before = kstat(KSTAT_FREE_PAGES);
    int st = child_status(oom_child);
    if (st != -1 && st != 2) fail("oom child status %d", st);
    long after = kstat(KSTAT_FREE_PAGES);
    if (before - after > 64) fail("%ld pages not returned after the oom child died", before - after);
}

static void t_cow_isolation(void)
{
    enum { N = 6 * 4096 };
    uint8_t *heap = malloc(N);
    uint8_t stack_buf[3000];
    fill(heap, N, 1);
    fill(stack_buf, sizeof stack_buf, 2);
    int fds[2];
    check_eq(pipe(fds), 0);
    int pid = fork();
    if (pid == 0) {
        char go;
        read(fds[0], &go, 1);                 /* wait until the parent has written */
        uint8_t ref[N];
        fill(ref, N, 1);
        check(memcmp(heap, ref, N) == 0);     /* parent's later writes are invisible here */
        fill(ref, sizeof stack_buf, 2);
        check(memcmp(stack_buf, ref, sizeof stack_buf) == 0);
        fill(heap, N, 3);                     /* and ours are invisible there */
        exit(0);
    }
    /* Write right after fork: if the kernel forgot to flush our TLB when it
     * made these pages copy-on-write, these stores would land in the page
     * the child shares. */
    fill(heap, N, 4);
    fill(stack_buf, sizeof stack_buf, 5);
    write(fds[1], "g", 1);
    int st;
    check_eq(waitpid(pid, &st, 0), pid);
    check_eq(st, 0);
    uint8_t ref[N];
    fill(ref, N, 4);
    check(memcmp(heap, ref, N) == 0);
    free(heap);
}

static void t_cow_sharing(void)
{
    enum { PAGES = 256 };                     /* 1 MiB, touched */
    char *p = sbrk(PAGES * 4096);
    for (int i = 0; i < PAGES; i++) p[i * 4096] = (char)i;
    long free0 = kstat(KSTAT_FREE_PAGES), cow0 = kstat(KSTAT_COW_FAULTS);
    int pids[8];
    int fds[2];
    check_eq(pipe(fds), 0);
    for (int k = 0; k < 8; k++) {
        pids[k] = fork();
        if (pids[k] == 0) {
            char c;
            read(fds[0], &c, 1);
            for (int i = 0; i < 16; i++) p[i * 4096] = 'w';    /* copy 16 pages */
            exit(p[20 * 4096] == 20 ? 0 : 1);
        }
    }
    long used = free0 - kstat(KSTAT_FREE_PAGES);
    /* 8 children share the 256 pages: each costs page tables and a kernel
     * stack, nowhere near 256 pages. */
    if (used > 8 * 40) fail("8 forks of a 1 MiB process used %ld pages", used);
    write(fds[1], "gggggggg", 8);
    for (int k = 0; k < 8; k++) {
        int st;
        waitpid(pids[k], &st, 0);
        check_eq(st, 0);
    }
    check(kstat(KSTAT_COW_FAULTS) - cow0 >= 8 * 16);
    sbrk(-PAGES * 4096);
}

/* The asm barrier keeps pad live across the call, so the compiler cannot
 * turn the recursion into a loop and the stack really grows. */
__attribute__((noinline)) static int recurse(int n)
{
    volatile char pad[1024];
    pad[0] = (char)n;
    int r = n == 0 ? 0 : recurse(n - 1);
    __asm__ volatile("" ::"r"(pad) : "memory");
    return r + pad[0];
}

static void t_stack_growth(void)
{
    int expect = 0;
    for (int n = 1; n <= 2000; n++) expect += (char)n;
    check_eq(recurse(2000), expect);          /* ~2 MiB of stack, grown on demand */
}

static void deep(void) { recurse(20000); }   /* ~20 MiB: past the 8 MiB stack limit */
static void t_stack_overflow(void) { expect_killed(deep, "stack overflow"); }

static void null_deref(void) { *(volatile int *)0 = 1; }
static void write_text(void) { *(volatile uint32_t *)(void *)t_fork_wait = 0; }
static void exec_heap(void)
{
    uint32_t *code = malloc(16);
    code[0] = 0xd65f03c0;                     /* ret */
    ((void (*)(void))code)();
}
static void read_kernel(void) { (void)*(volatile long *)0xffff000040000000UL; }
static void bad_instruction(void) { __asm__ volatile(".word 0x00000000"); }

static void t_faults(void)
{
    expect_killed(null_deref, "null pointer write");
    expect_killed(write_text, "write to program text");
    expect_killed(exec_heap, "execute from the heap");
    expect_killed(read_kernel, "read kernel memory");
    expect_killed(bad_instruction, "undefined instruction");
}

static void t_bad_pointers(void)
{
    char *kernel = (char *)0xffff000040000000UL;
    check_eq(write(1, kernel, 10), -EFAULT);
    int fd = open("/motd.txt", O_RDONLY);
    check_eq(read(fd, kernel, 10), -EFAULT);
    check_eq(read(fd, (void *)0x8, 10), -EFAULT);
    close(fd);
    check_eq(open((char *)0x10, O_RDONLY), -EFAULT);
    check_eq(pipe((int *)kernel), -EFAULT);
    check_eq(fstat(0, (struct stat *)kernel), -EFAULT);
    check_eq(waitpid(-1, (int *)kernel, WNOHANG), -ECHILD);
}

static void t_bad_syscall(void)
{
    register long x8 __asm__("x8") = 999;
    register long x0 __asm__("x0") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    check_eq(x0, -ENOSYS);
}

static void t_memory_leaks(void)
{
    /* fork + exec + pipe + exit, many times; all memory must come back. */
    long before = kstat(KSTAT_FREE_PAGES);
    for (int i = 0; i < 20; i++) {
        int fds[2];
        check_eq(pipe(fds), 0);
        int pid = fork();
        if (pid == 0) {
            dup2(fds[1], 1);
            close(fds[0]);
            close(fds[1]);
            char *argv[] = {"echo", "leak", 0};
            exec("/bin/echo", argv);
            exit(1);
        }
        close(fds[1]);
        char buf[16];
        while (read(fds[0], buf, sizeof buf) > 0) {}
        close(fds[0]);
        wait(0);
    }
    long lost = before - kstat(KSTAT_FREE_PAGES);
    if (lost > 8) fail("%ld pages lost over 20 fork/exec/pipe rounds", lost);
}

/* ====================================================== FP state and SMP */

static void fp_worker(int seed, int fd)
{
    double x = seed, acc = 0;
    for (int round = 0; round < 200; round++) {
        double expect = 0, y = x;
        for (int i = 0; i < 2000; i++) {
            y = y * 1.0000001 + 0.5;
            expect += y / (i + 1.0);
        }
        /* recompute with a yield in the middle: another process runs on this
         * CPU and uses the same FP registers */
        double z = x, got = 0;
        for (int i = 0; i < 2000; i++) {
            z = z * 1.0000001 + 0.5;
            got += z / (i + 1.0);
            if (i == 1000 && round % 8 == 0) sleep(1);
        }
        if (got != expect) {
            write(fd, "X", 1);
            exit(1);
        }
        acc += got;
        x += 1;
    }
    exit(acc > 0 ? 0 : 1);
}

static void t_fp_state(void)
{
    int pids[6];
    for (int k = 0; k < 6; k++) {
        pids[k] = fork();
        if (pids[k] == 0) fp_worker(k * 1000 + 1, 2);
    }
    for (int k = 0; k < 6; k++) {
        int st;
        waitpid(pids[k], &st, 0);
        if (st != 0) fail("FP worker %d saw corrupted registers", k);
    }
}

static void t_smp_spread(void)
{
    if (ncpu < 2) return;
    int fds[2];
    check_eq(pipe(fds), 0);
    int n = ncpu * 2;
    for (int k = 0; k < n; k++)
        if (fork() == 0) {
            int mask = 0;
            long t0 = uptime();
            while (uptime() - t0 < 300) mask |= 1 << getcpu();
            write(fds[1], &mask, sizeof mask);
            exit(0);
        }
    int all = 0;
    for (int k = 0; k < n; k++) {
        int m;
        check_eq(read(fds[0], &m, sizeof m), sizeof m);
        all |= m;
    }
    for (int k = 0; k < n; k++) wait(0);
    int used = 0;
    for (int c = 0; c < 32; c++) used += (all >> c) & 1;
    if (used < 2) fail("CPU-bound processes only ever ran on %d cpu (mask %x)", used, all);
}

/* ============================================================= file system */

static void t_file_rw(void)
{
    const char *path = "/tmp_rw";
    char buf[100];
    write_file(path, "hello world", 11);
    int fd = open(path, O_RDONLY);
    check(fd >= 0);
    check_eq(read(fd, buf, sizeof buf), 11);
    check(memcmp(buf, "hello world", 11) == 0);
    check_eq(read(fd, buf, sizeof buf), 0);
    check_eq(write(fd, "x", 1), -EBADF);              /* read-only */
    check_eq(lseek(fd, 6, SEEK_SET), 6);
    check_eq(read(fd, buf, 5), 5);
    check(memcmp(buf, "world", 5) == 0);
    close(fd);
    fd = open(path, O_WRONLY | O_APPEND);
    check_eq(write(fd, "!!", 2), 2);
    close(fd);
    struct stat st;
    fd = open(path, O_RDONLY);
    check_eq(fstat(fd, &st), 0);
    check_eq(st.size, 13);
    check_eq(st.type, T_FILE);
    check_eq(st.nlink, 1);
    close(fd);
    fd = open(path, O_RDWR | O_TRUNC);
    check_eq(fstat(fd, &st), 0);
    check_eq(st.size, 0);
    close(fd);
    check_eq(open(path, O_RDWR | 3), -EINVAL);
    check_eq(unlink(path), 0);
    check_eq(open(path, O_RDONLY), -ENOENT);
}

static void t_big_file(void)
{
    /* 11 direct blocks, then the single-indirect (1024 blocks), then the
     * double-indirect: 4.6 MB reaches all three. */
    const long size = (11 + 1024 + 100) * 4096L + 123;
    static uint8_t chunk[32768], back[32768];
    int fd = open("/bigfile", O_CREATE | O_RDWR | O_TRUNC);
    check(fd >= 0);
    for (long off = 0; off < size; off += sizeof chunk) {
        long n = MIN((long)sizeof chunk, size - off);
        fill(chunk, (size_t)n, (uint32_t)(off / sizeof chunk));
        check_eq(write(fd, chunk, (size_t)n), n);
    }
    struct stat st;
    fstat(fd, &st);
    check_eq(st.size, size);
    check_eq(lseek(fd, 0, SEEK_SET), 0);
    for (long off = 0; off < size; off += sizeof chunk) {
        long n = MIN((long)sizeof chunk, size - off);
        fill(chunk, (size_t)n, (uint32_t)(off / sizeof chunk));
        check_eq(read(fd, back, sizeof back), n);
        if (memcmp(chunk, back, (size_t)n) != 0) fail("big file mismatch at offset %ld", off);
    }
    close(fd);
    check_eq(unlink("/bigfile"), 0);
}

static void t_holes(void)
{
    int fd = open("/holes", O_CREATE | O_RDWR | O_TRUNC);
    check_eq(lseek(fd, 100000, SEEK_SET), 100000);
    check_eq(write(fd, "end", 3), 3);
    char buf[16];
    check_eq(lseek(fd, 50000, SEEK_SET), 50000);
    check_eq(read(fd, buf, 16), 16);
    for (int i = 0; i < 16; i++) check_eq(buf[i], 0);
    close(fd);
    unlink("/holes");
}

static void t_many_files(void)
{
    check_eq(mkdir("/many"), 0);
    char name[64];
    for (int i = 0; i < 150; i++) {
        snprintf(name, sizeof name, "/many/f%03d", i);
        write_file(name, name, strlen(name));
    }
    int fd = open("/many", O_RDONLY);
    struct dirent de;
    int count = 0;
    while (read(fd, &de, sizeof de) == sizeof de)
        if (de.inum) count++;
    close(fd);
    check_eq(count, 152);                     /* plus . and .. */
    for (int i = 0; i < 150; i++) {
        snprintf(name, sizeof name, "/many/f%03d", i);
        char buf[64] = {0};
        int f = open(name, O_RDONLY);
        check_eq(read(f, buf, sizeof buf), (long)strlen(name));
        check(strcmp(buf, name) == 0);
        close(f);
        check_eq(unlink(name), 0);
    }
    check_eq(unlink("/many"), 0);
}

static void t_directories(void)
{
    check_eq(mkdir("/d1"), 0);
    check_eq(mkdir("/d1"), -EEXIST);
    check_eq(mkdir("/d1/d2"), 0);
    check_eq(mkdir("/nonexistent/x"), -ENOENT);
    check_eq(chdir("/d1/d2"), 0);
    write_file("rel", "r", 1);
    check_eq(chdir(".."), 0);
    int fd = open("d2/rel", O_RDONLY);
    check(fd >= 0);
    close(fd);
    check_eq(chdir("/motd.txt"), -ENOTDIR);
    check_eq(unlink("/d1/d2"), -ENOTEMPTY);
    check_eq(unlink("/d1/d2/rel"), 0);
    check_eq(unlink("/d1/d2/."), -EINVAL);
    check_eq(unlink("/d1/d2"), 0);
    struct stat st;
    fd = open("/d1", O_RDONLY);
    fstat(fd, &st);
    check_eq(st.nlink, 2);                    /* "/d1" entry + its own "." */
    check_eq(open("/d1", O_WRONLY), -EISDIR);
    close(fd);
    check_eq(chdir("/"), 0);
    check_eq(unlink("/d1"), 0);
    check_eq(open("/aaaaaaaaaabbbbbbbbbbccccccccccd", O_CREATE | O_RDWR), -ENAMETOOLONG);
}

static void t_links(void)
{
    write_file("/l_a", "linked", 6);
    check_eq(link("/l_a", "/l_b"), 0);
    check_eq(link("/l_a", "/l_b"), -EEXIST);
    check_eq(link("/bin", "/bin2"), -EPERM);
    struct stat a, b;
    int fa = open("/l_a", O_RDONLY), fb = open("/l_b", O_RDONLY);
    fstat(fa, &a);
    fstat(fb, &b);
    check_eq(a.ino, b.ino);
    check_eq(a.nlink, 2);
    close(fa);
    close(fb);
    check_eq(unlink("/l_a"), 0);
    char buf[8] = {0};
    fb = open("/l_b", O_RDONLY);
    check_eq(read(fb, buf, 8), 6);
    check(memcmp(buf, "linked", 6) == 0);
    fstat(fb, &b);
    check_eq(b.nlink, 1);
    close(fb);
    check_eq(unlink("/l_b"), 0);
}

static void t_unlink_open(void)
{
    write_file("/uo", "still here", 10);
    int fd = open("/uo", O_RDONLY);
    check_eq(unlink("/uo"), 0);
    check_eq(open("/uo", O_RDONLY), -ENOENT);
    char buf[16] = {0};
    check_eq(read(fd, buf, sizeof buf), 10);   /* the inode lives until the last close */
    check(memcmp(buf, "still here", 10) == 0);
    close(fd);
}

static void t_concurrent_fs(void)
{
    enum { KIDS = 4, FILES = 12 };
    for (int k = 0; k < KIDS; k++)
        if (fork() == 0) {
            char name[32];
            static uint8_t data[9000], back[9000];
            for (int i = 0; i < FILES; i++) {
                snprintf(name, sizeof name, "/c%d_%d", k, i);
                fill(data, sizeof data, (uint32_t)(k * 100 + i));
                write_file(name, data, sizeof data);
            }
            for (int i = 0; i < FILES; i++) {
                snprintf(name, sizeof name, "/c%d_%d", k, i);
                fill(data, sizeof data, (uint32_t)(k * 100 + i));
                int fd = open(name, O_RDONLY);
                if (read(fd, back, sizeof back) != (long)sizeof back || memcmp(data, back, sizeof data)) exit(1);
                close(fd);
                if (unlink(name) != 0) exit(2);
            }
            exit(0);
        }
    for (int k = 0; k < KIDS; k++) {
        int st;
        wait(&st);
        check_eq(st, 0);
    }
}

static void t_shared_offset(void)
{
    /* parent and child share one open file (and its offset) after fork */
    int fd = open("/shared", O_CREATE | O_RDWR | O_TRUNC);
    int pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 50; i++) write(fd, "c", 1);
        exit(0);
    }
    for (int i = 0; i < 50; i++) write(fd, "p", 1);
    wait(0);
    struct stat st;
    fstat(fd, &st);
    check_eq(st.size, 100);
    close(fd);
    unlink("/shared");
}

static void t_fd_table(void)
{
    int fds[NOFILE_GUESS];
    int n = 0;
    for (;; n++) {
        int fd = open("/motd.txt", O_RDONLY);
        if (fd < 0) {
            check_eq(fd, -EMFILE);
            break;
        }
        fds[n] = fd;
        check(n < NOFILE_GUESS - 1);
    }
    check(n >= 10);
    for (int i = 0; i < n; i++) check_eq(close(fds[i]), 0);
    check_eq(close(fds[0]), -EBADF);
    check_eq(read(-1, scratch, 1), -EBADF);
    check_eq(read(1000, scratch, 1), -EBADF);
}

static void t_dup(void)
{
    write_file("/dupf", "0123456789", 10);
    int a = open("/dupf", O_RDONLY);
    int b = dup(a);
    check(b >= 0 && b != a);
    char c;
    read(a, &c, 1);
    read(b, &c, 1);
    check_eq(c, '1');                         /* one shared offset */
    int d = dup2(a, 9);
    check_eq(d, 9);
    read(9, &c, 1);
    check_eq(c, '2');
    check_eq(dup2(a, a), a);
    check_eq(dup2(a, 100), -EBADF);
    close(a);
    close(b);
    close(9);
    unlink("/dupf");
}

/* =================================================================== pipes */

static void t_pipe_basic(void)
{
    int fds[2];
    check_eq(pipe(fds), 0);
    check_eq(write(fds[1], "abc", 3), 3);
    char buf[8];
    check_eq(read(fds[0], buf, 8), 3);
    close(fds[1]);
    check_eq(read(fds[0], buf, 8), 0);        /* EOF once every writer is gone */
    close(fds[0]);
    check_eq(pipe(fds), 0);
    close(fds[0]);
    check_eq(write(fds[1], "x", 1), -EPIPE);  /* no reader */
    close(fds[1]);
    check_eq(lseek(0, 0, SEEK_SET), -ESPIPE);
}

static void t_pipe_bulk(void)
{
    int fds[2];
    check_eq(pipe(fds), 0);
    const long total = 1 << 20;
    int pid = fork();
    if (pid == 0) {
        close(fds[0]);
        static uint8_t buf[3000];
        for (long sent = 0; sent < total;) {
            long n = MIN((long)sizeof buf, total - sent);
            for (long i = 0; i < n; i++) buf[i] = (uint8_t)((sent + i) * 7);
            check_eq(write_all(fds[1], buf, (size_t)n), n);
            sent += n;
        }
        exit(0);
    }
    close(fds[1]);
    static uint8_t buf[5000];
    long got = 0, r;
    while ((r = read(fds[0], buf, sizeof buf)) > 0) {
        for (long i = 0; i < r; i++)
            if (buf[i] != (uint8_t)((got + i) * 7)) fail("pipe byte %ld corrupted", got + i);
        got += r;
    }
    check_eq(got, total);
    close(fds[0]);
    wait(0);
}

static void t_pipe_chain(void)
{
    /* 8 processes in a ring-less chain, each adding 1 to every byte */
    enum { STAGES = 8 };
    int in[2];
    check_eq(pipe(in), 0);
    int first_write = in[1], prev_read = in[0];
    for (int s = 0; s < STAGES; s++) {
        int p[2];
        check_eq(pipe(p), 0);
        if (fork() == 0) {
            close(first_write);
            close(p[0]);
            char buf[256];
            long n;
            while ((n = read(prev_read, buf, sizeof buf)) > 0) {
                for (long i = 0; i < n; i++) buf[i]++;
                write_all(p[1], buf, (size_t)n);
            }
            exit(0);
        }
        close(prev_read);
        close(p[1]);
        prev_read = p[0];
    }
    char msg[1000];
    for (int i = 0; i < 1000; i++) msg[i] = (char)(i % 50);
    check_eq(write_all(first_write, msg, sizeof msg), (long)sizeof msg);
    close(first_write);
    char out[1000];
    long got = 0, r;
    while ((r = read(prev_read, out + got, sizeof out - (size_t)got)) > 0) got += r;
    check_eq(got, 1000);
    for (int i = 0; i < 1000; i++) check_eq(out[i], (char)(i % 50 + STAGES));
    close(prev_read);
    for (int s = 0; s < STAGES; s++) wait(0);
}

/* ============================================================== the runner */

struct test {
    const char *name;
    void (*fn)(void);
    bool slow;
};

static const struct test tests[] = {
    {"fork_wait", t_fork_wait, false},
    {"fork_exhaust", t_fork_exhaust, false},
    {"orphans", t_orphans, false},
    {"wait_nohang", t_wait_nohang, false},
    {"getppid", t_getppid, false},
    {"kill", t_kill, false},
    {"kill_pipe_reader", t_kill_pipe_reader, false},
    {"preempt", t_preempt, false},
    {"sleep_time", t_sleep_time, false},
    {"exec_args", t_exec_args, false},
    {"exec_errors", t_exec_errors, false},
    {"exec_toobig_argv", t_exec_toobig_argv, false},
    {"sbrk_basic", t_sbrk_basic, false},
    {"sbrk_shrink_unmaps", t_sbrk_shrink_unmaps, false},
    {"sbrk_lazy", t_sbrk_lazy, false},
    {"out_of_memory", t_out_of_memory, true},
    {"cow_isolation", t_cow_isolation, false},
    {"cow_sharing", t_cow_sharing, false},
    {"stack_growth", t_stack_growth, false},
    {"stack_overflow", t_stack_overflow, false},
    {"faults", t_faults, false},
    {"bad_pointers", t_bad_pointers, false},
    {"bad_syscall", t_bad_syscall, false},
    {"memory_leaks", t_memory_leaks, false},
    {"fp_state", t_fp_state, false},
    {"smp_spread", t_smp_spread, false},
    {"file_rw", t_file_rw, false},
    {"big_file", t_big_file, true},
    {"holes", t_holes, false},
    {"many_files", t_many_files, false},
    {"directories", t_directories, false},
    {"links", t_links, false},
    {"unlink_open", t_unlink_open, false},
    {"concurrent_fs", t_concurrent_fs, false},
    {"shared_offset", t_shared_offset, false},
    {"fd_table", t_fd_table, false},
    {"dup", t_dup, false},
    {"pipe_basic", t_pipe_basic, false},
    {"pipe_bulk", t_pipe_bulk, false},
    {"pipe_chain", t_pipe_chain, false},
};

int main(int argc, char **argv)
{
    bool quick = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quick = true;
        first = 2;
    }
    ncpu = (int)kstat(KSTAT_NCPU);
    printf("usertests: %d cpu(s), %ld free pages\n", ncpu, kstat(KSTAT_FREE_PAGES));
    int passed = 0, failed = 0;
    long t_start = uptime();
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const struct test *t = &tests[i];
        if (argc > first) {
            bool wanted = false;
            for (int a = first; a < argc; a++) wanted |= strcmp(argv[a], t->name) == 0;
            if (!wanted) continue;
        } else if (quick && t->slow) {
            continue;
        }
        long t0 = uptime();
        int st = child_status(t->fn);
        long ms = uptime() - t0;
        if (st == 0) {
            passed++;
            printf("test %s: OK (%ld ms)\n", t->name, ms);
        } else {
            failed++;
            printf("test %s: FAILED (status %d)\n", t->name, st);
        }
    }
    printf("usertests: %d passed, %d failed in %ld ms\n", passed, failed, uptime() - t_start);
    if (failed == 0) printf("ALL TESTS PASSED\n");
    exit(failed != 0);
}
