#include "types.h"
#include "printk.h"
struct proc;
struct proc *myproc(void) { return 0; }
void proc_sleep(void *c, void *lk) { (void)c; (void)lk; }
void proc_wakeup(void *c) { (void)c; }
void console_input(char c) { (void)c; }
void stop_other_cpus(void) {}
void kernel_sync_trap(void *tf) { (void)tf; panic("sync trap"); }
void kernel_irq_trap(void *tf) { (void)tf; }
void user_sync_trap(void *tf) { (void)tf; }
void user_irq_trap(void *tf) { (void)tf; }
void bad_trap(void *tf, int k) { (void)tf; panic("bad trap %d", k); }
