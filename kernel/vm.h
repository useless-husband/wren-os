/* Page tables: the kernel's TTBR1 tables and one TTBR0 address space per process. */
#ifndef KERNEL_VM_H
#define KERNEL_VM_H
#include "types.h"

struct platform;

#define VM_R 1
#define VM_W 2
#define VM_X 4

struct vmspace {
    paddr_t  root;          /* level-0 table */
    uint16_t asid;          /* 1..255; TLB entries of this space carry it */
    vaddr_t  heap_start;    /* end of the loaded program, page aligned */
    vaddr_t  brk;           /* current break: [heap_start, brk) is lazily backed */
    uint64_t resident;      /* user pages currently mapped */
};

void kvm_init(const struct platform *pf, paddr_t img_pa);
void kvm_install(void);                          /* on every CPU: final TTBR1, empty TTBR0 */
paddr_t kvm_empty_table(void);

int  vm_create(struct vmspace *vs);
void vm_destroy(struct vmspace *vs);
void vm_activate(struct vmspace *vs);            /* NULL: no user space */
int  vm_map(struct vmspace *vs, vaddr_t va, paddr_t pa, int prot);
int  vm_clone_cow(struct vmspace *dst, struct vmspace *src);
int  vm_fault(struct vmspace *vs, vaddr_t va, bool write, bool exec);
int  vm_set_brk(struct vmspace *vs, vaddr_t newbrk);
void vm_sync_icache(struct vmspace *vs, vaddr_t va, size_t len);

/* Copy between kernel memory and a (not necessarily current) address space.
 * They walk the page table in software and fault pages in as needed, so the
 * kernel never takes a page fault itself.  Return 0 or -EFAULT. */
int copyout(struct vmspace *vs, vaddr_t dst, const void *src, size_t len);
int copyin(struct vmspace *vs, void *dst, vaddr_t src, size_t len);
int copyinstr(struct vmspace *vs, char *dst, vaddr_t src, size_t max);   /* -ENAMETOOLONG if no NUL */

#endif
