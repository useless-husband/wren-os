/* The collector core: arena layout, allocation, conservative marking and
 * sweeping.  No system calls and no knowledge of wren-os, so the same file
 * is compiled into the host unit test (tests/unit/test_gc.c), which feeds
 * it explicit roots.
 *
 * Arena layout:  [ page descriptors | mark stack | pad | object pages ... ]
 * Everything is lazily backed when the arena comes from sbrk: a page costs
 * physical memory only once it is touched. */
#include "gc.h"
#ifdef GC_HOST
#include <string.h>
#else
#include "../../lib/kstring.h"
#endif

static const uint16_t class_size[GC_NCLASS] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 256, 384, 512, 768, 1024, 2048,
};

size_t gc_class_size(int cls) { return class_size[cls]; }

static inline size_t page_of(const struct gc_heap *h, uintptr_t a) { return (a - h->base) / GC_PAGE; }
static inline uintptr_t page_addr(const struct gc_heap *h, size_t pi) { return h->base + pi * GC_PAGE; }

uint32_t gc_heap_init(struct gc_heap *h, void *mem, size_t bytes, size_t mstack_entries)
{
    memset(h, 0, sizeof *h);
    size_t fixed = mstack_entries * sizeof(uintptr_t) + GC_PAGE;    /* mark stack + alignment pad */
    if (bytes <= fixed + GC_PAGE + sizeof(struct gc_page)) return 0;
    size_t n = (bytes - fixed) / (GC_PAGE + sizeof(struct gc_page));
    if (n > GC_NONE - 1) n = GC_NONE - 1;
    uintptr_t m = (uintptr_t)mem;
    h->pages = (struct gc_page *)m;
    h->mstack = (uintptr_t *)(m + n * sizeof(struct gc_page));
    h->mcap = mstack_entries;
    h->base = ((uintptr_t)(h->mstack + mstack_entries) + GC_PAGE - 1) & ~(uintptr_t)(GC_PAGE - 1);
    h->npages = (uint32_t)n;
    h->free_runs = GC_NONE;
    h->interior = true;
    h->trigger_min = 256 * 1024;
    for (size_t i = 0, c = 0; i <= GC_MAX_SMALL / 16; i++) {
        while (class_size[c] < i * 16) c++;
        h->cls_of[i] = (uint8_t)c;
    }
    return h->npages;
}

/* ------------------------------------------------------------ pages */

/* k contiguous free pages: first fit in the free runs, else from the top. */
static uint32_t take_pages(struct gc_heap *h, uint32_t k)
{
    for (uint32_t *link = &h->free_runs; *link != GC_NONE; link = &h->pages[*link].next) {
        uint32_t i = *link;
        struct gc_page *r = &h->pages[i];
        if (r->run < k) continue;
        if (r->run == k) {
            *link = r->next;
        } else {
            struct gc_page *rest = &h->pages[i + k];
            rest->kind = GP_FREE;
            rest->run = r->run - k;
            rest->next = r->next;
            *link = i + k;
        }
        return i;
    }
    if (k > h->npages - h->top) return GC_NONE;
    uint32_t i = h->top;
    h->top += k;
    if (h->top > h->st.peak_pages) h->st.peak_pages = h->top;
    return i;
}

/* Give class c a fresh page and thread its objects onto the free list. */
static bool refill(struct gc_heap *h, int c)
{
    uint32_t pi = take_pages(h, 1);
    if (pi == GC_NONE) return false;
    struct gc_page *d = &h->pages[pi];
    memset(d, 0, sizeof *d);
    d->kind = GP_SMALL;
    d->cls = (uint8_t)c;
    d->size = class_size[c];
    d->nobj = (uint16_t)(GC_PAGE / d->size);
    char *p = (char *)page_addr(h, pi);
    for (int i = d->nobj - 1; i >= 0; i--) {
        void **slot = (void **)(p + (size_t)i * d->size);
        *slot = h->freelist[c];
        h->freelist[c] = slot;
    }
    h->st.heap_pages++;
    return true;
}

void *gc_heap_alloc(struct gc_heap *h, size_t n)
{
    if (n == 0) n = 1;
    if (n <= GC_MAX_SMALL) {
        int c = h->cls_of[(n + 15) / 16];
        if (!h->freelist[c] && !refill(h, c)) return NULL;
        void **p = h->freelist[c];
        h->freelist[c] = *p;
        size_t pi = page_of(h, (uintptr_t)p);
        struct gc_page *d = &h->pages[pi];
        unsigned slot = (unsigned)(((uintptr_t)p % GC_PAGE) / d->size);
        d->alloc[slot / 64] |= 1ull << (slot % 64);
        memset(p, 0, d->size);
        h->since += d->size;
        h->st.allocated_objects++;
        h->st.allocated_bytes += d->size;
        return p;
    }
    if (n > (size_t)h->npages * GC_PAGE) return NULL;
    uint32_t k = (uint32_t)((n + GC_PAGE - 1) / GC_PAGE);
    uint32_t pi = take_pages(h, k);
    if (pi == GC_NONE) return NULL;
    for (uint32_t j = 0; j < k; j++) {
        struct gc_page *d = &h->pages[pi + j];
        memset(d, 0, sizeof *d);
        d->kind = j ? GP_CONT : GP_LARGE;
        d->run = j ? j : k;
    }
    h->pages[pi].alloc[0] = 1;
    void *p = (void *)page_addr(h, pi);
    memset(p, 0, (size_t)k * GC_PAGE);
    h->since += (size_t)k * GC_PAGE;
    h->st.heap_pages += k;
    h->st.allocated_objects++;
    h->st.allocated_bytes += (size_t)k * GC_PAGE;
    return p;
}

bool gc_heap_want_collect(const struct gc_heap *h)
{
    size_t live = h->st.live_bytes;
    return h->since >= (live > h->trigger_min ? live : h->trigger_min);
}

/* ------------------------------------------------- pointer identification */

/* The conservative test, shared by marking and the queries.  A word is a
 * pointer to object X if it lies in X's slot: [start, start + slot size)
 * for small objects, the object's whole pages for large ones.  With
 * interior pointers off, only start itself counts.  A pointer one past the
 * end of X is the start of the next slot, so it never keeps X alive.
 * Words that land in a free slot, in the unused tail of a page or in a
 * free page are not pointers. */
static inline uintptr_t lookup(const struct gc_heap *h, uintptr_t w, struct gc_page **dp, unsigned *slotp)
{
    uintptr_t off = w - h->base;               /* wraps for w < base */
    if (off >= (uintptr_t)h->top * GC_PAGE) return 0;
    size_t pi = off / GC_PAGE;
    struct gc_page *d = &h->pages[pi];
    uintptr_t in = off % GC_PAGE;
    if (d->kind == GP_SMALL) {
        unsigned slot = (unsigned)(in / d->size);
        if (slot >= d->nobj) return 0;
        uintptr_t start = (uintptr_t)slot * d->size;
        if (start != in && !h->interior) return 0;
        if (!(d->alloc[slot / 64] & (1ull << (slot % 64)))) return 0;
        *dp = d;
        *slotp = slot;
        return page_addr(h, pi) + start;
    }
    if (d->kind == GP_CONT) {
        if (!h->interior) return 0;
        pi -= d->run;
        d = &h->pages[pi];
        in = 1;
    }
    if (d->kind != GP_LARGE || !(d->alloc[0] & 1)) return 0;
    if (in && !h->interior) return 0;
    *dp = d;
    *slotp = 0;
    return page_addr(h, pi);
}

uintptr_t gc_heap_find(const struct gc_heap *h, uintptr_t w)
{
    struct gc_page *d;
    unsigned slot;
    return lookup(h, w, &d, &slot);
}

bool gc_heap_is_allocated(const struct gc_heap *h, const void *p)
{
    struct gc_page *d;
    unsigned slot;
    return p && lookup(h, (uintptr_t)p, &d, &slot) == (uintptr_t)p;
}

/* ------------------------------------------------------------- marking */

static inline void mark_word(struct gc_heap *h, uintptr_t w)
{
    struct gc_page *d;
    unsigned slot;
    uintptr_t obj = lookup(h, w, &d, &slot);
    if (!obj) return;
    uint64_t bit = 1ull << (slot % 64);
    if (d->mark[slot / 64] & bit) return;
    d->mark[slot / 64] |= bit;
    if (h->msp < h->mcap) {
        h->mstack[h->msp++] = obj;
    } else {
        h->overflow = true;          /* marked but not scanned: mark_finish rescans */
        h->st.mark_overflows++;
    }
}

static void scan_words(struct gc_heap *h, uintptr_t lo, uintptr_t hi)
{
    lo = (lo + 7) & ~(uintptr_t)7;
    hi &= ~(uintptr_t)7;
    for (const uintptr_t *p = (const uintptr_t *)lo; (uintptr_t)p < hi; p++) mark_word(h, *p);
}

static void scan_object(struct gc_heap *h, uintptr_t obj)
{
    const struct gc_page *d = &h->pages[page_of(h, obj)];
    size_t len = d->kind == GP_SMALL ? d->size : (size_t)d->run * GC_PAGE;
    scan_words(h, obj, obj + len);
}

static void drain(struct gc_heap *h)
{
    while (h->msp) scan_object(h, h->mstack[--h->msp]);
}

void gc_heap_mark_range(struct gc_heap *h, const void *lo, const void *hi)
{
    if ((uintptr_t)lo < (uintptr_t)hi) scan_words(h, (uintptr_t)lo, (uintptr_t)hi);
    drain(h);
}

/* After an overflow some marked objects were never scanned.  Rescanning
 * every marked object finds their children; repeat until a pass ends
 * without overflow.  Each pass that overflows has marked something new,
 * so this terminates.  (The same recovery as the Boehm collector's.) */
void gc_heap_mark_finish(struct gc_heap *h)
{
    drain(h);
    while (h->overflow) {
        h->overflow = false;
        for (uint32_t pi = 0; pi < h->top; pi++) {
            const struct gc_page *d = &h->pages[pi];
            if (d->kind == GP_SMALL) {
                for (unsigned s = 0; s < d->nobj; s++)
                    if (d->mark[s / 64] & (1ull << (s % 64))) {
                        scan_object(h, page_addr(h, pi) + (uintptr_t)s * d->size);
                        drain(h);
                    }
            } else if (d->kind == GP_LARGE && (d->mark[0] & 1)) {
                scan_object(h, page_addr(h, pi));
                drain(h);
            }
        }
    }
}

/* ------------------------------------------------------------- sweeping */

static void rebuild_free_runs(struct gc_heap *h)
{
    uint32_t *tail = &h->free_runs;
    uint32_t i = 0;
    while (i < h->top) {
        if (h->pages[i].kind != GP_FREE) {
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < h->top && h->pages[j].kind == GP_FREE) j++;
        if (j == h->top) {           /* a free tail goes back to the bump region */
            h->top = i;
            break;
        }
        h->pages[i].run = j - i;
        *tail = i;
        tail = &h->pages[i].next;
        i = j;
    }
    *tail = GC_NONE;
}

void gc_heap_sweep(struct gc_heap *h)
{
    uint64_t live_obj = 0, live_bytes = 0, freed_obj = 0, freed_bytes = 0;
    for (int c = 0; c < GC_NCLASS; c++) h->freelist[c] = NULL;
    for (uint32_t pi = 0; pi < h->top; pi++) {
        struct gc_page *d = &h->pages[pi];
        if (d->kind == GP_SMALL) {
            unsigned live = 0, dead = 0;
            for (int w = 0; w < 4; w++) {
                dead += (unsigned)__builtin_popcountll(d->alloc[w] & ~d->mark[w]);
                d->alloc[w] &= d->mark[w];
                d->mark[w] = 0;
                live += (unsigned)__builtin_popcountll(d->alloc[w]);
            }
            freed_obj += dead;
            freed_bytes += (uint64_t)dead * d->size;
            if (!live) {
                d->kind = GP_FREE;
                h->st.heap_pages--;
                continue;
            }
            live_obj += live;
            live_bytes += (uint64_t)live * d->size;
            char *p = (char *)page_addr(h, pi);
            for (int s = d->nobj - 1; s >= 0; s--)
                if (!(d->alloc[s / 64] & (1ull << (s % 64)))) {
                    void **slot = (void **)(p + (size_t)s * d->size);
                    *slot = h->freelist[d->cls];
                    h->freelist[d->cls] = slot;
                }
        } else if (d->kind == GP_LARGE) {
            uint32_t k = d->run;
            if (d->mark[0] & 1) {
                d->mark[0] = 0;
                live_obj++;
                live_bytes += (uint64_t)k * GC_PAGE;
            } else {
                for (uint32_t j = 0; j < k; j++) h->pages[pi + j].kind = GP_FREE;
                h->st.heap_pages -= k;
                freed_obj++;
                freed_bytes += (uint64_t)k * GC_PAGE;
            }
            pi += k - 1;
        }
    }
    rebuild_free_runs(h);
    h->since = 0;
    h->st.collections++;
    h->st.live_objects = live_obj;
    h->st.live_bytes = live_bytes;
    h->st.freed_objects += freed_obj;
    h->st.freed_bytes += freed_bytes;
}
