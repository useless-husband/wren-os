/* Boot entry for the primary and the secondary CPUs.
 *
 * The file starts with the arm64 Linux Image header, so any loader that
 * boots Linux (QEMU -kernel, LeapVM -k) boots us: the loader puts the
 * image at a 2 MiB aligned physical address, turns the MMU off, and jumps
 * to offset 0 with x0 = physical address of the device tree blob.
 *
 * Until the MMU is on, everything here is position independent (adr/adrp
 * are PC-relative), because we do not know the load address in advance:
 * QEMU loads at RAM + 2 MiB, LeapVM at RAM + 0.
 *
 * Boot page tables (6 pages in .pgtbl, built here with the MMU off):
 *   TTBR0: identity map of the 1 GiB block holding the image (only needed
 *          for the instruction that turns the MMU on, and for the TTBR1
 *          switch trampoline later)
 *   TTBR1: linear map 0xffff000000000000 + [0, 4 GiB) with 1 GiB blocks
 *          (block 0 = device memory, blocks 1..3 = normal memory), and the
 *          kernel image at KIMAGE_VBASE with 2 MiB blocks.
 * kmain() replaces them with exact 4 KiB-granular tables built from the
 * device tree (see vm.c).
 */
#include "memlayout.h"

#define BOOT_STACK_SIZE   16384
#define NCPU_MAX          8

#define BLOCK_NORMAL_X    0x0040000000000705   /* AF|SH inner|Normal|UXN, EL1 RWX */
#define BLOCK_NORMAL_NX   0x0060000000000705   /* + PXN */
#define BLOCK_DEVICE      0x0060000000000401   /* AF|Device-nGnRE|PXN|UXN */

#define TCR_VALUE         0xb5103510   /* 48-bit VAs both halves, 4K granule, WB WA inner-shareable walks */
#define SCTLR_VALUE       0x34d5d81d   /* MMU, caches, SP alignment checks, EL0 cache ops/CTR/WFI */
#define SCTLR_EL1_RESET   0x30d00800
#define SPSR_EL1H_MASKED  0x3c5

    .section .text.head, "ax"
    .globl _start
_start:
    b       primary_entry           /* code0 */
    .long   0                       /* code1 */
    .quad   0                       /* text_offset */
    .quad   _image_size             /* image_size: whole image incl. bss and page tables */
    .quad   0xa                     /* flags: LE, 4 KiB pages, 2 MiB base anywhere in RAM */
    .quad   0, 0, 0                 /* reserved */
    .ascii  "ARM\x64"               /* magic */
    .long   0                       /* reserved (no PE header) */

primary_entry:
    mov     x19, x0                 /* DTB physical address */
    bl      drop_to_el1
    adr     x20, _start             /* where we were loaded (physical) */
    tst     x20, #0x1fffff          /* must be 2 MiB aligned for the block mappings */
    b.ne    hang
    bl      build_boot_tables
    bl      mmu_on
    ldr     x0, =primary_high       /* absolute link-time address in the TTBR1 image map */
    br      x0

primary_high:
    adrp    x0, boot_stacks
    add     x0, x0, :lo12:boot_stacks
    add     sp, x0, #BOOT_STACK_SIZE
    adrp    x0, vectors
    add     x0, x0, :lo12:vectors
    msr     vbar_el1, x0
    isb
    adrp    x0, _bss_start
    add     x0, x0, :lo12:_bss_start
    adrp    x1, _bss_end
    add     x1, x1, :lo12:_bss_end
1:  cmp     x0, x1
    b.hs    2f
    stp     xzr, xzr, [x0], #16
    b       1b
2:  ldr     x0, =KIMAGE_VBASE
    sub     x0, x0, x20
    adrp    x1, kimage_voffset
    str     x0, [x1, :lo12:kimage_voffset]
    mov     x0, x19
    mov     x1, x20
    bl      kmain
    b       hang

/* PSCI CPU_ON lands here with the MMU off and x0 = our logical CPU id. */
    .globl secondary_entry
secondary_entry:
    mov     x19, x0
    bl      drop_to_el1
    bl      mmu_on
    ldr     x0, =secondary_high
    br      x0

secondary_high:
    adrp    x0, boot_stacks
    add     x0, x0, :lo12:boot_stacks
    add     x1, x19, #1
    lsl     x1, x1, #14             /* (id + 1) * BOOT_STACK_SIZE */
    add     sp, x0, x1
    adrp    x0, vectors
    add     x0, x0, :lo12:vectors
    msr     vbar_el1, x0
    isb
    mov     x0, x19
    bl      secondary_main
    b       hang

    .globl hang
hang:
    wfe
    b       hang

/* If a loader enters at EL2 (QEMU with virtualization=on), configure EL2
 * to stay out of the way and drop to EL1.  Clobbers x0. */
drop_to_el1:
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    9f
    mov     x0, #(1 << 31)          /* HCR_EL2.RW: EL1 is AArch64; nothing trapped */
    msr     hcr_el2, x0
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3              /* EL1PCTEN | EL1PCEN: EL1 may use the timers */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr
    mrs     x0, id_aa64pfr0_el1     /* GIC system registers implemented? */
    ubfx    x0, x0, #24, #4
    cbz     x0, 8f
    mrs     x0, S3_4_C12_C9_5       /* ICC_SRE_EL2 */
    orr     x0, x0, #0xf            /* SRE | DFB | DIB | Enable (EL1 may use ICC_SRE_EL1) */
    msr     S3_4_C12_C9_5, x0
    isb
8:  mov     x0, #0x33ff             /* CPTR_EL2: do not trap FP/SIMD */
    msr     cptr_el2, x0
    msr     hstr_el2, xzr
    mrs     x0, midr_el1
    msr     vpidr_el2, x0
    mrs     x0, mpidr_el1
    msr     vmpidr_el2, x0
    msr     vttbr_el2, xzr
    ldr     x0, =SCTLR_EL1_RESET
    msr     sctlr_el1, x0
    mov     x0, #SPSR_EL1H_MASKED
    msr     spsr_el2, x0
    msr     elr_el2, lr
    isb
    eret
9:  ret

/* Invalidate data cache lines covering [x0, x0 + x1) to the point of
 * coherency.  Used around page-table writes made with the MMU off, so no
 * stale cached copy can shadow what we wrote.  Clobbers x2-x4. */
dcache_inval:
    mrs     x3, ctr_el0
    ubfx    x3, x3, #16, #4         /* DminLine: log2(words per line) */
    mov     x2, #4
    lsl     x2, x2, x3              /* line size in bytes */
    add     x1, x0, x1
    sub     x3, x2, #1
    bic     x4, x0, x3
1:  dc      ivac, x4
    add     x4, x4, x2
    cmp     x4, x1
    b.lo    1b
    dsb     sy
    ret

/* Build the boot tables described at the top.  x20 = image physical base.
 * Clobbers x0-x9, x21. */
build_boot_tables:
    mov     x21, lr
    adrp    x0, boot_pgtables
    mov     x1, #(6 * 4096)
    bl      dcache_inval
    adrp    x0, boot_pgtables
    add     x1, x0, #(6 * 4096)
1:  stp     xzr, xzr, [x0], #16
    cmp     x0, x1
    b.lo    1b
    adrp    x0, boot_pgtables       /* x0 = page 0: identity L0 */

    /* identity: L0[pa >> 39] -> page 1; page1[pa >> 30] = 1 GiB block */
    ubfx    x2, x20, #39, #9
    add     x3, x0, #4096
    orr     x3, x3, #3
    str     x3, [x0, x2, lsl #3]
    ubfx    x2, x20, #30, #9
    lsr     x4, x20, #30
    lsl     x4, x4, #30
    ldr     x5, =BLOCK_NORMAL_X
    orr     x4, x4, x5
    add     x3, x0, #4096
    str     x4, [x3, x2, lsl #3]

    /* kernel L0 (page 2): [0] -> linear L1 (page 3), [511] -> image L1 (page 4) */
    add     x6, x0, #(2 * 4096)
    add     x3, x0, #(3 * 4096)
    orr     x3, x3, #3
    str     x3, [x6]
    add     x3, x0, #(4 * 4096)
    orr     x3, x3, #3
    str     x3, [x6, #(511 * 8)]

    /* linear L1: [0] device, [1..3] normal, 1 GiB each */
    add     x6, x0, #(3 * 4096)
    ldr     x3, =BLOCK_DEVICE
    str     x3, [x6]
    ldr     x5, =BLOCK_NORMAL_NX
    mov     x2, #1
2:  lsl     x3, x2, #30
    orr     x3, x3, x5
    str     x3, [x6, x2, lsl #3]
    add     x2, x2, #1
    cmp     x2, #4
    b.lo    2b

    /* image L1[511] -> image L2 (page 5); L2[i] = image pa + i * 2 MiB */
    add     x6, x0, #(4 * 4096)
    add     x3, x0, #(5 * 4096)
    orr     x3, x3, #3
    str     x3, [x6, #(511 * 8)]
    add     x6, x0, #(5 * 4096)
    ldr     x7, =_image_size
    ldr     x8, =0x1fffff
    add     x7, x7, x8
    lsr     x7, x7, #21             /* number of 2 MiB blocks */
    ldr     x5, =BLOCK_NORMAL_X
    mov     x2, #0
3:  lsl     x3, x2, #21
    add     x3, x3, x20
    orr     x3, x3, x5
    str     x3, [x6, x2, lsl #3]
    add     x2, x2, #1
    cmp     x2, x7
    b.lo    3b

    dmb     sy
    adrp    x0, boot_pgtables
    mov     x1, #(6 * 4096)
    bl      dcache_inval
    mov     lr, x21
    ret

/* Program MAIR/TCR/TTBRs for the boot tables and turn the MMU on.
 * Runs on every CPU; touches only registers and the boot tables. */
mmu_on:
    ldr     x0, =0x44ff04           /* MAIR: 0 Device-nGnRE, 1 Normal WB, 2 Normal NC */
    msr     mair_el1, x0
    ldr     x0, =TCR_VALUE
    mrs     x1, id_aa64mmfr0_el1
    and     x1, x1, #7              /* PARange */
    cmp     x1, #5
    mov     x2, #5
    csel    x1, x1, x2, ls          /* at most 48-bit PAs */
    bfi     x0, x1, #32, #3         /* TCR.IPS */
    msr     tcr_el1, x0
    adrp    x0, boot_pgtables
    msr     ttbr0_el1, x0
    add     x0, x0, #(2 * 4096)
    msr     ttbr1_el1, x0
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    mov     x0, #(3 << 20)          /* CPACR_EL1.FPEN: FP/SIMD allowed at EL0 and EL1 */
    msr     cpacr_el1, x0
    ldr     x0, =SCTLR_VALUE
    msr     sctlr_el1, x0
    isb
    ic      iallu
    dsb     nsh
    isb
    ret

/* Switch TTBR1 to new tables without ever having two different valid
 * translations for the same address in the TLB (break-before-make).
 * Must be called through its identity-mapped physical address.
 * x0 = new TTBR1 value, x1 = physical address of an all-invalid table. */
    .globl ttbr1_replace
ttbr1_replace:
    msr     ttbr1_el1, x1
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    msr     ttbr1_el1, x0
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    ret

    .ltorg

    .section .pgtbl, "aw", %nobits
    .balign 4096
    .globl boot_pgtables
boot_pgtables:
    .space  6 * 4096

    .section .bss.boot_stacks, "aw", %nobits
    .balign 16
    .globl boot_stacks
boot_stacks:
    .space  NCPU_MAX * BOOT_STACK_SIZE
