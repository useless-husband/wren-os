#ifndef KERNEL_PROC_H
#define KERNEL_PROC_H
struct proc { int pid; };
struct proc *myproc(void);
void proc_sleep(void *chan, void *lk);
void proc_wakeup(void *chan);
#endif
