/* gctest: the garbage collector's tests, run inside wren-os with the real
 * conservative roots (registers, stack, .data/.bss).
 *
 *   gctest            run everything
 *   gctest name...    run only the named tests
 *
 * Like usertests, every test runs in its own child process.  A test that
 * must show an object was reclaimed keeps only a "hidden" copy of its
 * address (XOR with a key, so the collector cannot see it) and scrubs the
 * dead stack first, so no stale copy left by an earlier call keeps it. */
#include "lib/ulib.h"
#include "lib/gc.h"

#define fail(...) do { \
        fprintf(2, "    FAIL %s:%d: ", __FILE__, __LINE__); fprintf(2, __VA_ARGS__); fprintf(2, "\n"); exit(1); \
    } while (0)
#define check(c) do { if (!(c)) fail("check failed: %s", #c); } while (0)
#define check_eq(a, b) do { long a_ = (long)(a), b_ = (long)(b); \
        if (a_ != b_) fail("%s == %ld, expected %s == %ld", #a, a_, #b, b_); } while (0)

#define KEY 0x5a5a5a5a5a5a5a5aull
static volatile uint64_t key = KEY;  /* volatile: SHOW() results cannot be shared between uses */
#define HIDE(p) ((uintptr_t)(p) ^ key)
#define SHOW(h) ((void *)((h) ^ key))

struct node {
    struct node *next, *left, *right;
    uint64_t     id, canary;
};
static inline uint64_t canary(uint64_t id) { return 0xc0ffee0000000000ull ^ (id * 0x9e3779b97f4a7c15ull); }

static struct node *new_node(uint64_t id)
{
    struct node *n = gc_malloc(sizeof *n);
    if (!n) fail("gc_malloc returned NULL");
    n->id = id;
    n->canary = canary(id);
    return n;
}

static bool node_ok(const struct node *n) { return gc_is_allocated(n) && n->canary == canary(n->id); }

/* Queries on hidden addresses.  Out of line, so the real address only
 * ever exists in this frame's registers, which are gone (callee-saved
 * ones restored) when it returns; otherwise the compiler may keep it in
 * x19-x28 across the next collection, where it is a perfectly good root. */
static __attribute__((noinline)) bool live(uintptr_t h) { return gc_is_allocated(SHOW(h)); }
static __attribute__((noinline)) bool ok(uintptr_t h) { return node_ok(SHOW(h)); }

static __attribute__((noinline)) void scrub(void)
{
    volatile char pad[8192];
    for (size_t i = 0; i < sizeof pad; i++) pad[i] = 0;
}

/* collect with nothing from the caller's frames on the stack */
static void clean_collect(void)
{
    scrub();
    gc_collect();
}

static struct node *list(uint64_t first, int n)
{
    struct node *head = NULL;
    for (int i = n - 1; i >= 0; i--) {
        struct node *x = new_node(first + (uint64_t)i);
        x->next = head;
        head = x;
    }
    return head;
}

static struct node *tree(uint64_t *id, int depth)
{
    struct node *t = new_node((*id)++);
    if (depth > 0) {
        t->left = tree(id, depth - 1);
        t->right = tree(id, depth - 1);
    }
    return t;
}

static long check_tree(const struct node *t)
{
    if (!t) return 0;
    if (!node_ok(t)) fail("tree node %lu damaged", (unsigned long)t->id);
    return 1 + check_tree(t->left) + check_tree(t->right);
}

static long check_list(const struct node *l)
{
    long n = 0;
    for (; l; l = l->next, n++)
        if (!node_ok(l)) fail("list node %lu damaged", (unsigned long)l->id);
    return n;
}

static __attribute__((noinline)) void make_garbage(int n)
{
    uint64_t id = 1u << 20;
    struct node *l = list(id, n);
    struct node *t = tree(&id, 8);
    l->left = t;
}

static __attribute__((noinline)) uintptr_t make_hidden(uint64_t id) { return HIDE(new_node(id)); }

static struct gc_stats stats(void)
{
    struct gc_stats st;
    gc_get_stats(&st);
    return st;
}

/* ---------------------------------------------------------------- tests */

static struct node *g_list, *g_tree;               /* .bss roots */
static struct node *volatile g_data = (struct node *)1;   /* a .data root */

static void t_reachable_survive(void)
{
    uint64_t id = 0;
    g_list = list(1000000, 2000);
    g_tree = tree(&id, 10);
    for (int round = 0; round < 40; round++) {
        make_garbage(3000);
        gc_collect();
        check_eq(check_list(g_list), 2000);
        check_eq(check_tree(g_tree), 2047);
    }
    check(stats().collections >= 40);
}

static void t_unreachable_reclaimed(void)
{
    uint64_t before = stats().freed_objects;
    make_garbage(10000);             /* 10000 list nodes + 511 tree nodes */
    clean_collect();
    uint64_t freed = stats().freed_objects - before;
    if (freed < 10400) fail("only %lu of 10511 garbage objects reclaimed", (unsigned long)freed);
    check(stats().live_objects < 100);
}

static void t_heap_bounded(void)
{
    uint64_t id = 0;
    g_tree = tree(&id, 8);
    uint64_t peak_warm = 0;
    long free_warm = 0;
    for (int round = 1; round <= 300; round++) {
        make_garbage(2000);
        if (round == 30) {
            peak_warm = stats().peak_pages;
            free_warm = kstat(KSTAT_FREE_PAGES);
        }
    }
    struct gc_stats st = stats();
    long free_end = kstat(KSTAT_FREE_PAGES);
    printf("    %lu collections, peak %lu pages at round 30 and %lu at round 300, "
           "%lu MiB allocated, kernel free pages %ld -> %ld\n",
           (unsigned long)st.collections, (unsigned long)peak_warm, (unsigned long)st.peak_pages,
           (unsigned long)(st.allocated_bytes >> 20), free_warm, free_end);
    check(st.collections >= 20);
    check(st.peak_pages <= peak_warm + 8);
    check(free_end >= free_warm - 16);
    check(st.allocated_bytes >= 20 * st.peak_pages * GC_PAGE);
    check_eq(check_tree(g_tree), 511);
}

/* collect_in_regs(h, key): load h[i] ^ key into x19-x28 and d8 (the
 * only copies of those addresses anywhere), run gc_collect, then store the
 * registers back into h[].  Every callee-saved register the root scan
 * spills is covered, not just one the collector's own frames happen to
 * save on the way down. */
#define NREG 11
void collect_in_regs(uintptr_t h[NREG], uint64_t k);
__asm__("    .text\n"
        "    .globl collect_in_regs\n"
        "collect_in_regs:\n"
        "    stp  x29, x30, [sp, #-112]!\n"
        "    mov  x29, sp\n"
        "    stp  x19, x20, [sp, #16]\n"
        "    stp  x21, x22, [sp, #32]\n"
        "    stp  x23, x24, [sp, #48]\n"
        "    stp  x25, x26, [sp, #64]\n"
        "    stp  x27, x28, [sp, #80]\n"
        "    str  d8, [sp, #96]\n"
        "    str  x0, [sp, #104]\n"
        "    ldp  x19, x20, [x0, #0]\n"
        "    ldp  x21, x22, [x0, #16]\n"
        "    ldp  x23, x24, [x0, #32]\n"
        "    ldp  x25, x26, [x0, #48]\n"
        "    ldp  x27, x28, [x0, #64]\n"
        "    ldr  x9, [x0, #80]\n"
        "    eor  x19, x19, x1\n"
        "    eor  x20, x20, x1\n"
        "    eor  x21, x21, x1\n"
        "    eor  x22, x22, x1\n"
        "    eor  x23, x23, x1\n"
        "    eor  x24, x24, x1\n"
        "    eor  x25, x25, x1\n"
        "    eor  x26, x26, x1\n"
        "    eor  x27, x27, x1\n"
        "    eor  x28, x28, x1\n"
        "    eor  x9, x9, x1\n"
        "    fmov d8, x9\n"
        "    mov  x9, #0\n"
        "    mov  x1, #0\n"
        "    bl   gc_collect\n"
        "    ldr  x0, [sp, #104]\n"
        "    stp  x19, x20, [x0, #0]\n"
        "    stp  x21, x22, [x0, #16]\n"
        "    stp  x23, x24, [x0, #32]\n"
        "    stp  x25, x26, [x0, #48]\n"
        "    stp  x27, x28, [x0, #64]\n"
        "    fmov x9, d8\n"
        "    str  x9, [x0, #80]\n"
        "    ldp  x19, x20, [sp, #16]\n"
        "    ldp  x21, x22, [sp, #32]\n"
        "    ldp  x23, x24, [sp, #48]\n"
        "    ldp  x25, x26, [sp, #64]\n"
        "    ldp  x27, x28, [sp, #80]\n"
        "    ldr  d8, [sp, #96]\n"
        "    ldp  x29, x30, [sp], #112\n"
        "    ret\n");

static void t_register_root(void)
{
    uintptr_t h[NREG];
    /* control: without the registers the objects are reclaimed */
    for (int i = 0; i < NREG; i++) h[i] = make_hidden((uint64_t)i);
    clean_collect();
    for (int i = 0; i < NREG; i++) check(!live(h[i]));
    /* held only in x19-x28 and d8, they survive with their contents */
    for (int i = 0; i < NREG; i++) h[i] = make_hidden(100 + (uint64_t)i);
    scrub();
    collect_in_regs(h, KEY);
    for (int i = 0; i < NREG; i++) {
        struct node *n = (struct node *)h[i];
        if (!node_ok(n) || n->id != 100 + (uint64_t)i)
            fail("object held in %s was not kept", i < 10 ? (const char *[]){"x19", "x20", "x21", "x22",
                 "x23", "x24", "x25", "x26", "x27", "x28"}[i] : "d8");
    }
}

static __attribute__((noinline)) void hold_on_stack(uintptr_t h)
{
    volatile uintptr_t slot = (uintptr_t)SHOW(h);    /* a stack slot is the only reference */
    gc_collect();
    check(node_ok((struct node *)slot));
    slot = 0;
}

static void t_stack_root(void)
{
    uintptr_t h = make_hidden(3);
    scrub();
    hold_on_stack(h);
    clean_collect();                 /* the frame is gone: now it is garbage */
    check(!live(h));
}

static void t_global_roots(void)
{
    uintptr_t a = make_hidden(4), b = make_hidden(5), c = make_hidden(6);
    g_list = SHOW(a);                /* .bss */
    g_data = SHOW(b);                /* .data */
    clean_collect();
    check(ok(a) && ok(b));
    check(!live(c));
    g_list = NULL;
    g_data = NULL;
    clean_collect();
    check(!live(a) && !live(b));
}

static char *volatile g_interior;   /* volatile: the stores are the point */

static __attribute__((noinline)) uintptr_t make_buf(size_t n)
{
    char *p = gc_malloc(n);
    memset(p, 0x77, n);
    return HIDE(p);
}

static void t_interior_pointers(void)
{
    /* 64-byte object: a pointer to byte 40 keeps it */
    uintptr_t h = make_buf(64);
    g_interior = (char *)SHOW(h) + 40;
    clean_collect();
    check(gc_is_allocated(SHOW(h)) && ((char *)SHOW(h))[63] == 0x77);
    /* one past the end does not */
    g_interior = (char *)SHOW(h) + 64;
    clean_collect();
    check(!live(h));
    /* a large object (3 pages) is kept by a pointer into its last page */
    h = make_buf(3 * GC_PAGE - 100);
    g_interior = (char *)SHOW(h) + 2 * GC_PAGE + 5;
    clean_collect();
    check(gc_is_allocated(SHOW(h)));
    /* with interior pointers off, only the start counts */
    gc_set_interior(false);
    h = make_buf(64);
    g_interior = (char *)SHOW(h) + 8;
    clean_collect();
    check(!live(h));
    h = make_buf(64);
    g_interior = SHOW(h);
    clean_collect();
    check(gc_is_allocated(SHOW(h)));
    gc_set_interior(true);
    g_interior = NULL;
}

static uintptr_t hidden_ring[100];

static __attribute__((noinline)) void make_cycles(void)
{
    struct node *self = new_node(10);
    self->next = self;
    struct node *a = new_node(11), *b = new_node(12);
    a->next = b;
    b->next = a;
    hidden_ring[0] = HIDE(self);
    hidden_ring[1] = HIDE(a);
    hidden_ring[2] = HIDE(b);
    struct node *first = new_node(100), *prev = first;
    for (int i = 3; i < 100; i++) {
        struct node *x = new_node(100 + (uint64_t)i);
        prev->next = x;
        prev = x;
        hidden_ring[i] = HIDE(x);
    }
    prev->next = first;
    g_tree = new_node(200);          /* a reachable cycle through a global */
    g_tree->next = new_node(201);
    g_tree->next->next = g_tree;
    g_tree->left = first;            /* holds the 98-node ring for now */
}

static void t_cycles(void)
{
    make_cycles();
    clean_collect();
    for (int i = 3; i < 100; i++) check(ok(hidden_ring[i]));
    for (int i = 0; i < 3; i++) check(!live(hidden_ring[i]));
    g_tree->left = NULL;             /* drop the ring */
    clean_collect();
    for (int i = 3; i < 100; i++) check(!live(hidden_ring[i]));
    check(node_ok(g_tree) && node_ok(g_tree->next) && g_tree->next->next == g_tree);
}

static void t_fork(void)
{
    g_list = list(5000, 1000);
    int pid = fork();
    if (pid < 0) fail("fork: %s", strerror(pid));
    for (int round = 0; round < 20; round++) {
        make_garbage(1000);
        gc_collect();
        if (check_list(g_list) != 1000) fail("list damaged after fork (pid %d)", getpid());
    }
    if (pid == 0) exit(0);
    int st = -1;
    check_eq(waitpid(pid, &st, 0), pid);
    check_eq(st, 0);
    check_eq(check_list(g_list), 1000);
}

#define NG 3000
#define NROOTS 30
static uintptr_t hidden_nodes[NG];
static struct node *g_roots[NROOTS];
static uint8_t reach[NG];

static void visit(int i, struct node **all)
{
    while (i >= 0 && !reach[i]) {    /* follow next iteratively, recurse on left */
        reach[i] = 1;
        if (all[i]->left) visit((int)all[i]->left->id, all);
        i = all[i]->next ? (int)all[i]->next->id : -1;
    }
}

static __attribute__((noinline)) void build_graph(uint64_t seed)
{
    static struct node *all[NG];     /* dropped before collecting */
    struct rng r;
    rng_seed(&r, seed);
    for (int i = 0; i < NG; i++) all[i] = new_node((uint64_t)i);
    for (int i = 0; i < NG; i++) {
        if (rng_range(&r, 4)) all[i]->next = all[rng_range(&r, NG)];
        if (rng_range(&r, 3) == 0) all[i]->left = all[rng_range(&r, NG)];
    }
    for (int k = 0; k < NROOTS; k++) g_roots[k] = all[rng_range(&r, NG)];
    for (int k = 0; k < NROOTS; k++) visit((int)g_roots[k]->id, all);
    for (int i = 0; i < NG; i++) {
        hidden_nodes[i] = HIDE(all[i]);
        all[i] = NULL;
    }
}

static void t_random_graph(void)
{
    const uint64_t seed = 20261006;
    build_graph(seed);
    for (int round = 0; round < 5; round++) {
        clean_collect();
        int kept = 0, extra = 0;
        for (int i = 0; i < NG; i++) {
            if (reach[i] && !ok(hidden_nodes[i])) fail("seed %lu: reachable node %d lost", (unsigned long)seed, i);
            if (reach[i]) kept++;
            else extra += live(hidden_nodes[i]);
        }
        if (round == 0) printf("    seed %lu: %d of %d nodes reachable, %d unreachable retained\n",
                               (unsigned long)seed, kept, NG, extra);
        if (extra > NG / 100) fail("seed %lu: %d unreachable nodes retained", (unsigned long)seed, extra);
    }
}

static void *g_large[16];

static void t_large_objects(void)
{
    struct rng r;
    rng_seed(&r, 7);
    uint64_t peak = 0;
    for (int round = 0; round < 400; round++) {
        size_t n = 3000 + rng_range(&r, 40000);
        int k = (int)rng_range(&r, 16);
        g_large[k] = gc_malloc(n);
        check(g_large[k] != NULL);
        memset(g_large[k], k + 1, n);
        ((uint64_t *)g_large[k])[0] = n;
        for (int j = 0; j < 16; j++) {
            uint8_t *p = g_large[j];
            if (!p) continue;
            size_t len = (size_t)((uint64_t *)p)[0];
            if (p[len - 1] != j + 1 || p[8] != j + 1) fail("large object %d damaged", j);
        }
        if (round == 100) peak = stats().peak_pages;
    }
    printf("    peak %lu pages at round 100, %lu at round 400\n", (unsigned long)peak,
           (unsigned long)stats().peak_pages);
    check(stats().peak_pages <= peak + 16 * 11);
}

static void t_zeroed(void)
{
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 2000; i++) {
            uint64_t *p = gc_malloc(100);
            for (int w = 0; w < 13; w++) check(p[w] == 0);
            for (int w = 0; w < 13; w++) p[w] = ~0ull;
        }
        clean_collect();
    }
}

static void t_with_malloc(void)
{
    uint8_t *m[64];
    for (int i = 0; i < 64; i++) {
        m[i] = malloc(200);
        memset(m[i], i, 200);
        make_garbage(200);
    }
    gc_collect();
    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 200; j++) check(m[i][j] == i);
        free(m[i]);
    }
}

struct test {
    const char *name;
    void      (*fn)(void);
};

static const struct test tests[] = {
    {"reachable_survive", t_reachable_survive},
    {"unreachable_reclaimed", t_unreachable_reclaimed},
    {"heap_bounded", t_heap_bounded},
    {"register_root", t_register_root},
    {"stack_root", t_stack_root},
    {"global_roots", t_global_roots},
    {"interior_pointers", t_interior_pointers},
    {"cycles", t_cycles},
    {"fork", t_fork},
    {"random_graph", t_random_graph},
    {"large_objects", t_large_objects},
    {"zeroed", t_zeroed},
    {"with_malloc", t_with_malloc},
};

int main(int argc, char **argv)
{
    printf("gctest: %ld cpu(s)\n", kstat(KSTAT_NCPU));
    int passed = 0, failed = 0;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const struct test *t = &tests[i];
        if (argc > 1) {
            bool wanted = false;
            for (int a = 1; a < argc; a++) wanted |= strcmp(argv[a], t->name) == 0;
            if (!wanted) continue;
        }
        long t0 = uptime();
        int pid = fork();
        if (pid < 0) fail("fork: %s", strerror(pid));
        if (pid == 0) {
            t->fn();
            exit(0);
        }
        int st = -1;
        waitpid(pid, &st, 0);
        if (st == 0) {
            passed++;
            printf("test %s: OK (%ld ms)\n", t->name, uptime() - t0);
        } else {
            failed++;
            printf("test %s: FAILED (status %d)\n", t->name, st);
        }
    }
    printf("gctest: %d passed, %d failed\n", passed, failed);
    if (failed == 0) printf("ALL GC TESTS PASSED\n");
    exit(failed != 0);
}
