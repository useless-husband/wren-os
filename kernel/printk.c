#include "printk.h"
#include "spinlock.h"
#include "cpu.h"
#include "memlayout.h"
#include "../lib/fmt.h"
#include <stdarg.h>

volatile int panicking;
static struct spinlock print_lock = SPINLOCK_INIT("printk");

static void sink(void *ctx, char c)
{
    (void)ctx;
    if (c == '\n') uart_putc('\r');
    uart_putc(c);
}

int printk(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool locked = !panicking && mycpu();
    if (locked) spin_lock(&print_lock);
    int n = vformat(sink, NULL, fmt, ap);
    if (locked) spin_unlock(&print_lock);
    va_end(ap);
    return n;
}

/* Walk the frame-pointer chain (the kernel is built with frame pointers).
 * The harness turns these addresses into function names with addr2line. */
void backtrace(uint64_t fp)
{
    printk("backtrace:\n");
    for (int depth = 0; depth < 16 && fp >= KIMAGE_VBASE && !(fp & 7); depth++) {
        uint64_t *frame = (uint64_t *)fp;
        uint64_t lr = frame[1];
        if (lr < KIMAGE_VBASE) break;
        printk("  %p\n", (void *)(lr - 4));
        fp = frame[0];
    }
}

void panic(const char *fmt, ...)
{
    irq_off();
    int first = !__atomic_exchange_n(&panicking, 1, __ATOMIC_SEQ_CST);
    if (!first) for (;;) wfi();          /* another CPU is already reporting */
    stop_other_cpus();
    va_list ap;
    va_start(ap, fmt);
    struct cpu *c = mycpu();
    printk("\nPANIC on cpu %d: ", c ? c->id : -1);
    vformat(sink, NULL, fmt, ap);
    printk("\n");
    va_end(ap);
    backtrace((uint64_t)__builtin_frame_address(0));
    printk("system halted\n");
    for (;;) wfi();
}
