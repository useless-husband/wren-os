/* Exception vectors and the trap frame.
 *
 * Every exception saves all general registers plus SP_EL0, ELR_EL1 and
 * SPSR_EL1 into a struct trapframe on the current kernel stack (SP_EL1).
 * For a process running in user mode SP_EL1 sits at the top of its kernel
 * stack, so the user trap frame is always at kstack_top - TF_SIZE.
 * Layout must match struct trapframe in trap.h.
 */
#define TF_SIZE 272

.macro SAVE_REGS
    sub     sp, sp, #TF_SIZE
    stp     x0, x1, [sp, #0]
    stp     x2, x3, [sp, #16]
    stp     x4, x5, [sp, #32]
    stp     x6, x7, [sp, #48]
    stp     x8, x9, [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x21, sp_el0
    stp     x30, x21, [sp, #240]
    mrs     x22, elr_el1
    mrs     x23, spsr_el1
    stp     x22, x23, [sp, #256]
.endm

.macro RESTORE_REGS_AND_ERET
    ldp     x22, x23, [sp, #256]
    msr     elr_el1, x22
    msr     spsr_el1, x23
    ldp     x30, x21, [sp, #240]
    msr     sp_el0, x21
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldp     x4, x5, [sp, #32]
    ldp     x6, x7, [sp, #48]
    ldp     x8, x9, [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    add     sp, sp, #TF_SIZE
    eret
    dsb     nsh                     /* no straight-line speculation past eret */
    isb
.endm

.macro VENTRY target
    .balign 0x80
    b       \target
.endm

    .text
    .balign 0x800
    .globl vectors
vectors:
    VENTRY  bad_el1t_sync           /* current EL, SP_EL0: never used */
    VENTRY  bad_el1t_irq
    VENTRY  bad_el1t_fiq
    VENTRY  bad_el1t_serror
    VENTRY  el1h_sync               /* current EL, SP_EL1: the kernel */
    VENTRY  el1h_irq
    VENTRY  bad_el1h_fiq
    VENTRY  bad_el1h_serror
    VENTRY  el0_sync                /* lower EL, AArch64: user processes */
    VENTRY  el0_irq
    VENTRY  bad_el0_fiq
    VENTRY  bad_el0_serror
    VENTRY  bad_el0_32              /* lower EL, AArch32: not supported */
    VENTRY  bad_el0_32
    VENTRY  bad_el0_32
    VENTRY  bad_el0_32

el1h_sync:
    SAVE_REGS
    mov     x0, sp
    bl      kernel_sync_trap
    RESTORE_REGS_AND_ERET

el1h_irq:
    SAVE_REGS
    mov     x0, sp
    bl      kernel_irq_trap
    RESTORE_REGS_AND_ERET

el0_sync:
    SAVE_REGS
    mov     x0, sp
    bl      user_sync_trap
    b       user_return

el0_irq:
    SAVE_REGS
    mov     x0, sp
    bl      user_irq_trap
    b       user_return

/* void trap_return_to_user(struct trapframe *tf): enter user mode with the
 * registers in tf.  Used for the first run of every new process. */
    .globl trap_return_to_user
trap_return_to_user:
    mov     sp, x0
user_return:
    msr     daifset, #2             /* ELR/SPSR must not change under us */
    RESTORE_REGS_AND_ERET

.macro BAD name, kind
\name:
    SAVE_REGS
    mov     x0, sp
    mov     x1, #\kind
    bl      bad_trap
    b       hang
.endm

BAD bad_el1t_sync, 0
BAD bad_el1t_irq, 1
BAD bad_el1t_fiq, 2
BAD bad_el1t_serror, 3
BAD bad_el1h_fiq, 6
BAD bad_el1h_serror, 7
BAD bad_el0_fiq, 10
BAD bad_el0_serror, 11
BAD bad_el0_32, 12
