/* gcdemo [rounds]: build and drop lists, trees and cycles with gc_malloc,
 * never calling free, and show that the heap stays bounded.  A long-lived
 * tree, rebuilt every 50 rounds, is checked after every round. */
#include "lib/ulib.h"
#include "lib/gc.h"

struct node {
    struct node *a, *b;
    uint64_t     val;
};

static struct node *keep;            /* the long-lived structure: a root in .bss */

static struct node *mk(uint64_t v, struct node *a, struct node *b)
{
    struct node *n = gc_malloc(sizeof *n);
    if (!n) {
        printf("gcdemo: out of memory\n");
        exit(1);
    }
    n->val = v;
    n->a = a;
    n->b = b;
    return n;
}

static struct node *tree(int depth, uint64_t *v)
{
    if (depth < 0) return NULL;
    struct node *l = tree(depth - 1, v), *r = tree(depth - 1, v);
    return mk((*v)++, l, r);
}

static uint64_t sum(const struct node *t)
{
    return t ? t->val + sum(t->a) + sum(t->b) : 0;
}

static void garbage(int round)
{
    struct node *l = NULL;
    for (int i = 0; i < 500; i++) l = mk((uint64_t)i, l, NULL);                /* a list */
    uint64_t v = 0;
    struct node *t = tree(7, &v);                                            /* a tree */
    struct node *first = mk(0, NULL, NULL), *p = first;                      /* a ring */
    for (int i = 1; i < 100; i++) p = p->a = mk((uint64_t)i, NULL, NULL);
    p->a = first;
    first->b = t;
    char *buf = gc_malloc(8192 + (size_t)(round % 7) * 1000);               /* a large object */
    if (buf) buf[0] = (char)round;
    (void)l;
}

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    if (rounds < 4) rounds = 4;
    uint64_t v = 0, expect = 0, warm_peak = 0;
    struct gc_stats st;
    for (int r = 1; r <= rounds; r++) {
        if (r % 50 == 1) {
            v = 0;
            keep = tree(9, &v);
            expect = sum(keep);
        }
        garbage(r);
        if (sum(keep) != expect) {
            printf("gcdemo: round %d: the live tree was damaged\n", r);
            exit(1);
        }
        gc_get_stats(&st);
        if (r == rounds / 4) warm_peak = st.peak_pages;
        if (r % 50 == 0 || r == rounds)
            printf("round %4d: %3lu collections, heap %4lu pages (peak %4lu), live %lu KiB, "
                   "%lu MiB allocated so far\n", r, (unsigned long)st.collections,
                   (unsigned long)st.heap_pages, (unsigned long)st.peak_pages,
                   (unsigned long)(st.live_bytes >> 10), (unsigned long)(st.allocated_bytes >> 20));
    }
    printf("gcdemo: %d rounds, %lu MiB allocated, peak heap %lu KiB, %lu collections, "
           "pause mean %lu us max %lu us\n", rounds, (unsigned long)(st.allocated_bytes >> 20),
           (unsigned long)(st.peak_pages * GC_PAGE >> 10), (unsigned long)st.collections,
           (unsigned long)(st.collections ? st.total_pause_ns / st.collections / 1000 : 0),
           (unsigned long)(st.max_pause_ns / 1000));
    /* bounded: the high-water mark stopped growing after the first quarter */
    bool bounded = st.peak_pages <= warm_peak + 16;
    printf("%s (peak %lu pages after round %d, %lu at the end)\n", bounded ? "GCDEMO OK" : "GCDEMO UNBOUNDED",
           (unsigned long)warm_peak, rounds / 4, (unsigned long)st.peak_pages);
    exit(bounded ? 0 : 1);
}
