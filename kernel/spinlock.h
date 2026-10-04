/* Ticket spinlocks.  Holding one disables interrupts on this CPU (so an
 * interrupt handler can never spin on a lock its own CPU holds), and a lock
 * held for more than DEADLOCK_SECONDS makes the waiter panic with the lock's
 * name and owner instead of hanging silently. */
#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H
#include "types.h"

#define DEADLOCK_SECONDS 20

struct spinlock {
    uint32_t    next;       /* next ticket to hand out */
    uint32_t    serving;    /* ticket allowed in */
    int         cpu;        /* holder, -1 if free (debug only) */
    const char *name;
};

#define SPINLOCK_INIT(n) { 0, 0, -1, (n) }

void spin_init(struct spinlock *lk, const char *name);
void spin_lock(struct spinlock *lk);
void spin_unlock(struct spinlock *lk);
bool spin_held(const struct spinlock *lk);   /* by this CPU */

/* Sleep locks: may be held across blocking operations (disk I/O). */
struct sleeplock {
    bool            locked;
    int             pid;    /* holder */
    struct spinlock lk;
    const char     *name;
};

void sleep_init(struct sleeplock *lk, const char *name);
void sleep_lock(struct sleeplock *lk);
void sleep_unlock(struct sleeplock *lk);
bool sleep_held(struct sleeplock *lk);

#endif
