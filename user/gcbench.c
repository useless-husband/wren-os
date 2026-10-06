/* gcbench [depth] [trees]: the same allocation workload with gc_malloc and
 * with malloc/free, plus collection pause times.
 *
 * Workload (after Boehm's GCBench): a long-lived binary tree of depth 14
 * (32767 nodes) stays live throughout; then `trees` short-lived trees of
 * depth `depth` are built, checksummed and dropped.  With malloc every
 * short-lived tree is freed node by node; with the collector it is just
 * dropped.  Times come from the guest's own counter (CNTVCT_EL0); under
 * QEMU TCG they are emulator times, useful only for ratios. */
#include "lib/ulib.h"
#include "lib/gc.h"

struct node {
    struct node *l, *r;
    uint64_t     v;
};

static struct node *gtree(int d)
{
    struct node *n = gc_malloc(sizeof *n);
    if (!n) {
        printf("gcbench: out of memory\n");
        exit(1);
    }
    n->v = (uint64_t)d;
    if (d > 0) {
        n->l = gtree(d - 1);
        n->r = gtree(d - 1);
    }
    return n;
}

static struct node *mtree(int d)
{
    struct node *n = malloc(sizeof *n);
    if (!n) {
        printf("gcbench: out of memory\n");
        exit(1);
    }
    n->v = (uint64_t)d;
    n->l = d > 0 ? mtree(d - 1) : NULL;
    n->r = d > 0 ? mtree(d - 1) : NULL;
    return n;
}

static void mfree(struct node *n)
{
    if (!n) return;
    mfree(n->l);
    mfree(n->r);
    free(n);
}

static uint64_t sum(const struct node *n) { return n ? n->v + sum(n->l) + sum(n->r) : 0; }

static struct node *longlived;

static uint64_t us(uint64_t ns) { return ns / 1000; }

int main(int argc, char **argv)
{
    int depth = argc > 1 ? atoi(argv[1]) : 12;
    int trees = argc > 2 ? atoi(argv[2]) : 40;
    uint64_t nodes = (2ull << depth) - 1, expect = 0;
    for (int d = 0; d <= depth; d++) expect += (uint64_t)d << (depth - d);

    /* malloc/free */
    longlived = mtree(14);
    uint64_t t0 = now_ns();
    for (int i = 0; i < trees; i++) {
        struct node *t = mtree(depth);
        if (sum(t) != expect) printf("gcbench: bad checksum\n");
        mfree(t);
    }
    uint64_t t_malloc = now_ns() - t0;
    mfree(longlived);
    longlived = NULL;

    /* garbage collector */
    longlived = gtree(14);
    struct gc_stats s0, s1;
    gc_get_stats(&s0);
    t0 = now_ns();
    for (int i = 0; i < trees; i++) {
        struct node *t = gtree(depth);
        if (sum(t) != expect) printf("gcbench: bad checksum\n");
    }
    uint64_t t_gc = now_ns() - t0;
    gc_get_stats(&s1);
    uint64_t ncoll = s1.collections - s0.collections;
    uint64_t pause_total = s1.total_pause_ns - s0.total_pause_ns;

    /* explicit full collections at a known live set: the long-lived tree only */
    uint64_t pmin = ~0ull, pmax = 0;
    for (int i = 0; i < 5; i++) {
        gc_collect();
        gc_get_stats(&s1);
        if (s1.last_pause_ns < pmin) pmin = s1.last_pause_ns;
        if (s1.last_pause_ns > pmax) pmax = s1.last_pause_ns;
    }
    uint64_t total = (uint64_t)trees * nodes;
    printf("gcbench: %d trees of depth %d (%lu nodes each, %lu-byte objects), long-lived tree of 32767 nodes\n",
           trees, depth, (unsigned long)nodes, (unsigned long)gc_class_size(1));
    printf("BENCH malloc_free_per_node   %lu ns\n", (unsigned long)(t_malloc / total));
    printf("BENCH gc_malloc_per_node     %lu ns\n", (unsigned long)(t_gc / total));
    printf("BENCH gc_collections         %lu count\n", (unsigned long)ncoll);
    printf("BENCH gc_time_in_collector   %lu %%\n", (unsigned long)(t_gc ? pause_total * 100 / t_gc : 0));
    printf("BENCH gc_pause_mean          %lu us\n", (unsigned long)us(ncoll ? pause_total / ncoll : 0));
    printf("BENCH gc_pause_max           %lu us\n", (unsigned long)us(s1.max_pause_ns));
    printf("BENCH gc_full_pause_live_%luKiB %lu us (min of 5; max %lu us)\n",
           (unsigned long)(s1.live_bytes >> 10), (unsigned long)us(pmin), (unsigned long)us(pmax));
    printf("BENCH gc_peak_heap           %lu KiB\n", (unsigned long)(s1.peak_pages * GC_PAGE >> 10));
    printf("BENCH done 0 x\n");
    return 0;
}
