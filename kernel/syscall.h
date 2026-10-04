#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H
#include "types.h"
#include "trap.h"

typedef int64_t (*syscall_fn)(struct trapframe *tf);

/* Argument helpers: arguments are x0..x5 of the trap frame. */
static inline uint64_t arg(struct trapframe *tf, int i) { return tf->x[i]; }
int fetch_path(struct trapframe *tf, int i, char *buf);   /* MAXPATH bytes; validates components */

#define SYSCALL(name) int64_t sys_##name(struct trapframe *tf)

#endif
