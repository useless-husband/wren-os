/* C side of the exception vectors in entry.S. */
#include "trap.h"
#include "arch.h"
#include "cpu.h"
#include "gic.h"
#include "printk.h"
#include "proc.h"
#include "vm.h"

static const char *ec_name(uint32_t ec)
{
    switch (ec) {
    case EC_UNKNOWN: return "undefined instruction";
    case EC_SVC64: return "svc";
    case EC_IABT_LOW: case EC_IABT_CUR: return "instruction abort";
    case EC_DABT_LOW: case EC_DABT_CUR: return "data abort";
    case EC_PC_ALIGN: return "pc misaligned";
    case EC_SP_ALIGN: return "sp misaligned";
    case EC_BRK: return "brk";
    case EC_SYSREG: return "system register access";
    default: return "exception";
    }
}

/* A fault inside the kernel is always a bug: the kernel never touches user
 * memory directly (copyin/copyout walk the tables in software). */
void kernel_sync_trap(struct trapframe *tf)
{
    uint64_t esr = read_sysreg(esr_el1), far = read_sysreg(far_el1);
    printk("\nkernel %s: esr %lx far %p elr %p\n", ec_name((uint32_t)(esr >> 26)), esr, (void *)far,
           (void *)tf->elr);
    backtrace(tf->x[29]);
    panic("unexpected kernel exception");
}

void kernel_irq_trap(struct trapframe *tf)
{
    (void)tf;
    gic_handle_irq();
}

void bad_trap(struct trapframe *tf, int kind)
{
    panic("unexpected exception vector %d, esr %lx elr %p", kind, read_sysreg(esr_el1), (void *)tf->elr);
}

/* Things to do before going back to user mode. */
static void user_trap_exit(void)
{
    struct proc *p = myproc();
    if (proc_killed(p)) {
        irq_on();
        proc_exit(-1);
    }
    if (proc_need_resched()) proc_yield();
    if (proc_killed(p)) {
        irq_on();
        proc_exit(-1);
    }
}

void user_sync_trap(struct trapframe *tf)
{
    uint64_t esr = read_sysreg(esr_el1), far = read_sysreg(far_el1);
    uint32_t ec = (uint32_t)(esr >> 26);
    struct proc *p = myproc();
    irq_on();
    if (ec == EC_SVC64) {
        syscall_dispatch(tf);
    } else if (ec == EC_DABT_LOW || ec == EC_IABT_LOW) {
        bool write = ec == EC_DABT_LOW && (esr & (1u << 6));   /* ISS.WnR */
        uint32_t fsc = esr & 0x3f;
        int r = -1;
        if (FSC_TYPE(fsc) == FSC_TRANSLATION || FSC_TYPE(fsc) == FSC_PERMISSION)
            r = vm_fault(proc_vm(p), far, write, ec == EC_IABT_LOW);
        if (r != 0) {
            printk("pid %d (%s): %s at %p (%s), pc %p: killed\n", p->pid, p->name,
                   r == -12 ? "out of memory" : "segmentation fault", (void *)far,
                   ec == EC_IABT_LOW ? "exec" : write ? "write" : "read", (void *)tf->elr);
            proc_kill_self();
        }
    } else {
        printk("pid %d (%s): %s (esr %lx) at pc %p: killed\n", p->pid, p->name, ec_name(ec), esr,
               (void *)tf->elr);
        proc_kill_self();
    }
    user_trap_exit();   /* entry.S masks interrupts again before eret */
}

void user_irq_trap(struct trapframe *tf)
{
    (void)tf;
    gic_handle_irq();
    user_trap_exit();
}
