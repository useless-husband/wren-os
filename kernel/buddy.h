/* Binary buddy allocator for physical pages.
 *
 * Pages are tracked by index from `base`, which must be aligned to the
 * largest block (2^BUDDY_MAX_ORDER pages) so that a block's buddy is found by
 * flipping one bit of its index.  Every page starts out reserved; memory is
 * handed over with buddy_add_range().  No locking: the kernel wraps calls in
 * a spinlock (kalloc.c), the host tests call it directly. */
#ifndef KERNEL_BUDDY_H
#define KERNEL_BUDDY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUDDY_MAX_ORDER 10            /* largest block: 1024 pages = 4 MiB */
#define BUDDY_PAGE      4096u
#define BUDDY_NIL       0xffffffffu

/* PG_FREE/PG_ALLOC mark the first page of a block, PG_TAIL its other pages. */
enum { PG_RESERVED = 0, PG_FREE = 1, PG_ALLOC = 2, PG_TAIL = 3 };

struct page {
    uint32_t next, prev;     /* free-list links (page indices) */
    int32_t  ref;            /* references to an allocated block (copy-on-write sharing) */
    uint8_t  order;          /* block order, valid on the first page of a block */
    uint8_t  state;          /* PG_* */
    uint16_t slab;           /* kmalloc size class + 1 for slab pages, 0 otherwise */
};

struct buddy {
    uint64_t     base;       /* physical address of page index 0 */
    uint32_t     npages;
    struct page *pages;      /* npages entries */
    uint32_t     free_head[BUDDY_MAX_ORDER + 1];
    uint64_t     nfree;      /* free pages */
};

void     buddy_init(struct buddy *b, uint64_t base, uint32_t npages, struct page *meta);
void     buddy_add_range(struct buddy *b, uint64_t pa, uint64_t len);
uint64_t buddy_alloc(struct buddy *b, unsigned order);          /* 0 when out of memory */
int      buddy_free(struct buddy *b, uint64_t pa, unsigned order); /* -1 on a bad/double free */
struct page *buddy_page(struct buddy *b, uint64_t pa);          /* NULL if not managed */
/* Verify every invariant (free lists consistent, no overlapping blocks,
 * no two free buddies left uncoalesced, nfree correct). */
bool     buddy_check(const struct buddy *b, char *why, size_t whylen);

#endif
