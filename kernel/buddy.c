#include "buddy.h"
#include "../lib/fmt.h"

static void list_push(struct buddy *b, unsigned order, uint32_t idx)
{
    struct page *p = &b->pages[idx];
    p->state = PG_FREE;
    p->order = (uint8_t)order;
    p->prev = BUDDY_NIL;
    p->next = b->free_head[order];
    if (p->next != BUDDY_NIL) b->pages[p->next].prev = idx;
    b->free_head[order] = idx;
}

static void list_remove(struct buddy *b, unsigned order, uint32_t idx)
{
    struct page *p = &b->pages[idx];
    if (p->prev != BUDDY_NIL) b->pages[p->prev].next = p->next;
    else b->free_head[order] = p->next;
    if (p->next != BUDDY_NIL) b->pages[p->next].prev = p->prev;
    p->next = p->prev = BUDDY_NIL;
}

void buddy_init(struct buddy *b, uint64_t base, uint32_t npages, struct page *meta)
{
    b->base = base;
    b->npages = npages;
    b->pages = meta;
    b->nfree = 0;
    for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++) b->free_head[o] = BUDDY_NIL;
    for (uint32_t i = 0; i < npages; i++)
        meta[i] = (struct page){.next = BUDDY_NIL, .prev = BUDDY_NIL, .state = PG_RESERVED};
}

struct page *buddy_page(struct buddy *b, uint64_t pa)
{
    if (pa < b->base || (pa - b->base) / BUDDY_PAGE >= b->npages) return NULL;
    return &b->pages[(pa - b->base) / BUDDY_PAGE];
}

uint64_t buddy_alloc(struct buddy *b, unsigned order)
{
    if (order > BUDDY_MAX_ORDER) return 0;
    unsigned k = order;
    while (k <= BUDDY_MAX_ORDER && b->free_head[k] == BUDDY_NIL) k++;
    if (k > BUDDY_MAX_ORDER) return 0;
    uint32_t idx = b->free_head[k];
    list_remove(b, k, idx);
    while (k > order) {              /* split, keeping the lower half */
        k--;
        list_push(b, k, idx + (1u << k));
    }
    struct page *p = &b->pages[idx];
    p->state = PG_ALLOC;
    p->order = (uint8_t)order;
    p->ref = 1;
    p->slab = 0;
    for (uint32_t i = 1; i < (1u << order); i++) b->pages[idx + i].state = PG_TAIL;
    b->nfree -= 1u << order;
    return b->base + (uint64_t)idx * BUDDY_PAGE;
}

int buddy_free(struct buddy *b, uint64_t pa, unsigned order)
{
    struct page *p = buddy_page(b, pa);
    if (!p || (pa & (BUDDY_PAGE - 1)) || order > BUDDY_MAX_ORDER) return -1;
    uint32_t idx = (uint32_t)(p - b->pages);
    if (p->state != PG_ALLOC || p->order != order || (idx & ((1u << order) - 1))) return -1;
    if (idx + (1u << order) > b->npages) return -1;
    b->nfree += 1u << order;
    p->ref = 0;
    p->slab = 0;
    while (order < BUDDY_MAX_ORDER) {
        uint32_t bud = idx ^ (1u << order);
        if (bud >= b->npages || b->pages[bud].state != PG_FREE || b->pages[bud].order != order) break;
        list_remove(b, order, bud);
        b->pages[bud].state = PG_TAIL;     /* both heads become interior pages... */
        b->pages[idx].state = PG_TAIL;
        if (bud < idx) idx = bud;          /* ...of the merged block headed here */
        order++;
    }
    list_push(b, order, idx);
    return 0;
}

void buddy_add_range(struct buddy *b, uint64_t pa, uint64_t len)
{
    uint64_t start = (pa + BUDDY_PAGE - 1) & ~(uint64_t)(BUDDY_PAGE - 1);
    uint64_t end = (pa + len) & ~(uint64_t)(BUDDY_PAGE - 1);
    for (uint64_t a = start; a < end; a += BUDDY_PAGE) {
        struct page *p = buddy_page(b, a);
        if (!p || p->state != PG_RESERVED) continue;
        p->state = PG_ALLOC;           /* pretend it was allocated, then free it */
        p->order = 0;
        buddy_free(b, a, 0);
    }
}

bool buddy_check(const struct buddy *b, char *why, size_t whylen)
{
    uint64_t counted = 0;
    for (unsigned o = 0; o <= BUDDY_MAX_ORDER; o++) {
        uint32_t prev = BUDDY_NIL;
        for (uint32_t i = b->free_head[o]; i != BUDDY_NIL; i = b->pages[i].next) {
            const struct page *p = &b->pages[i];
            if (i >= b->npages || p->state != PG_FREE || p->order != o || p->prev != prev ||
                (i & ((1u << o) - 1)) || i + (1u << o) > b->npages) {
                snformat(why, whylen, "free list %u corrupt at page %u", o, i);
                return false;
            }
            for (uint32_t j = 1; j < (1u << o); j++)
                if (b->pages[i + j].state != PG_TAIL) {
                    snformat(why, whylen, "free block %u/%u overlaps page %u", i, o, i + j);
                    return false;
                }
            uint32_t bud = i ^ (1u << o);
            if (o < BUDDY_MAX_ORDER && bud < b->npages && b->pages[bud].state == PG_FREE &&
                b->pages[bud].order == o) {
                snformat(why, whylen, "buddies %u and %u (order %u) not coalesced", i, bud, o);
                return false;
            }
            counted += 1u << o;
            prev = i;
            if (counted > b->npages) {
                snformat(why, whylen, "free list %u loops", o);
                return false;
            }
        }
    }
    if (counted != b->nfree) {
        snformat(why, whylen, "nfree %llu but lists hold %llu", (unsigned long long)b->nfree,
                 (unsigned long long)counted);
        return false;
    }
    return true;
}
