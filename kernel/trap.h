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
void syscall_dispatch(struct trapframe *tf);

#endif
