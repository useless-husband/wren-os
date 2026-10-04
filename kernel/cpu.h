/* Per-CPU state.  TPIDR_EL1 holds a pointer to the running CPU's struct cpu. */
#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H
#include "types.h"
#include "param.h"
#include "arch.h"

/* Callee-saved registers of a suspended kernel thread (layout: switch.S). */
struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp, lr, sp;
};

struct proc;

struct cpu {
    int             id;             /* logical id: index in cpus[] */
    uint64_t        mpidr;
    struct proc    *proc;           /* running process, or NULL in the scheduler */
    struct context  sched_ctx;      /* the scheduler thread of this CPU */
    int             irq_depth;      /* nesting of irq_push() */
    bool            irq_was_on;     /* interrupts on before the outermost irq_push()? */
    volatile bool   online;
    volatile bool   idle;           /* waiting in WFI for work */
    uint64_t        gicr_rd;        /* this CPU's GIC redistributor (RD frame VA) */
    uint64_t        ctx_switches;
    uint64_t        ticks;
};

extern struct cpu cpus[NCPU];
extern int ncpu_online;

static inline struct cpu *mycpu(void) { return (struct cpu *)read_sysreg(tpidr_el1); }

void irq_push(void);   /* disable interrupts, nestable */
void irq_pop(void);

#endif
