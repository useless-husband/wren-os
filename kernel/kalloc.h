/* Physical pages (buddy allocator + reference counts) and the kernel heap. */
#ifndef KERNEL_KALLOC_H
#define KERNEL_KALLOC_H
#include "types.h"

struct platform;
struct fdt;

void     kalloc_init(const struct platform *pf, const struct fdt *f, paddr_t img_start, paddr_t img_end,
                     paddr_t dtb, size_t dtb_size);
paddr_t  page_alloc(void);              /* one zeroed page with refcount 1, or 0 */
paddr_t  pages_alloc(unsigned order);   /* 2^order contiguous zeroed pages, or 0 */
void     pages_free(paddr_t pa, unsigned order);
void     page_get(paddr_t pa);          /* another mapping now shares the page */
void     page_put(paddr_t pa);          /* drop a reference; frees the page at zero */
int      page_refcount(paddr_t pa);
uint64_t pages_free_count(void);
uint64_t pages_total(void);

void    *kmalloc(size_t n);             /* zeroed; NULL when out of memory */
void     kfree(void *p);

#endif
