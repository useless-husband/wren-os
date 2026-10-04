/* Virtual memory layout (48-bit VAs, 4 KiB granule, 4-level tables).
 *
 *  TTBR1 (kernel, shared by every CPU and process)
 *   0xffff_0000_0000_0000 + pa   linear map of RAM and device MMIO
 *   0xffff_ffff_c000_0000        kernel image, wherever the loader put it
 *
 *  TTBR0 (one per process, tagged with an 8-bit ASID)
 *   0x0000_0000_0001_0000        program text/data (ELF PT_LOAD), then heap (sbrk)
 *   USER_TOP - USTACK_MAX        stack, grows down lazily to USTACK_MAX
 *   USER_TOP = 0x40_0000_0000    (256 GiB)
 */
#ifndef KERNEL_MEMLAYOUT_H
#define KERNEL_MEMLAYOUT_H

#ifdef __ASSEMBLER__
#define UL(x) x
#else
#define UL(x) x##UL
#endif

#define PAGE_SHIFT   12
#define PAGE_SIZE    (UL(1) << PAGE_SHIFT)
#define LINEAR_BASE  UL(0xffff000000000000)
#define KIMAGE_VBASE UL(0xffffffffc0000000)

#define USER_MIN     UL(0x10000)
#define USER_TOP     UL(0x4000000000)
#define USTACK_MAX   (UL(8) << 20)
#define USTACK_LOW   (USER_TOP - USTACK_MAX)

#ifndef __ASSEMBLER__
#include "types.h"
extern uint64_t kimage_voffset;              /* image VA - image PA */
static inline void *P2V(paddr_t pa) { return (void *)(pa + LINEAR_BASE); }
static inline paddr_t V2P(const void *va) {  /* linear-map or image address -> PA */
    uint64_t v = (uint64_t)va;
    return v >= KIMAGE_VBASE ? v - kimage_voffset : v - LINEAR_BASE;
}
#endif

#endif
