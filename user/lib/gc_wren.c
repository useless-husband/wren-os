/* The collector's wren-os front end: where the arena comes from, what the
 * roots are, and when to collect.
 *
 * Arena: one sbrk on the first gc_malloc that reserves GC_ARENA bytes
 * starting at GC_ARENA_LOW (4 GiB).  The kernel backs the heap lazily
 * (vm_fault), so the reservation, and the gap below it, are address space
 * only; a page costs memory once the collector touches it.  Starting at
 * 4 GiB means no value that fits in 32 bits can be mistaken for a pointer
 * into the heap, which removes the most common cause of false retention
 * (counters, sizes and small integers in the roots).  The price: stray
 * accesses in the gap no longer fault in a program that uses the
 * collector.  malloc keeps working beside it: its later sbrk calls land
 * above the arena.
 *
 * Roots (roots.c): callee-saved registers, the stack from the current sp
 * to the top recorded by crt0, and .data/.bss minus the collector's own
 * state.  Objects reachable only through malloc'd memory are not found:
 * the malloc heap is not scanned (see docs/GC.md).
 *
 * wren-os processes are single-threaded, so there is no locking; a forked
 * child gets a copy-on-write copy of the whole heap and collects it on its
 * own. */
#include "gc.h"
#include "roots.h"
#include "ulib.h"

#define GC_ARENA     (256UL << 20)  /* address space reserved for the collector */
#define GC_ARENA_LOW (4UL << 30)    /* ... starting no lower than this */
#define GC_MSTACK  8192             /* mark-stack entries (64 KiB) */

static struct gc_heap heap;
static int state;                   /* 0 = not yet, 1 = ready, -1 = sbrk failed */

static bool ensure(void)
{
    if (state) return state > 0;
    uintptr_t cur = (uintptr_t)sbrk(0);
    uintptr_t gap = cur < GC_ARENA_LOW ? GC_ARENA_LOW - cur : 0;
    char *p = sbrk((long)(gap + GC_ARENA + GC_PAGE));
    if (sbrk_failed(p)) {
        state = -1;
        return false;
    }
    uintptr_t mem = ((uintptr_t)p + gap + GC_PAGE - 1) & ~(uintptr_t)(GC_PAGE - 1);
    state = gc_heap_init(&heap, (void *)mem, GC_ARENA, GC_MSTACK) ? 1 : -1;
    return state > 0;
}

static void mark_cb(void *ctx, const void *lo, const void *hi) { gc_heap_mark_range(ctx, lo, hi); }

void gc_collect(void)
{
    if (state <= 0) return;
    uint64_t t0 = now_ns();
    roots_scan(mark_cb, &heap, &heap, &heap + 1);
    gc_heap_mark_finish(&heap);
    gc_heap_sweep(&heap);
    /* Kept inside the heap struct: it is excluded from the roots, so a
     * pause time can never pose as a pointer. */
    struct gc_stats *st = &heap.st;
    st->last_pause_ns = now_ns() - t0;
    st->total_pause_ns += st->last_pause_ns;
    if (st->last_pause_ns > st->max_pause_ns) st->max_pause_ns = st->last_pause_ns;
}

void *gc_malloc(size_t n)
{
    if (!ensure()) return NULL;
    if (gc_heap_want_collect(&heap)) gc_collect();
    void *p = gc_heap_alloc(&heap, n);
    if (!p) {                       /* arena full: collect, then try once more */
        gc_collect();
        p = gc_heap_alloc(&heap, n);
    }
    return p;
}

void gc_get_stats(struct gc_stats *st)
{
    *st = heap.st;
}

bool gc_is_allocated(const void *p) { return state > 0 && gc_heap_is_allocated(&heap, p); }
void gc_set_interior(bool on)
{
    if (ensure()) heap.interior = on;
}
