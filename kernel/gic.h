/* GICv3 interrupt controller, the per-CPU virtual timer and inter-processor interrupts. */
#ifndef KERNEL_GIC_H
#define KERNEL_GIC_H
#include "types.h"

struct platform;

#define SGI_WAKEUP 0     /* an idle CPU has work: just leave WFI */
#define SGI_STOP   1     /* panic on another CPU: halt */

typedef void (*irq_handler_t)(void *arg);

void gic_init(const struct platform *pf);      /* distributor, once */
int  gic_cpu_init(void);                       /* redistributor + CPU interface, every CPU */
void gic_enable(uint32_t intid, irq_handler_t h, void *arg);   /* SPIs go to CPU 0 */
void gic_handle_irq(void);
void gic_send_sgi(int cpu, int sgi);

void timer_cpu_init(void);                     /* start this CPU's tick */
extern volatile uint64_t ticks;                /* global tick count, advanced by CPU 0 */
extern struct spinlock tick_lock;
uint64_t uptime_ms(void);

#endif
