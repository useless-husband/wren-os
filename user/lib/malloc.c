/* The malloc heap (first-fit free list with coalescing, grown with sbrk)
 * and a leak checker built on the garbage collector's roots.
 *
 * Every block has a 16-byte header: the payload size, whose low four bits
 * are free for flags, and a second word that is the free-list link while
 * the block is free and the allocation site while it is allocated (the
 * return address of the malloc, calloc or realloc call).  The memory from
 * sbrk is kept as a list of regions, and blocks tile each region without
 * gaps, so the whole heap can be walked block by block. */
#include "ulib.h"
#include "roots.h"

struct block {
    size_t size;                /* payload bytes (a multiple of 16) | B_* flags */
    union {
        struct block *next;     /* free: next free block, address order */
        uintptr_t     site;     /* allocated: caller of malloc */
    };
};
#define HDR     sizeof(struct block)
#define B_ALLOC 1ul
#define B_MARK  2ul             /* leak checker: reached from a root */
#define B_FLAGS 15ul

/* A stretch of memory from consecutive sbrk calls; its blocks follow the header. */
struct region {
    struct region *next;        /* address order */
    char          *end;
};

static struct block  *free_list;
static struct region *regions, *last_region;

static inline size_t bsize(const struct block *b) { return b->size & ~B_FLAGS; }
static inline struct block *first_block(struct region *r) { return (struct block *)(r + 1); }
static inline struct block *next_block(struct block *b) { return (struct block *)((char *)b + HDR + bsize(b)); }

static void insert_free(struct block *b)
{
    struct block *prev = NULL, *cur = free_list;
    while (cur && cur < b) {
        prev = cur;
        cur = cur->next;
    }
    b->next = cur;
    if (prev) prev->next = b;
    else free_list = b;
    if (cur && (char *)b + HDR + b->size == (char *)cur) {         /* merge with the next block */
        b->size += HDR + cur->size;
        b->next = cur->next;
    }
    if (prev && (char *)prev + HDR + prev->size == (char *)b) {   /* and with the previous one */
        prev->size += HDR + b->size;
        prev->next = b->next;
    }
}

/* Add at least n payload bytes to the heap.  sbrk memory that continues
 * the last region extends it, so blocks on both sides can merge; anything
 * else (someone else moved the break) starts a new region. */
static bool grow(size_t n)
{
    size_t want = n + HDR + sizeof(struct region);
    size_t len = want < 65536 ? 65536 : (want + 4095) & ~(size_t)4095;
    char *p = sbrk((long)len);
    if (sbrk_failed(p)) return false;
    struct block *b;
    if (last_region && p == last_region->end) {
        last_region->end = p + len;
        b = (struct block *)p;
        b->size = len - HDR;
    } else {
        struct region *r = (struct region *)p;
        r->next = NULL;
        r->end = p + len;
        if (last_region) last_region->next = r;
        else regions = r;
        last_region = r;
        b = first_block(r);
        b->size = len - sizeof *r - HDR;
    }
    insert_free(b);
    return true;
}

static void *malloc_at(size_t n, uintptr_t site)
{
    if (n == 0) n = 1;
    if (n > (1ul << 40)) return NULL;
    n = (n + 15) & ~(size_t)15;
    for (;;) {
        for (struct block **pp = &free_list; *pp; pp = &(*pp)->next) {
            struct block *b = *pp;
            if (b->size < n) continue;
            if (b->size >= n + HDR + 32) {           /* split */
                struct block *rest = (struct block *)((char *)b + HDR + n);
                rest->size = b->size - n - HDR;
                rest->next = b->next;
                b->size = n;
                *pp = rest;
            } else {
                *pp = b->next;
            }
            b->size |= B_ALLOC;
            b->site = site;
            return (char *)b + HDR;
        }
        if (!grow(n)) return NULL;
    }
}

#define CALLER() ((uintptr_t)__builtin_return_address(0))

void *malloc(size_t n) { return malloc_at(n, CALLER()); }

void free(void *ptr)
{
    if (!ptr) return;
    struct block *b = (struct block *)((char *)ptr - HDR);
    if (!(b->size & B_ALLOC)) {      /* would corrupt the free list and the heap walk */
        fprintf(2, "free: %p is not an allocated block (double free?)\n", ptr);
        return;
    }
    b->size &= ~B_FLAGS;
    insert_free(b);
}

void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) return NULL;
    void *p = malloc_at(n * size, CALLER());
    if (p) memset(p, 0, n * size);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    if (!ptr) return malloc_at(n, CALLER());
    struct block *b = (struct block *)((char *)ptr - HDR);
    if (bsize(b) >= n) return ptr;
    void *q = malloc_at(n, CALLER());
    if (q) {
        memcpy(q, ptr, bsize(b));
        free(ptr);
    }
    return q;
}

/* ---- leak checker ----
 *
 * The garbage collector's mark phase, run over the malloc heap: mark every
 * block that a root word points into (anywhere in its payload), then every
 * block that a marked block points into.  Allocated blocks left unmarked
 * are leaks: nothing the program can still reach refers to them.  Like
 * LeakSanitizer this is conservative: an integer that happens to look like
 * a pointer hides a leak, never invents one. */

struct leakscan {
    uintptr_t *start;           /* payload address of every allocated block, ascending */
    uintptr_t *stack;           /* mark stack; each block is pushed at most once */
    size_t     n, sp;
    uintptr_t  limit;           /* end of the highest block */
};

static void leak_mark_word(struct leakscan *s, uintptr_t w)
{
    if (w < s->start[0] || w >= s->limit) return;
    size_t lo = 0, hi = s->n;    /* the last block starting at or below w */
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->start[mid] <= w) lo = mid;
        else hi = mid;
    }
    struct block *b = (struct block *)(s->start[lo] - HDR);
    if (w >= s->start[lo] + bsize(b) || (b->size & B_MARK)) return;
    b->size |= B_MARK;
    s->stack[s->sp++] = s->start[lo];
}

static void leak_scan_words(struct leakscan *s, uintptr_t lo, uintptr_t hi)
{
    lo = (lo + 7) & ~(uintptr_t)7;
    hi &= ~(uintptr_t)7;
    for (const uintptr_t *p = (const uintptr_t *)lo; (uintptr_t)p < hi; p++) leak_mark_word(s, *p);
}

static void leak_scan_root(void *ctx, const void *lo, const void *hi)
{
    struct leakscan *s = ctx;
    if ((uintptr_t)lo < (uintptr_t)hi) leak_scan_words(s, (uintptr_t)lo, (uintptr_t)hi);
    while (s->sp) {
        uintptr_t a = s->stack[--s->sp];
        leak_scan_words(s, a, a + bsize((struct block *)(a - HDR)));
    }
}

/* The block table lives in fresh sbrk memory: outside every region and
 * every root, so it can neither be reported nor keep anything alive.  This
 * runs in its own frame, gone before the roots are scanned, so no stale
 * block address is left in a register or a live stack slot. */
static __attribute__((noinline)) size_t leak_table(struct leakscan *s)
{
    size_t n = 0;
    for (struct region *r = regions; r; r = r->next)
        for (struct block *b = first_block(r); (char *)b < r->end; b = next_block(b))
            n += b->size & B_ALLOC;
    s->n = n;
    s->sp = 0;
    if (!n) return 0;
    size_t len = (2 * n * sizeof(uintptr_t) + 4095) & ~(size_t)4095;
    char *mem = sbrk((long)len);
    if (sbrk_failed(mem)) return (size_t)-1;
    s->start = (uintptr_t *)mem;
    s->stack = s->start + n;
    size_t i = 0;
    for (struct region *r = regions; r; r = r->next)
        for (struct block *b = first_block(r); (char *)b < r->end; b = next_block(b))
            if (b->size & B_ALLOC) {
                s->start[i++] = (uintptr_t)b + HDR;
                s->limit = (uintptr_t)b + HDR + bsize(b);
            }
    return len;
}

static __attribute__((noinline)) long leak_check_scrubbed(void)
{
    struct leakscan s = {0};
    size_t len = leak_table(&s);
    if (len == (size_t)-1) {
        fprintf(2, "leakcheck: out of memory for the block table\n");
        return -1;
    }
    if (len) roots_scan(leak_scan_root, &s, NULL, NULL);
    long leaked = 0, reach = 0;
    size_t leaked_bytes = 0, reach_bytes = 0;
    for (struct region *r = regions; r; r = r->next)
        for (struct block *b = first_block(r); (char *)b < r->end; b = next_block(b)) {
            if (!(b->size & B_ALLOC)) continue;
            if (b->size & B_MARK) {
                b->size &= ~B_MARK;
                reach++;
                reach_bytes += bsize(b);
            } else {
                leaked++;
                leaked_bytes += bsize(b);
                fprintf(2, "leak: %lu bytes at %p, allocated from pc %p\n", (unsigned long)bsize(b),
                        (void *)((char *)b + HDR), (void *)b->site);
            }
        }
    if (leaked)
        fprintf(2, "leakcheck: %ld leaked blocks, %lu bytes; %ld blocks, %lu bytes still reachable\n",
                leaked, (unsigned long)leaked_bytes, reach, (unsigned long)reach_bytes);
    else
        fprintf(2, "leakcheck: no leaks; %ld blocks, %lu bytes still reachable\n", reach,
                (unsigned long)reach_bytes);
    if (len && (char *)sbrk(0) == (char *)s.start + len) sbrk(-(long)len);
    return leaked;
}

/* Zero the dead stack below the caller.  The frames of the checker are
 * built on that memory next; without this, an uninitialised slot in one
 * of them could still hold a pointer left by some earlier, deeper call
 * and hide a leak. */
static __attribute__((noinline)) void scrub_stack(void)
{
    volatile char pad[4096];
    for (size_t i = 0; i < sizeof pad; i++) pad[i] = 0;
}

long leak_check(void)
{
    scrub_stack();
    return leak_check_scrubbed();
}
