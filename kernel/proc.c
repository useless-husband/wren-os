#include "proc.h"
#include "file.h"
#include "fs.h"
#include "gic.h"
#include "kalloc.h"
#include "log.h"
#include "memlayout.h"
#include "printk.h"
#include "stats.h"
#include "trap.h"
#include <wren/errno.h>
#include <wren/fcntl.h>
#include <wren/procinfo.h>
#include "../lib/kstring.h"

struct spinlock proc_lock = SPINLOCK_INIT("proc");
static struct proc ptable[NPROC];
static struct proc *initproc;
static struct proc *rq_head, *rq_tail;   /* FIFO of RUNNABLE processes */
static int next_pid = 1;
static bool need_resched[NCPU];

void proc_init(void)
{
    for (int i = 0; i < NPROC; i++) ptable[i].state = P_UNUSED;
}

struct proc *myproc(void)
{
    irq_push();
    struct proc *p = mycpu()->proc;
    irq_pop();
    return p;
}

struct vmspace *proc_vm(struct proc *p) { return &p->vm; }

/* ---------------------------------------------------- run queue */

static void runq_push(struct proc *p)
{
    p->state = P_RUNNABLE;
    p->rq_next = NULL;
    if (rq_tail) rq_tail->rq_next = p;
    else rq_head = p;
    rq_tail = p;
}

static struct proc *runq_pop(void)
{
    struct proc *p = rq_head;
    if (p) {
        rq_head = p->rq_next;
        if (!rq_head) rq_tail = NULL;
        p->rq_next = NULL;
    }
    return p;
}

/* Caller holds proc_lock.  Queue p and poke one idle CPU so it does not
 * wait for its next timer tick to notice. */
static void make_runnable(struct proc *p)
{
    runq_push(p);
    int me = mycpu()->id;
    for (int i = 0; i < ncpu_online; i++)
        if (i != me && cpus[i].idle) {
            gic_send_sgi(i, SGI_WAKEUP);
            break;
        }
}

/* ---------------------------------------------------- scheduling */

void scheduler(void)
{
    struct cpu *c = mycpu();
    c->proc = NULL;
    for (;;) {
        irq_on();                       /* let pending interrupts in, then mask again */
        isb();
        irq_off();
        spin_lock(&proc_lock);
        struct proc *p = runq_pop();
        if (!p) {
            c->idle = true;
            spin_unlock(&proc_lock);    /* interrupts stay masked: WFI still wakes on one */
            wfi();
            c->idle = false;
            continue;
        }
        p->state = P_RUNNING;
        p->cpu = c->id;
        c->proc = p;
        need_resched[c->id] = false;
        vm_activate(&p->vm);
        fpu_restore(&p->fp);
        c->ctx_switches++;
        stat_inc(STAT_CTX_SWITCHES);
        ctx_switch(&c->sched_ctx, &p->ctx);
        /* p gave up the CPU and still holds proc_lock on our behalf. */
        vm_activate(NULL);              /* never leave TTBR0 on tables that may be freed */
        c->proc = NULL;
        spin_unlock(&proc_lock);
    }
}

/* Switch from the current process to this CPU's scheduler.  The caller
 * holds proc_lock (and nothing else) and has already changed p->state. */
static void sched(void)
{
    struct proc *p = myproc();
    struct cpu *c = mycpu();
    if (!spin_held(&proc_lock)) panic("sched: proc_lock not held");
    if (c->irq_depth != 1) panic("sched: holding %d locks", c->irq_depth);
    if (p->state == P_RUNNING) panic("sched: still running");
    if (*(uint64_t *)p->kstack != 0x57524e4b53544b30UL) panic("kernel stack overflow in pid %d", p->pid);
    bool was_on = c->irq_was_on;
    fpu_save(&p->fp);
    ctx_switch(&p->ctx, &c->sched_ctx);
    mycpu()->irq_was_on = was_on;       /* we may have moved to another CPU */
}

void proc_yield(void)
{
    struct proc *p = myproc();
    spin_lock(&proc_lock);
    runq_push(p);
    sched();
    spin_unlock(&proc_lock);
}

void proc_tick(void)
{
    struct cpu *c = mycpu();
    if (c->proc) c->proc->ticks++;
    need_resched[c->id] = true;
}

bool proc_need_resched(void)
{
    irq_push();
    bool r = need_resched[mycpu()->id];
    irq_pop();
    return r;
}

/* Atomically release lk and sleep on chan; reacquire lk when woken.  The
 * wakeup cannot be missed: proc_lock is taken before lk is released, and
 * proc_wakeup needs proc_lock. */
void proc_sleep(void *chan, struct spinlock *lk)
{
    struct proc *p = myproc();
    if (lk != &proc_lock) {
        spin_lock(&proc_lock);
        spin_unlock(lk);
    }
    p->chan = chan;
    p->state = P_SLEEPING;
    sched();
    p->chan = NULL;
    if (lk != &proc_lock) {
        spin_unlock(&proc_lock);
        spin_lock(lk);
    }
}

static void wakeup_locked(void *chan)
{
    for (struct proc *p = ptable; p < ptable + NPROC; p++)
        if (p->state == P_SLEEPING && p->chan == chan) make_runnable(p);
}

void proc_wakeup(void *chan)
{
    spin_lock(&proc_lock);
    wakeup_locked(chan);
    spin_unlock(&proc_lock);
}

bool proc_killed(struct proc *p)
{
    spin_lock(&proc_lock);
    bool k = p->killed;
    spin_unlock(&proc_lock);
    return k;
}

void proc_kill_self(void)
{
    spin_lock(&proc_lock);
    myproc()->killed = true;
    spin_unlock(&proc_lock);
}

int proc_kill(int pid)
{
    if (initproc && pid == initproc->pid) return -EPERM;   /* init must never exit */
    spin_lock(&proc_lock);
    for (struct proc *p = ptable; p < ptable + NPROC; p++) {
        if (p->pid == pid && p->state != P_UNUSED && p->state != P_ZOMBIE) {
            p->killed = true;
            if (p->state == P_SLEEPING) make_runnable(p);   /* let it notice */
            spin_unlock(&proc_lock);
            return 0;
        }
    }
    spin_unlock(&proc_lock);
    return -ESRCH;
}

/* ---------------------------------------------------- creation */

#define KSTACK_MAGIC 0x57524e4b53544b30UL    /* canary at the bottom of every kernel stack */
static void fork_return(void);

/* Find a free slot and give it a kernel stack whose top holds the trap
 * frame and whose saved context starts in fork_return(). */
static struct proc *proc_alloc(void)
{
    spin_lock(&proc_lock);
    struct proc *p = NULL;
    for (struct proc *q = ptable; q < ptable + NPROC; q++)
        if (q->state == P_UNUSED) {
            p = q;
            break;
        }
    if (!p) {
        spin_unlock(&proc_lock);
        return NULL;
    }
    p->state = P_EMBRYO;
    p->pid = next_pid++;
    spin_unlock(&proc_lock);

    paddr_t ks = pages_alloc(2);   /* KSTACK_PAGES = 4 = order 2 */
    if (!ks) {
        spin_lock(&proc_lock);
        p->state = P_UNUSED;
        spin_unlock(&proc_lock);
        return NULL;
    }
    p->kstack = P2V(ks);
    *(uint64_t *)p->kstack = KSTACK_MAGIC;
    p->tf = (struct trapframe *)(p->kstack + KSTACK_PAGES * PAGE_SIZE - sizeof(struct trapframe));
    memset(p->tf, 0, sizeof *p->tf);
    memset(&p->ctx, 0, sizeof p->ctx);
    p->ctx.lr = (uint64_t)fork_return;
    p->ctx.sp = (uint64_t)p->tf;
    memset(&p->fp, 0, sizeof p->fp);
    memset(&p->vm, 0, sizeof p->vm);
    memset(p->ofile, 0, sizeof p->ofile);
    p->cwd = NULL;
    p->killed = false;
    p->xstatus = 0;
    p->ticks = 0;
    p->chan = NULL;
    p->parent = NULL;
    p->name[0] = 0;
    return p;
}

/* Release a process slot.  Caller holds proc_lock; the address space is gone. */
static void proc_free(struct proc *p)
{
    if (p->kstack) pages_free(V2P(p->kstack), 2);
    p->kstack = NULL;
    p->tf = NULL;
    p->pid = 0;
    p->parent = NULL;
    p->state = P_UNUSED;
}

/* A new process's first instructions in the kernel (via ctx_switch). */
static void fork_return(void)
{
    static bool fs_ready;
    spin_unlock(&proc_lock);     /* held since the scheduler picked us */
    if (!fs_ready) {             /* first process: mount needs a process context to sleep */
        fs_ready = true;
        fs_init(ROOTDEV);
        myproc()->cwd = namei("/");
        if (!myproc()->cwd) panic("no root directory");
    }
    trap_return_to_user(myproc()->tf);
}

/* The first process runs a tiny program (initcode.S) that execs /bin/init. */
extern const uint8_t initcode_start[], initcode_end[];

void user_init(void)
{
    struct proc *p = proc_alloc();
    if (!p || vm_create(&p->vm) != 0) panic("user_init");
    size_t n = (size_t)(initcode_end - initcode_start);
    if (n > PAGE_SIZE) panic("initcode too big");
    paddr_t page = page_alloc();
    if (!page) panic("user_init: memory");
    memcpy(P2V(page), initcode_start, n);
    if (vm_map(&p->vm, USER_MIN, page, VM_R | VM_X)) panic("user_init: map");
    vm_sync_icache(&p->vm, USER_MIN, n);
    p->vm.heap_start = p->vm.brk = USER_MIN + PAGE_SIZE;
    p->tf->elr = USER_MIN;
    p->tf->sp = USER_TOP;
    p->tf->spsr = 0;              /* EL0t, interrupts unmasked */
    strlcpy(p->name, "initcode", sizeof p->name);
    initproc = p;
    spin_lock(&proc_lock);
    make_runnable(p);
    spin_unlock(&proc_lock);
}

int proc_fork(void)
{
    struct proc *p = myproc(), *np = proc_alloc();
    if (!np) return -EAGAIN;
    int r = vm_create(&np->vm);
    if (r == 0) r = vm_clone_cow(&np->vm, &p->vm);
    if (r != 0) {
        vm_destroy(&np->vm);
        spin_lock(&proc_lock);
        proc_free(np);
        spin_unlock(&proc_lock);
        return r;
    }
    *np->tf = *p->tf;
    np->tf->x[0] = 0;                 /* the child sees fork() return 0 */
    fpu_save(&p->fp);                 /* live FP registers belong to the parent */
    np->fp = p->fp;
    for (int i = 0; i < NOFILE; i++)
        if (p->ofile[i]) np->ofile[i] = file_dup(p->ofile[i]);
    np->cwd = idup(p->cwd);
    strlcpy(np->name, p->name, sizeof np->name);
    np->personality = p->personality;
    int pid = np->pid;
    spin_lock(&proc_lock);
    np->parent = p;
    make_runnable(np);
    spin_unlock(&proc_lock);
    return pid;
}

void proc_exit(int status)
{
    struct proc *p = myproc();
    if (p == initproc) panic("init exited with status %d", status);
    for (int fd = 0; fd < NOFILE; fd++)
        if (p->ofile[fd]) {
            file_close(p->ofile[fd]);
            p->ofile[fd] = NULL;
        }
    log_begin_op();
    iput(p->cwd);
    log_end_op();
    p->cwd = NULL;
    /* Free the address space now rather than in wait(), so zombies cost only
     * a kernel stack.  Leave the tables first: TTBR0 must not point at them. */
    vm_activate(NULL);
    vm_destroy(&p->vm);

    spin_lock(&proc_lock);
    for (struct proc *q = ptable; q < ptable + NPROC; q++)
        if (q->parent == p) {
            q->parent = initproc;
            if (q->state == P_ZOMBIE) wakeup_locked(initproc);
        }
    if (p->parent) wakeup_locked(p->parent);
    p->xstatus = status;
    p->state = P_ZOMBIE;
    sched();
    panic("zombie ran");
}

int proc_wait(int pid, uint64_t status_uaddr, int options)
{
    struct proc *p = myproc();
    spin_lock(&proc_lock);
    for (;;) {
        bool have = false;
        for (struct proc *q = ptable; q < ptable + NPROC; q++) {
            if (q->parent != p || (pid > 0 && q->pid != pid)) continue;
            have = true;
            if (q->state != P_ZOMBIE) continue;
            int got = q->pid;
            if (status_uaddr && copyout(&p->vm, status_uaddr, &q->xstatus, sizeof q->xstatus) < 0) {
                spin_unlock(&proc_lock);
                return -EFAULT;
            }
            proc_free(q);
            spin_unlock(&proc_lock);
            return got;
        }
        if (!have) {
            spin_unlock(&proc_lock);
            return -ECHILD;
        }
        if (options & WNOHANG) {
            spin_unlock(&proc_lock);
            return 0;
        }
        if (p->killed) {
            spin_unlock(&proc_lock);
            return -EINTR;
        }
        proc_sleep(p, &proc_lock);     /* exit() wakes the parent */
    }
}

int proc_info(uint64_t uaddr, int max)
{
    struct procinfo rows[NPROC];
    int n = 0;
    spin_lock(&proc_lock);
    for (struct proc *p = ptable; p < ptable + NPROC && n < max; p++) {
        if (p->state == P_UNUSED || p->state == P_EMBRYO) continue;
        struct procinfo *r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->pid = p->pid;
        r->ppid = p->parent ? p->parent->pid : 0;
        r->state = p->state == P_RUNNABLE ? PS_RUNNABLE : p->state == P_RUNNING ? PS_RUNNING
                 : p->state == P_SLEEPING ? PS_SLEEPING : PS_ZOMBIE;
        r->cpu = p->cpu;
        r->mem = p->vm.resident * PAGE_SIZE;
        r->ticks = p->ticks;
        strlcpy(r->name, p->name, sizeof r->name);
    }
    spin_unlock(&proc_lock);
    if (copyout(&myproc()->vm, uaddr, rows, (size_t)n * sizeof rows[0]) < 0) return -EFAULT;
    return n;
}
