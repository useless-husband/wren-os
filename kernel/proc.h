/* Processes and the scheduler.
 *
 * Locking: one spinlock, proc_lock, protects every process's state, chan,
 * parent, killed and exit status, the run queue and pid allocation.  It is
 * held across ctx_switch(): whoever switches away holds it, whoever resumes
 * releases it.  Coarse, but each critical section is short and it makes the
 * sleep/wakeup and exit/wait races easy to reason about (see docs/DESIGN.md). */
#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H
#include "types.h"
#include "param.h"
#include "cpu.h"
#include "vm.h"
#include "spinlock.h"

struct trapframe;
struct file;
struct inode;

enum procstate { P_UNUSED, P_EMBRYO, P_RUNNABLE, P_RUNNING, P_SLEEPING, P_ZOMBIE };

struct fpstate {
    __uint128_t q[32];
    uint64_t    fpcr, fpsr;
} __attribute__((aligned(16)));

struct proc {
    enum procstate   state;
    int              pid;
    struct proc     *parent;
    void            *chan;          /* sleeping on this */
    bool             killed;
    int              xstatus;
    struct proc     *rq_next;       /* run-queue link */
    int              cpu;           /* CPU it runs or last ran on */
    uint64_t         ticks;         /* ticks spent running */
    uint8_t         *kstack;        /* KSTACK_PAGES pages */
    struct trapframe *tf;           /* user registers, at the top of kstack */
    struct context   ctx;           /* kernel registers while switched out */
    struct fpstate   fp;            /* user FP/SIMD registers while switched out */
    struct vmspace   vm;
    struct file     *ofile[NOFILE];
    struct inode    *cwd;
    char             name[16];
    uint32_t         personality;   /* PER_* flags: kept by fork and exec */
};

extern struct spinlock proc_lock;

void proc_init(void);
void user_init(void);
NORETURN void scheduler(void);

struct proc *myproc(void);
struct vmspace *proc_vm(struct proc *p);
void proc_sleep(void *chan, struct spinlock *lk);
void proc_wakeup(void *chan);
void proc_yield(void);
void proc_tick(void);                 /* timer interrupt on a CPU running a process */
bool proc_need_resched(void);
bool proc_killed(struct proc *p);
void proc_kill_self(void);
NORETURN void proc_exit(int status);

int  proc_fork(void);
int  proc_wait(int pid, uint64_t status_uaddr, int options);
int  proc_kill(int pid);
int  proc_info(uint64_t uaddr, int max);
int  proc_exec(const char *path, char *const argv[], int argc);

void ctx_switch(struct context *from, struct context *to);
void fpu_save(struct fpstate *fp);
void fpu_restore(struct fpstate *fp);

#endif
