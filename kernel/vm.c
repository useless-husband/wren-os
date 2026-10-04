#include "vm.h"
#include "arch.h"
#include "kalloc.h"
#include "memlayout.h"
#include "platform.h"
#include "printk.h"
#include "spinlock.h"
#include "stats.h"
#include <wren/errno.h>
#include "../lib/kstring.h"

#define LEVEL_SHIFT(l) (39 - 9 * (l))     /* level 0..3 */
#define IDX(va, l)     (((va) >> LEVEL_SHIFT(l)) & 511)
#define BLOCK_2M       (1UL << 21)

static paddr_t kroot;          /* kernel TTBR1 level-0 table */
static paddr_t empty_table;    /* all-invalid table: TTBR0 when no process runs */

/* ---------------------------------------------------------------- TLB */

static inline void tlb_flush_asid(uint16_t asid)
{
    dsb_ishst();
    __asm__ volatile("tlbi aside1is, %0" ::"r"((uint64_t)asid << 48) : "memory");
    dsb_ish();
    isb();
}

static inline void tlb_flush_page(uint16_t asid, vaddr_t va)
{
    dsb_ishst();
    __asm__ volatile("tlbi vae1is, %0" ::"r"(((uint64_t)asid << 48) | ((va >> 12) & 0xfffffffffffUL)) : "memory");
    dsb_ish();
    isb();
}

/* ---------------------------------------------------------- ASIDs */

static uint64_t asid_map[4];   /* 256 bits; ASID 0 is never handed out */
static struct spinlock asid_lock = SPINLOCK_INIT("asid");

static uint16_t asid_alloc(void)
{
    spin_lock(&asid_lock);
    for (int a = 1; a < 256; a++) {
        if (!(asid_map[a / 64] & (1UL << (a % 64)))) {
            asid_map[a / 64] |= 1UL << (a % 64);
            spin_unlock(&asid_lock);
            return (uint16_t)a;
        }
    }
    spin_unlock(&asid_lock);
    return 0;
}

static void asid_release(uint16_t a)
{
    tlb_flush_asid(a);          /* no stale entry may outlive the ASID's owner */
    spin_lock(&asid_lock);
    asid_map[a / 64] &= ~(1UL << (a % 64));
    spin_unlock(&asid_lock);
}

/* ------------------------------------------------------ table walks */

/* Return the descriptor slot for va at `level`, allocating intermediate
 * tables when `alloc`.  NULL if missing (or a block mapping is in the way). */
static pte_t *walk_to(paddr_t root, vaddr_t va, int level, bool alloc)
{
    pte_t *table = P2V(root);
    for (int l = 0; l < level; l++) {
        pte_t *e = &table[IDX(va, l)];
        if (*e & PTE_VALID) {
            if (!(*e & PTE_TABLE)) return NULL;
            table = P2V(*e & PTE_ADDR_MASK);
        } else {
            if (!alloc) return NULL;
            paddr_t t = page_alloc();
            if (!t) return NULL;
            dsb_ishst();             /* the zeroed table is visible before it is linked */
            *e = t | PTE_VALID | PTE_TABLE;
            table = P2V(t);
        }
    }
    return &table[IDX(va, level)];
}

static pte_t *walk(paddr_t root, vaddr_t va, bool alloc) { return walk_to(root, va, 3, alloc); }

/* ---------------------------------------------------- kernel tables */

static void kmap(vaddr_t va, paddr_t pa, uint64_t size, uint64_t attrs)
{
    vaddr_t end = va + ROUNDUP(size, PAGE_SIZE);
    va = ROUNDDOWN(va, PAGE_SIZE);
    pa = ROUNDDOWN(pa, PAGE_SIZE);
    while (va < end) {
        pte_t *e;
        if (!(va & (BLOCK_2M - 1)) && !(pa & (BLOCK_2M - 1)) && end - va >= BLOCK_2M &&
            (e = walk_to(kroot, va, 2, true)) && !(*e & PTE_VALID)) {
            *e = pa | attrs | PTE_VALID;                 /* 2 MiB block */
            va += BLOCK_2M;
            pa += BLOCK_2M;
            continue;
        }
        e = walk(kroot, va, true);
        if (!e) panic("kmap: cannot map %p", (void *)va);
        *e = pa | attrs | PTE_VALID | PTE_PAGE;
        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }
}

extern char _start_image[], _etext[], _erodata[], _end[];

void kvm_init(const struct platform *pf, paddr_t img_pa)
{
    kroot = page_alloc();
    empty_table = page_alloc();
    if (!kroot || !empty_table) panic("kvm_init: out of memory");

    for (int i = 0; i < pf->nmem; i++) {
        uint64_t end = MIN(pf->mem_base[i] + pf->mem_size[i], 4UL << 30);
        if (pf->mem_base[i] < end)
            kmap(LINEAR_BASE + pf->mem_base[i], pf->mem_base[i], end - pf->mem_base[i], PTE_KERN_RW);
    }
    kmap(LINEAR_BASE + pf->gicd_base, pf->gicd_base, pf->gicd_size, PTE_KERN_DEV);
    kmap(LINEAR_BASE + pf->gicr_base, pf->gicr_base, pf->gicr_size, PTE_KERN_DEV);
    kmap(LINEAR_BASE + pf->uart_base, pf->uart_base, PAGE_SIZE, PTE_KERN_DEV);
    if (pf->rtc_base) kmap(LINEAR_BASE + pf->rtc_base, pf->rtc_base, PAGE_SIZE, PTE_KERN_DEV);
    for (int i = 0; i < pf->nvirtio; i++)
        kmap(LINEAR_BASE + pf->virtio_base[i], pf->virtio_base[i], PAGE_SIZE, PTE_KERN_DEV);

    /* The image with W^X: text RX, rodata RO, data/bss RW+XN. */
    vaddr_t t = (vaddr_t)_start_image, et = (vaddr_t)_etext, er = (vaddr_t)_erodata, e = (vaddr_t)_end;
    kmap(t, img_pa, et - t, PTE_KERN_RX);
    kmap(et, img_pa + (et - t), er - et, PTE_KERN_RO);
    kmap(er, img_pa + (er - t), e - er, PTE_KERN_RW);
    dsb_ish();
}

paddr_t kvm_empty_table(void) { return empty_table; }

extern char ttbr1_replace[];

void kvm_install(void)
{
    /* ttbr1_replace runs from the boot identity map (TTBR0) while TTBR1
     * briefly points at an empty table: break-before-make for the switch. */
    void (*replace)(uint64_t, uint64_t) = (void (*)(uint64_t, uint64_t))V2P(ttbr1_replace);
    replace(kroot, empty_table);
    write_sysreg(ttbr0_el1, empty_table);
    isb();
    __asm__ volatile("tlbi vmalle1\n dsb nsh\n isb" ::: "memory");
}

/* --------------------------------------------------- user spaces */

static uint64_t prot_bits(int prot)
{
    uint64_t b = PTE_USER_BASE | PTE_PAGE | PTE_VALID;
    if (!(prot & VM_W)) b |= PTE_RDONLY;
    if (!(prot & VM_X)) b |= PTE_UXN;
    return b;
}

int vm_create(struct vmspace *vs)
{
    memset(vs, 0, sizeof *vs);
    vs->asid = asid_alloc();
    if (!vs->asid) return -ENOMEM;
    vs->root = page_alloc();
    if (!vs->root) {
        asid_release(vs->asid);
        vs->asid = 0;
        return -ENOMEM;
    }
    return 0;
}

static void free_level(paddr_t table, int level)
{
    pte_t *t = P2V(table);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_VALID)) continue;
        paddr_t pa = t[i] & PTE_ADDR_MASK;
        if (level < 3) free_level(pa, level + 1);
        else page_put(pa);
    }
    page_put(table);
}

void vm_destroy(struct vmspace *vs)
{
    if (vs->root) free_level(vs->root, 0);
    if (vs->asid) asid_release(vs->asid);
    vs->root = 0;
    vs->asid = 0;
    vs->resident = 0;
}

void vm_activate(struct vmspace *vs)
{
    if (vs) write_sysreg(ttbr0_el1, vs->root | ((uint64_t)vs->asid << 48));
    else write_sysreg(ttbr0_el1, empty_table);
    isb();
}

int vm_map(struct vmspace *vs, vaddr_t va, paddr_t pa, int prot)
{
    if ((prot & VM_W) && (prot & VM_X)) return -EINVAL;     /* W^X */
    pte_t *e = walk(vs->root, va, true);
    if (!e) return -ENOMEM;
    if (*e & PTE_VALID) return -EEXIST;
    *e = pa | prot_bits(prot);
    vs->resident++;
    dsb_ishst();
    return 0;
}

/* fork: share every page.  Writable pages become read-only + COW in both
 * spaces; the first write to one copies it (vm_fault). */
static int clone_level(struct vmspace *dst, paddr_t stable, int level, vaddr_t base)
{
    pte_t *t = P2V(stable);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PTE_VALID)) continue;
        vaddr_t va = base | ((vaddr_t)i << LEVEL_SHIFT(level));
        if (level < 3) {
            int r = clone_level(dst, t[i] & PTE_ADDR_MASK, level + 1, va);
            if (r) return r;
            continue;
        }
        if (!(t[i] & PTE_RDONLY)) t[i] |= PTE_RDONLY | PTE_COW;
        pte_t *d = walk(dst->root, va, true);
        if (!d) return -ENOMEM;
        page_get(t[i] & PTE_ADDR_MASK);
        *d = t[i];
        dst->resident++;
    }
    return 0;
}

int vm_clone_cow(struct vmspace *dst, struct vmspace *src)
{
    int r = clone_level(dst, src->root, 0, 0);
    /* The parent may still hold writable TLB entries for pages that are now
     * shared; without this flush it would scribble on the child's memory. */
    tlb_flush_asid(src->asid);
    dst->heap_start = src->heap_start;
    dst->brk = src->brk;
    return r;
}

static bool lazy_region(struct vmspace *vs, vaddr_t va)
{
    return (va >= vs->heap_start && va < vs->brk) || (va >= USTACK_LOW && va < USER_TOP);
}

int vm_fault(struct vmspace *vs, vaddr_t va, bool write, bool exec)
{
    if (va < USER_MIN || va >= USER_TOP) return -EFAULT;
    vaddr_t page = ROUNDDOWN(va, PAGE_SIZE);
    pte_t *e = walk(vs->root, page, false);
    if (e && (*e & PTE_VALID)) {
        if (exec && (*e & PTE_UXN)) return -EFAULT;
        if (!write || !(*e & PTE_RDONLY)) {
            tlb_flush_page(vs->asid, page);   /* stale entry on this CPU; nothing to fix */
            return 0;
        }
        if (!(*e & PTE_COW)) return -EFAULT;  /* genuinely read-only (program text) */
        paddr_t old = *e & PTE_ADDR_MASK;
        stat_inc(STAT_COW_FAULTS);
        if (page_refcount(old) == 1) {        /* last sharer: take the page over */
            *e = (*e & ~(PTE_RDONLY | PTE_COW));
            tlb_flush_page(vs->asid, page);
            return 0;
        }
        paddr_t copy = page_alloc();
        if (!copy) return -ENOMEM;
        memcpy(P2V(copy), P2V(old), PAGE_SIZE);
        pte_t updated = copy | (*e & ~(PTE_ADDR_MASK | PTE_RDONLY | PTE_COW));
        *e = 0;                               /* break-before-make: new PA for a live VA */
        tlb_flush_page(vs->asid, page);
        *e = updated;
        dsb_ishst();
        page_put(old);
        return 0;
    }
    if (exec || !lazy_region(vs, va)) return -EFAULT;
    paddr_t pa = page_alloc();
    if (!pa) return -ENOMEM;
    stat_inc(STAT_LAZY_FAULTS);
    int r = vm_map(vs, page, pa, VM_R | VM_W);
    if (r) {
        page_put(pa);
        return r;
    }
    dsb_ish();
    return 0;
}

int vm_set_brk(struct vmspace *vs, vaddr_t newbrk)
{
    if (newbrk < vs->heap_start) return -EINVAL;
    if (newbrk > USTACK_LOW - (1UL << 20)) return -ENOMEM;   /* keep a 1 MiB gap below the stack */
    vaddr_t old_top = ROUNDUP(vs->brk, PAGE_SIZE), new_top = ROUNDUP(newbrk, PAGE_SIZE);
    bool unmapped = false;
    for (vaddr_t va = new_top; va < old_top; va += PAGE_SIZE) {
        pte_t *e = walk(vs->root, va, false);
        if (e && (*e & PTE_VALID)) {
            page_put(*e & PTE_ADDR_MASK);
            *e = 0;
            vs->resident--;
            unmapped = true;
        }
    }
    if (unmapped) tlb_flush_asid(vs->asid);
    vs->brk = newbrk;
    return 0;
}

/* Make freshly written code visible to instruction fetch. */
void vm_sync_icache(struct vmspace *vs, vaddr_t va, size_t len)
{
    for (vaddr_t p = ROUNDDOWN(va, PAGE_SIZE); p < va + len; p += PAGE_SIZE) {
        pte_t *e = walk(vs->root, p, false);
        if (!e || !(*e & PTE_VALID)) continue;
        char *k = P2V(*e & PTE_ADDR_MASK);
        for (int off = 0; off < (int)PAGE_SIZE; off += 64)
            __asm__ volatile("dc cvau, %0" ::"r"(k + off) : "memory");
    }
    __asm__ volatile("dsb ish\n ic ialluis\n dsb ish\n isb" ::: "memory");
}

/* Kernel pointer for user address va, faulting the page in (lazy or COW)
 * when needed.  NULL if va is not a valid user address. */
static uint8_t *user_ptr(struct vmspace *vs, vaddr_t va, bool write)
{
    if (va < USER_MIN || va >= USER_TOP) return NULL;
    pte_t *e = walk(vs->root, va, false);
    if (!e || !(*e & PTE_VALID) || (write && (*e & PTE_RDONLY))) {
        if (vm_fault(vs, va, write, false) != 0) return NULL;
        e = walk(vs->root, va, false);
        if (!e) return NULL;
    }
    if (!(*e & PTE_USER)) return NULL;
    return (uint8_t *)P2V(*e & PTE_ADDR_MASK) + (va & (PAGE_SIZE - 1));
}

int copyout(struct vmspace *vs, vaddr_t dst, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint8_t *k = user_ptr(vs, dst, true);
        if (!k) return -EFAULT;
        size_t n = MIN(len, PAGE_SIZE - (dst & (PAGE_SIZE - 1)));
        memcpy(k, s, n);
        len -= n;
        s += n;
        dst += n;
    }
    return 0;
}

int copyin(struct vmspace *vs, void *dst, vaddr_t src, size_t len)
{
    uint8_t *d = dst;
    while (len) {
        uint8_t *k = user_ptr(vs, src, false);
        if (!k) return -EFAULT;
        size_t n = MIN(len, PAGE_SIZE - (src & (PAGE_SIZE - 1)));
        memcpy(d, k, n);
        len -= n;
        d += n;
        src += n;
    }
    return 0;
}

int copyinstr(struct vmspace *vs, char *dst, vaddr_t src, size_t max)
{
    for (size_t i = 0; i < max;) {
        uint8_t *k = user_ptr(vs, src + i, false);
        if (!k) return -EFAULT;
        size_t n = MIN(max - i, PAGE_SIZE - ((src + i) & (PAGE_SIZE - 1)));
        for (size_t j = 0; j < n; j++, i++) {
            dst[i] = (char)k[j];
            if (!dst[i]) return 0;
        }
    }
    return -ENAMETOOLONG;
}
