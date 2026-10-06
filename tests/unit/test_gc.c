/* user/lib/gc.c on the host, with explicit roots (no stack scanning), so
 * every result is exact: an object survives a collection if and only if a
 * model of the object graph says it is reachable.
 *
 *  - size classes, alignment, zeroing, large objects
 *  - the interior-pointer policy, word by word, in both modes
 *  - a randomised model check (fixed seeds, printed on failure) with a
 *    roomy and a 4-entry mark stack, so the overflow rescan is exercised
 *  - page reuse: the heap shrinks back to nothing when everything dies
 *  - the collection trigger and arena exhaustion */
#include "check.h"
#include <stdint.h>
#include <string.h>
#include "../../user/lib/gc.h"

static struct gc_heap h;
static void *arena;

static void setup(size_t bytes, size_t mstack, bool interior)
{
    static size_t cur;
    if (!arena || bytes > cur) {
        free(arena);
        cur = (bytes + GC_PAGE - 1) & ~(size_t)(GC_PAGE - 1);   /* aligned_alloc wants a multiple */
        arena = aligned_alloc(GC_PAGE, cur);
    }
    CHECK(gc_heap_init(&h, arena, bytes, mstack) > 0);
    h.interior = interior;
}

static void collect(void *const *roots, size_t n)
{
    if (n) gc_heap_mark_range(&h, roots, roots + n);
    gc_heap_mark_finish(&h);
    gc_heap_sweep(&h);
}

/* ------------------------------------------------------------- basics */

static void test_classes(void)
{
    setup(8 << 20, 1024, true);
    for (size_t n = 1; n <= 3 * GC_PAGE; n += (n < 2100 ? 1 : 97)) {
        uint8_t *p = gc_heap_alloc(&h, n);
        CHECK(p != NULL);
        CHECK_EQ((uintptr_t)p % 16, 0);
        for (size_t i = 0; i < n; i++)
            if (p[i]) {
                CHECK(!"not zeroed");
                break;
            }
        memset(p, 0xee, n);
        CHECK(gc_heap_is_allocated(&h, p));
        CHECK_EQ(gc_heap_find(&h, (uintptr_t)p + n - 1), (uintptr_t)p);   /* the last byte is inside */
    }
    collect(NULL, 0);
    CHECK_EQ(h.st.live_objects, 0);
    CHECK_EQ(h.st.heap_pages, 0);
    CHECK_EQ(h.top, 0);
    /* the classes are the smallest that fit */
    for (size_t n = 1; n <= GC_MAX_SMALL; n++) {
        int c = h.cls_of[(n + 15) / 16];
        CHECK(gc_class_size(c) >= n);
        if (c) CHECK(gc_class_size(c - 1) < n);
    }
}

static void test_interior_policy(void)
{
    setup(8 << 20, 1024, true);
    char *a = gc_heap_alloc(&h, 48), *b = gc_heap_alloc(&h, 48);   /* adjacent slots */
    CHECK_EQ(b - a, 48);
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)a), (uintptr_t)a);
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)a + 47), (uintptr_t)a);
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)a + 48), (uintptr_t)b);       /* one past a is b */
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)b + 48), 0);                   /* a free slot */
    /* the 16-byte tail of a 48-byte page holds no object */
    uintptr_t page = (uintptr_t)a & ~(uintptr_t)(GC_PAGE - 1);
    CHECK_EQ(gc_heap_find(&h, page + GC_PAGE - 8), 0);
    /* outside the object pages */
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)h.pages), 0);
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)h.mstack), 0);
    CHECK_EQ(gc_heap_find(&h, h.base - 8), 0);
    CHECK_EQ(gc_heap_find(&h, h.base + (uintptr_t)h.top * GC_PAGE), 0);
    CHECK_EQ(gc_heap_find(&h, 0), 0);
    CHECK_EQ(gc_heap_find(&h, ~(uintptr_t)0), 0);
    /* a large object: every byte of its pages, not one past them */
    char *big = gc_heap_alloc(&h, 2 * GC_PAGE + 1);
    CHECK_EQ((uintptr_t)big % GC_PAGE, 0);
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)big + 3 * GC_PAGE - 1), (uintptr_t)big);
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)big + 3 * GC_PAGE), 0);
    /* a root to the middle keeps a, one past the end of b keeps nothing */
    void *roots[2] = {a + 20, b + 48};
    collect(roots, 2);
    CHECK(gc_heap_is_allocated(&h, a));
    CHECK(!gc_heap_is_allocated(&h, b));
    CHECK(!gc_heap_is_allocated(&h, big));
    /* interior pointers off: only object starts count */
    h.interior = false;
    b = gc_heap_alloc(&h, 48);
    big = gc_heap_alloc(&h, 2 * GC_PAGE);
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)a + 1), 0);
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)big + GC_PAGE), 0);
    CHECK_EQ(gc_heap_find(&h, (uintptr_t)big + 8), 0);
    void *roots2[3] = {a + 20, b, big + 8};
    collect(roots2, 3);
    CHECK(!gc_heap_is_allocated(&h, a));
    CHECK(gc_heap_is_allocated(&h, b));
    CHECK(!gc_heap_is_allocated(&h, big));
}

/* ----------------------------------------------------------- model check */

#define MAXOBJ 3000
#define NFIELD 4
#define NROOT  24

struct obj {                     /* the first NFIELD words of every object */
    struct obj *f[NFIELD];
    uint64_t    id, canary;
};

static struct {
    struct obj *p;
    size_t      size;
    int         edge[NFIELD];     /* model of p->f[]: object index or -1 */
    bool        reach;
} m[MAXOBJ];
static int nobj;
static void *roots[NROOT];
static int root_of[NROOT];

static uint64_t canary_of(uint64_t id) { return 0xdead000000000000ull ^ (id * 0x9e3779b97f4a7c15ull); }

static void reach_from(int i)
{
    if (i < 0 || m[i].reach) return;
    m[i].reach = true;
    for (int k = 0; k < NFIELD; k++) reach_from(m[i].edge[k]);
}

/* Collect, then compare with the model; drop dead objects from the model. */
static void collect_and_check(unsigned long long seed, int step)
{
    for (int i = 0; i < nobj; i++) m[i].reach = false;
    for (int r = 0; r < NROOT; r++) reach_from(root_of[r]);
    collect(roots, NROOT);
    int remap[MAXOBJ], n = 0, bad = 0;
    for (int i = 0; i < nobj; i++) {
        if (gc_heap_is_allocated(&h, m[i].p) != m[i].reach) bad++;
        if (!m[i].reach) continue;
        const struct obj *o = m[i].p;
        if (o->canary != canary_of(o->id)) bad++;
        for (int k = 0; k < NFIELD; k++)
            if (o->f[k] != (m[i].edge[k] < 0 ? NULL : m[m[i].edge[k]].p)) bad++;
    }
    for (int i = 0; i < nobj; i++) {    /* forget the dead */
        remap[i] = m[i].reach ? n : -1;
        if (m[i].reach) m[n++] = m[i];
    }
    for (int i = 0; i < n; i++)
        for (int k = 0; k < NFIELD; k++)
            if (m[i].edge[k] >= 0) m[i].edge[k] = remap[m[i].edge[k]];
    for (int r = 0; r < NROOT; r++)
        if (root_of[r] >= 0) root_of[r] = remap[root_of[r]];
    nobj = n;
    if (bad) fprintf(stderr, "seed %llu step %d: %d mismatches\n", seed, step, bad);
    CHECK_EQ(bad, 0);
    CHECK_EQ(h.st.live_objects, (uint64_t)n);
}

static void model_run(unsigned long long seed, size_t mstack, bool interior, int steps)
{
    setup(32 << 20, mstack, interior);
    rng_state = seed;
    nobj = 0;
    for (int r = 0; r < NROOT; r++) {
        roots[r] = NULL;
        root_of[r] = -1;
    }
    static uint64_t next_id;
    for (int step = 0; step < steps; step++) {
        unsigned op = (unsigned)(rnd() % 100);
        if (op < 50 && nobj < MAXOBJ) {                  /* allocate */
            size_t size = rnd() % 8 ? sizeof(struct obj) + rnd() % 200 : 2000 + rnd() % 12000;
            struct obj *o = gc_heap_alloc(&h, size);
            CHECK(o != NULL);
            if (!o) return;
            o->id = ++next_id;
            o->canary = canary_of(o->id);
            m[nobj].p = o;
            m[nobj].size = size;
            for (int k = 0; k < NFIELD; k++) m[nobj].edge[k] = -1;
            int r = (int)(rnd() % NROOT);                /* new objects start rooted */
            roots[r] = interior && rnd() % 2 ? (char *)o + rnd() % size : (void *)o;
            root_of[r] = nobj++;
        } else if (op < 85 && nobj) {                    /* link or unlink a field */
            int i = (int)(rnd() % (unsigned)nobj), k = (int)(rnd() % NFIELD);
            int j = rnd() % 4 ? (int)(rnd() % (unsigned)nobj) : -1;
            m[i].p->f[k] = j < 0 ? NULL : m[j].p;
            m[i].edge[k] = j;
        } else if (op < 97) {                            /* drop a root */
            int r = (int)(rnd() % NROOT);
            roots[r] = NULL;
            root_of[r] = -1;
        } else {
            collect_and_check(seed, step);
        }
    }
    collect_and_check(seed, steps);
    for (int r = 0; r < NROOT; r++) {
        roots[r] = NULL;
        root_of[r] = -1;
    }
    collect_and_check(seed, steps + 1);
    CHECK_EQ(nobj, 0);
    CHECK_EQ(h.st.heap_pages, 0);
    CHECK_EQ(h.top, 0);                                   /* every page went back */
}

static void test_model(void)
{
    const unsigned long long seeds[] = {1, 20261006, 0xfeedfacecafebeefull};
    for (size_t s = 0; s < sizeof seeds / sizeof seeds[0]; s++) {
        model_run(seeds[s], 4096, true, 20000);
        CHECK_EQ(h.st.mark_overflows, 0);
        model_run(seeds[s], 4, true, 20000);              /* tiny mark stack: overflow and rescan */
        CHECK(h.st.mark_overflows > 0);
        model_run(seeds[s], 64, false, 20000);
    }
    printf("test_gc: model check, %zu seeds x 3 configurations x 20000 steps\n",
           sizeof seeds / sizeof seeds[0]);
}

/* ------------------------------------------------------------ the heap */

/* A long list whose only root is its head: deep marking with a tiny stack. */
static void test_deep_list(void)
{
    setup(16 << 20, 2, true);
    struct obj *head = NULL;
    for (int i = 0; i < 50000; i++) {
        struct obj *o = gc_heap_alloc(&h, sizeof *o);
        o->f[0] = head;
        o->id = (uint64_t)i;
        head = o;
    }
    void *r[1] = {head};
    collect(r, 1);
    CHECK_EQ(h.st.live_objects, 50000);
    int n = 0;
    for (struct obj *o = head; o; o = o->f[0]) n += o->id == (uint64_t)(49999 - n);
    CHECK_EQ(n, 50000);
    r[0] = head->f[0]->f[0];                    /* drop the first two */
    collect(r, 1);
    CHECK_EQ(h.st.live_objects, 49998);
}

static void test_reuse_and_shrink(void)
{
    setup(16 << 20, 256, true);
    rng_state = 99;
    uint32_t most = 0;                          /* the most pages any single round needed */
    for (int round = 0; round < 200; round++) {
        void *keep[8] = {0};
        for (int i = 0; i < 1000; i++) {
            size_t n = rnd() % 10 ? 16 + rnd() % 500 : 3000 + rnd() % 20000;
            void *p = gc_heap_alloc(&h, n);
            CHECK(p != NULL);
            keep[rnd() % 8] = p;
        }
        if (h.top > most) most = h.top;
        collect(keep, 8);
        CHECK(h.st.live_objects <= 8);
        collect(NULL, 0);
        CHECK_EQ(h.st.heap_pages, 0);
        CHECK_EQ(h.top, 0);
        CHECK_EQ(h.free_runs, GC_NONE);
    }
    CHECK_EQ(h.st.peak_pages, most);            /* rounds never pile up */
}

/* Pages freed in the middle are reused, and neighbouring runs merge. */
static void test_page_runs(void)
{
    setup(16 << 20, 256, true);
    void *big[10];
    for (int i = 0; i < 10; i++) big[i] = gc_heap_alloc(&h, 2 * GC_PAGE);   /* 20 pages */
    uint32_t top = h.top;
    void *keep[2] = {big[0], big[9]};
    collect(keep, 2);                           /* pages 2..17 free, as one run */
    CHECK_EQ(h.top, top);
    CHECK(h.free_runs != GC_NONE);
    CHECK_EQ(h.pages[h.free_runs].run, 16);
    void *mid = gc_heap_alloc(&h, 16 * GC_PAGE - 10);
    CHECK_EQ((uintptr_t)mid, (uintptr_t)big[0] + 2 * GC_PAGE);     /* fits the hole exactly */
    CHECK_EQ(h.top, top);
    CHECK_EQ(h.free_runs, GC_NONE);
}

static void test_trigger_and_exhaustion(void)
{
    setup(64 * (GC_PAGE + sizeof(struct gc_page)) + 64 * sizeof(uintptr_t) + GC_PAGE, 64, true);
    CHECK_EQ(h.npages, 64);
    h.trigger_min = 10000;
    CHECK(!gc_heap_want_collect(&h));
    void *keep[1];
    int n = 0;
    while ((keep[0] = gc_heap_alloc(&h, 1000)) != NULL) n++;
    CHECK_EQ(n, 64 * 4);                        /* 1000 bytes -> 1024 class, 4 per page */
    CHECK(gc_heap_want_collect(&h));
    CHECK(gc_heap_alloc(&h, 16) == NULL);
    collect(NULL, 0);
    CHECK(!gc_heap_want_collect(&h));
    CHECK(gc_heap_alloc(&h, 64 * GC_PAGE) != NULL);    /* the whole arena as one object */
    CHECK(gc_heap_alloc(&h, 65 * GC_PAGE) == NULL);
    CHECK(gc_heap_alloc(&h, (size_t)-1) == NULL);
    collect(NULL, 0);
    /* live data above trigger_min raises the trigger */
    void *live[40];
    for (int i = 0; i < 40; i++) live[i] = gc_heap_alloc(&h, 1000);
    collect(live, 40);
    CHECK_EQ(h.st.live_bytes, 40 * 1024);
    for (int i = 0; i < 39; i++) gc_heap_alloc(&h, 1000);
    CHECK(!gc_heap_want_collect(&h));           /* 39 KiB < 40 KiB live */
    gc_heap_alloc(&h, 1000);
    CHECK(gc_heap_want_collect(&h));
}

int main(void)
{
    test_classes();
    test_interior_policy();
    test_model();
    test_deep_list();
    test_reuse_and_shrink();
    test_page_runs();
    test_trigger_and_exhaustion();
    free(arena);
    return check_summary("test_gc");
}
