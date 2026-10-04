#include "spinlock.h"
#include "cpu.h"
#include "printk.h"
#include "proc.h"

void irq_push(void)
{
    bool was_on = irq_enabled();
    irq_off();
    struct cpu *c = mycpu();
    if (c->irq_depth++ == 0) c->irq_was_on = was_on;
}

void irq_pop(void)
{
    struct cpu *c = mycpu();
    if (irq_enabled()) panic("irq_pop: interrupts enabled");
    if (c->irq_depth < 1) panic("irq_pop: unbalanced");
    if (--c->irq_depth == 0 && c->irq_was_on) irq_on();
}

void spin_init(struct spinlock *lk, const char *name)
{
    lk->next = lk->serving = 0;
    lk->cpu = -1;
    lk->name = name;
}

bool spin_held(const struct spinlock *lk)
{
    return __atomic_load_n(&lk->serving, __ATOMIC_RELAXED) != __atomic_load_n(&lk->next, __ATOMIC_RELAXED) &&
           lk->cpu == mycpu()->id;
}

void spin_lock(struct spinlock *lk)
{
    irq_push();
    if (spin_held(lk)) panic("spin_lock: %s already held by this cpu", lk->name);
    uint32_t ticket = __atomic_fetch_add(&lk->next, 1, __ATOMIC_RELAXED);
    uint64_t spins = 0, since = 0;
    while (__atomic_load_n(&lk->serving, __ATOMIC_ACQUIRE) != ticket) {
        cpu_relax();
        if ((++spins & 0xffff) == 0) {
            uint64_t now = timer_count();
            if (!since) since = now;
            else if (now - since > DEADLOCK_SECONDS * timer_freq() && !panicking)
                panic("deadlock? waited %ds for %s held by cpu %d", DEADLOCK_SECONDS, lk->name, lk->cpu);
        }
    }
    lk->cpu = mycpu()->id;
}

void spin_unlock(struct spinlock *lk)
{
    if (!spin_held(lk)) panic("spin_unlock: %s not held", lk->name);
    lk->cpu = -1;
    __atomic_store_n(&lk->serving, lk->serving + 1, __ATOMIC_RELEASE);
    irq_pop();
}

void sleep_init(struct sleeplock *lk, const char *name)
{
    spin_init(&lk->lk, "sleeplock");
    lk->name = name;
    lk->locked = false;
    lk->pid = 0;
}

void sleep_lock(struct sleeplock *lk)
{
    spin_lock(&lk->lk);
    while (lk->locked) proc_sleep(lk, &lk->lk);
    lk->locked = true;
    lk->pid = myproc()->pid;
    spin_unlock(&lk->lk);
}

void sleep_unlock(struct sleeplock *lk)
{
    spin_lock(&lk->lk);
    lk->locked = false;
    lk->pid = 0;
    proc_wakeup(lk);
    spin_unlock(&lk->lk);
}

bool sleep_held(struct sleeplock *lk)
{
    spin_lock(&lk->lk);
    bool r = lk->locked && lk->pid == myproc()->pid;
    spin_unlock(&lk->lk);
    return r;
}
