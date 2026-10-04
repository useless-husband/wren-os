#ifndef KERNEL_TRAP_H
#define KERNEL_TRAP_H
#include "types.h"

/* Saved by entry.S; layout must match TF_SIZE and the offsets there. */
struct trapframe {
    uint64_t x[31];
    uint64_t sp;      /* SP_EL0 */
    uint64_t elr;     /* return address */
    uint64_t spsr;
};
_Static_assert(sizeof(struct trapframe) == 272, "trapframe layout is fixed by entry.S");

NORETURN void trap_return_to_user(struct trapframe *tf);

/* C entry points called from assembly (boot.S, entry.S). */
void kmain(paddr_t dtb_pa, paddr_t load_pa);
void secondary_main(int id);
void kernel_sync_trap(struct trapframe *tf);
void kernel_irq_trap(struct trapframe *tf);
void user_sync_trap(struct trapframe *tf);
void user_irq_trap(struct trapframe *tf);
void bad_trap(struct trapframe *tf, int kind);
void syscall_dispatch(struct trapframe *tf);

#endif
