/* AArch64 helpers: system registers, barriers, interrupt masking and the
 * page-table descriptor format (ARM ARM D8.3, 4 KiB granule). */
#ifndef KERNEL_ARCH_H
#define KERNEL_ARCH_H
#include "types.h"

#define read_sysreg(r) ({ uint64_t v__; __asm__ volatile("mrs %0, " #r : "=r"(v__)); v__; })
#define write_sysreg(r, v) __asm__ volatile("msr " #r ", %0" ::"r"((uint64_t)(v)) : "memory")

/* GICv3 CPU interface registers, by encoding so any assembler accepts them. */
#define ICC_PMR_EL1     S3_0_C4_C6_0
#define ICC_IAR1_EL1    S3_0_C12_C12_0
#define ICC_EOIR1_EL1   S3_0_C12_C12_1
#define ICC_BPR1_EL1    S3_0_C12_C12_3
#define ICC_CTLR_EL1    S3_0_C12_C12_4
#define ICC_SRE_EL1     S3_0_C12_C12_5
#define ICC_IGRPEN1_EL1 S3_0_C12_C12_7
#define ICC_SGI1R_EL1   S3_0_C12_C11_5
#define read_icc(r)     read_sysreg(r)
#define write_icc(r, v) write_sysreg(r, v)

static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }
static inline void dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void dsb_ish(void) { __asm__ volatile("dsb ish" ::: "memory"); }
static inline void dsb_ishst(void) { __asm__ volatile("dsb ishst" ::: "memory"); }
static inline void dmb_ish(void) { __asm__ volatile("dmb ish" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile("yield" ::: "memory"); }
static inline void wfi(void) { __asm__ volatile("wfi" ::: "memory"); }

/* PSTATE.I is the only mask we use; FIQs are never routed to us. */
static inline void irq_on(void) { __asm__ volatile("msr daifclr, #2" ::: "memory"); }
static inline void irq_off(void) { __asm__ volatile("msr daifset, #2" ::: "memory"); }
static inline bool irq_enabled(void) { return (read_sysreg(daif) & (1 << 7)) == 0; }

static inline uint64_t timer_count(void) { isb(); return read_sysreg(cntvct_el0); }
static inline uint64_t timer_freq(void) { return read_sysreg(cntfrq_el0); }

/* MMIO accessors: volatile, naturally aligned, no compiler reordering. */
static inline uint32_t mmio_read32(uint64_t a) { return *(volatile uint32_t *)a; }
static inline void mmio_write32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint64_t mmio_read64(uint64_t a) { return *(volatile uint64_t *)a; }
static inline void mmio_write64(uint64_t a, uint64_t v) { *(volatile uint64_t *)a = v; }

/* ---- translation table descriptors ---- */
#define PTE_VALID     (1UL << 0)
#define PTE_TABLE     (1UL << 1)    /* at levels 0-2: next-level table; at level 3: page */
#define PTE_PAGE      (1UL << 1)
#define PTE_ATTR(i)   ((uint64_t)(i) << 2)   /* MAIR index */
#define PTE_USER      (1UL << 6)    /* AP[1]: EL0 may access */
#define PTE_RDONLY    (1UL << 7)    /* AP[2] */
#define PTE_SH_INNER  (3UL << 8)
#define PTE_AF        (1UL << 10)
#define PTE_NG        (1UL << 11)   /* not global: tagged with the ASID */
#define PTE_PXN       (1UL << 53)
#define PTE_UXN       (1UL << 54)
#define PTE_COW       (1UL << 55)   /* software bit: read-only because of copy-on-write */
#define PTE_ADDR_MASK 0x0000fffffffff000UL

#define MAIR_DEVICE   0             /* Device-nGnRE */
#define MAIR_NORMAL   1             /* Normal, write-back, read/write-allocate */
#define MAIR_NORMAL_NC 2            /* Normal, non-cacheable */
#define MAIR_VALUE    0x44ff04UL

/* Leaf attribute sets. */
#define PTE_KERN_RW   (PTE_AF | PTE_SH_INNER | PTE_ATTR(MAIR_NORMAL) | PTE_PXN | PTE_UXN)
#define PTE_KERN_RO   (PTE_KERN_RW | PTE_RDONLY)
#define PTE_KERN_RX   (PTE_AF | PTE_SH_INNER | PTE_ATTR(MAIR_NORMAL) | PTE_RDONLY | PTE_UXN)
#define PTE_KERN_DEV  (PTE_AF | PTE_ATTR(MAIR_DEVICE) | PTE_PXN | PTE_UXN)
#define PTE_USER_BASE (PTE_AF | PTE_SH_INNER | PTE_ATTR(MAIR_NORMAL) | PTE_NG | PTE_USER | PTE_PXN)

/* ESR_EL1 exception classes we care about. */
#define EC_UNKNOWN    0x00
#define EC_FP         0x07
#define EC_SVC64      0x15
#define EC_SYSREG     0x18
#define EC_IABT_LOW   0x20
#define EC_IABT_CUR   0x21
#define EC_PC_ALIGN   0x22
#define EC_DABT_LOW   0x24
#define EC_DABT_CUR   0x25
#define EC_SP_ALIGN   0x26
#define EC_BRK        0x3c

/* Fault status codes in ESR.ISS[5:0] (DFSC/IFSC). */
#define FSC_TYPE(fsc) ((fsc) & 0x3c)
#define FSC_TRANSLATION 0x04
#define FSC_ACCESS      0x08
#define FSC_PERMISSION  0x0c

#endif
