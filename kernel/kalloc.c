#include "kalloc.h"
#include "buddy.h"
#include "fdt.h"
#include "memlayout.h"
#include "platform.h"
#include "printk.h"
#include "spinlock.h"
#include "../lib/kstring.h"

static struct buddy mem;
static struct spinlock mem_lock = SPINLOCK_INIT("pages");
static uint64_t total_pages;

/* RAM above 4 GiB is not covered by the boot linear map; we do not need it. */
#define RAM_LIMIT (4UL << 30)

static bool overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return a < b + blen && b < a + alen;
}

void kalloc_init(const struct platform *pf, const struct fdt *f, paddr_t img_start, paddr_t img_end,
                 paddr_t dtb, size_t dtb_size)
{
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < pf->nmem; i++) {
        uint64_t end = MIN(pf->mem_base[i] + pf->mem_size[i], RAM_LIMIT);
        if (pf->mem_base[i] >= end) continue;
        lo = MIN(lo, pf->mem_base[i]);
        hi = MAX(hi, end);
    }
    if (lo >= hi) panic("no usable RAM below 4 GiB");
    uint64_t block = (uint64_t)BUDDY_PAGE << BUDDY_MAX_ORDER;
    uint64_t base = ROUNDDOWN(lo, block);
    uint32_t npages = (uint32_t)((ROUNDUP(hi, PAGE_SIZE) - base) / PAGE_SIZE);
    uint64_t meta_bytes = ROUNDUP((uint64_t)npages * sizeof(struct page), PAGE_SIZE);

    /* The page array goes in the first RAM after the image that is clear of the DTB. */
    paddr_t meta = ROUNDUP(img_end, PAGE_SIZE);
    if (overlaps(meta, meta_bytes, dtb, dtb_size)) meta = ROUNDUP(dtb + dtb_size, PAGE_SIZE);
    buddy_init(&mem, base, npages, P2V(meta));

    /* Hand over every RAM page except the image, the DTB, the page array
     * and /memreserve/ ranges. */
    const uint64_t busy[3][2] = {{img_start, img_end - img_start}, {dtb, dtb_size}, {meta, meta_bytes}};
    for (int i = 0; i < pf->nmem; i++) {
        uint64_t end = MIN(pf->mem_base[i] + pf->mem_size[i], RAM_LIMIT);
        for (uint64_t a = ROUNDUP(pf->mem_base[i], PAGE_SIZE); a + PAGE_SIZE <= end; a += PAGE_SIZE) {
            bool used = false;
            for (int k = 0; k < 3; k++) used |= overlaps(a, PAGE_SIZE, busy[k][0], busy[k][1]);
            for (int k = 0; k < f->nrsv; k++) used |= overlaps(a, PAGE_SIZE, f->rsv_addr[k], f->rsv_size[k]);
            if (!used) buddy_add_range(&mem, a, PAGE_SIZE);
        }
    }
    total_pages = mem.nfree;
}

paddr_t pages_alloc(unsigned order)
{
    spin_lock(&mem_lock);
    paddr_t pa = buddy_alloc(&mem, order);
    spin_unlock(&mem_lock);
    if (pa) memset(P2V(pa), 0, PAGE_SIZE << order);
    return pa;
}

paddr_t page_alloc(void) { return pages_alloc(0); }

void pages_free(paddr_t pa, unsigned order)
{
    spin_lock(&mem_lock);
    int r = buddy_free(&mem, pa, order);
    spin_unlock(&mem_lock);
    if (r < 0) panic("pages_free: bad or double free of %p order %u", (void *)pa, order);
}

void page_get(paddr_t pa)
{
    struct page *p = buddy_page(&mem, pa);
    if (!p || p->state != PG_ALLOC) panic("page_get: %p not allocated", (void *)pa);
    __atomic_fetch_add(&p->ref, 1, __ATOMIC_RELAXED);
}

void page_put(paddr_t pa)
{
    struct page *p = buddy_page(&mem, pa);
    if (!p || p->state != PG_ALLOC || p->order != 0) panic("page_put: %p not an allocated page", (void *)pa);
    int32_t left = __atomic_sub_fetch(&p->ref, 1, __ATOMIC_ACQ_REL);
    if (left < 0) panic("page_put: refcount underflow on %p", (void *)pa);
    if (left == 0) pages_free(pa, 0);
}

int page_refcount(paddr_t pa)
{
    struct page *p = buddy_page(&mem, pa);
    return p ? __atomic_load_n(&p->ref, __ATOMIC_ACQUIRE) : 0;
}

uint64_t pages_free_count(void) { return __atomic_load_n(&mem.nfree, __ATOMIC_RELAXED); }
uint64_t pages_total(void) { return total_pages; }

/* ---- kernel heap: power-of-two size classes carved out of pages ---- */

#define NCLASS 7                        /* 32 .. 2048 bytes */
struct freeobj { struct freeobj *next; };
static struct freeobj *heap_free[NCLASS];
static struct spinlock heap_lock = SPINLOCK_INIT("kheap");

static int size_class(size_t n)
{
    int c = 0;
    for (size_t sz = 32; sz < n; sz <<= 1) c++;
    return c;
}

void *kmalloc(size_t n)
{
    if (n == 0) n = 1;
    if (n > 2048) {
        unsigned order = 0;
        while ((PAGE_SIZE << order) < n) order++;
        paddr_t pa = pages_alloc(order);
        return pa ? P2V(pa) : NULL;
    }
    int c = size_class(n);
    size_t sz = 32u << c;
    spin_lock(&heap_lock);
    if (!heap_free[c]) {
        spin_unlock(&heap_lock);
        paddr_t pa = page_alloc();
        if (!pa) return NULL;
        buddy_page(&mem, pa)->slab = (uint16_t)(c + 1);
        char *base = P2V(pa);
        spin_lock(&heap_lock);
        for (size_t off = 0; off + sz <= PAGE_SIZE; off += sz) {
            struct freeobj *o = (struct freeobj *)(base + off);
            o->next = heap_free[c];
            heap_free[c] = o;
        }
    }
    struct freeobj *o = heap_free[c];
    heap_free[c] = o->next;
    spin_unlock(&heap_lock);
    memset(o, 0, sz);
    return o;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    paddr_t pa = V2P(ptr);
    struct page *p = buddy_page(&mem, ROUNDDOWN(pa, PAGE_SIZE));
    if (!p) panic("kfree: %p is not heap memory", ptr);
    if (p->state == PG_ALLOC && p->slab) {
        int c = p->slab - 1;
        if ((pa & ((32u << c) - 1)) != 0) panic("kfree: %p misaligned for class %d", ptr, c);
        spin_lock(&heap_lock);
        struct freeobj *o = ptr;
        o->next = heap_free[c];
        heap_free[c] = o;
        spin_unlock(&heap_lock);
    } else {
        pages_free(pa, p->order);
    }
}
