#include "syscall.h"
#include "blk.h"
#include "cpu.h"
#include "gic.h"
#include "kalloc.h"
#include "printk.h"
#include "proc.h"
#include "psci.h"
#include "stats.h"
#include <wren/errno.h>
#include <wren/procinfo.h>
#include <wren/syscall.h>

SYSCALL(fork) { (void)tf; return proc_fork(); }
SYSCALL(exit) { proc_exit((int)arg(tf, 0)); }
SYSCALL(waitpid) { return proc_wait((int)arg(tf, 0), arg(tf, 1), (int)arg(tf, 2)); }
SYSCALL(kill) { return proc_kill((int)arg(tf, 0)); }
SYSCALL(getpid) { (void)tf; return myproc()->pid; }

SYSCALL(getppid)
{
    (void)tf;
    spin_lock(&proc_lock);
    struct proc *parent = myproc()->parent;
    int pid = parent ? parent->pid : 0;
    spin_unlock(&proc_lock);
    return pid;
}

/* Grow or shrink the heap.  Growing only moves the break: pages are
 * allocated on first touch (vm_fault). */
SYSCALL(sbrk)
{
    struct vmspace *vm = &myproc()->vm;
    int64_t incr = (int64_t)arg(tf, 0);
    uint64_t old = vm->brk;
    if (incr < 0 && (uint64_t)-incr > old - vm->heap_start) return -EINVAL;
    if (incr > 0 && (uint64_t)incr > (1UL << 36)) return -ENOMEM;
    int r = vm_set_brk(vm, old + (uint64_t)incr);
    return r < 0 ? r : (int64_t)old;
}

SYSCALL(sleep)
{
    uint64_t ms = arg(tf, 0);
    if (ms > 86400000UL) return -EINVAL;
    uint64_t deadline = timer_count() + ms * timer_freq() / 1000;
    spin_lock(&tick_lock);
    while (timer_count() < deadline) {
        if (proc_killed(myproc())) {
            spin_unlock(&tick_lock);
            return -EINTR;
        }
        proc_sleep((void *)&ticks, &tick_lock);
    }
    spin_unlock(&tick_lock);
    return 0;
}

SYSCALL(uptime) { (void)tf; return (int64_t)uptime_ms(); }
SYSCALL(procinfo) { return proc_info(arg(tf, 0), (int)arg(tf, 1)); }

SYSCALL(poweroff)
{
    (void)tf;
    printk("wren-os: power off\n");
    psci_system_off();
}

SYSCALL(getcpu)
{
    (void)tf;
    irq_push();
    int id = mycpu()->id;
    irq_pop();
    return id;
}

SYSCALL(kstat)
{
    switch (arg(tf, 0)) {
    case KSTAT_FREE_PAGES: return (int64_t)pages_free_count();
    case KSTAT_TOTAL_PAGES: return (int64_t)pages_total();
    case KSTAT_NCPU: return ncpu_online;
    case KSTAT_DISK_WRITES: return (int64_t)stat_get(STAT_DISK_WRITES);
    case KSTAT_DISK_READS: return (int64_t)stat_get(STAT_DISK_READS);
    case KSTAT_LOG_COMMITS: return (int64_t)stat_get(STAT_LOG_COMMITS);
    case KSTAT_CTX_SWITCHES: return (int64_t)stat_get(STAT_CTX_SWITCHES);
    case KSTAT_COW_FAULTS: return (int64_t)stat_get(STAT_COW_FAULTS);
    case KSTAT_LAZY_FAULTS: return (int64_t)stat_get(STAT_LAZY_FAULTS);
    case KSTAT_TIMER_HZ: return HZ;
    default: return -EINVAL;
    }
}

/* Set the PER_* flags and return the old ones; PER_QUERY only reads them.
 * Only the calling process changes: there is nothing to lock. */
SYSCALL(personality)
{
    uint64_t want = arg(tf, 0);
    struct proc *p = myproc();
    uint32_t old = p->personality;
    if (want == PER_QUERY) return old;
    if (want & ~(uint64_t)PER_MASK) return -EINVAL;
    p->personality = (uint32_t)want;
    return old;
}

SYSCALL(crashctl)
{
    switch (arg(tf, 0)) {
    case CRASH_DISARM: blk_crash_arm(0, 0); return 0;
    case CRASH_AFTER: blk_crash_arm(arg(tf, 1), 0); return 0;
    case CRASH_TORN: blk_crash_arm(arg(tf, 1), (int)arg(tf, 2)); return 0;
    default: return -EINVAL;
    }
}
