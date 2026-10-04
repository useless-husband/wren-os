#ifndef KERNEL_PRINTK_H
#define KERNEL_PRINTK_H
#include "types.h"

extern volatile int panicking;

int  printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
NORETURN void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void backtrace(uint64_t fp);
void stop_other_cpus(void);   /* smp.c */

#define KASSERT(c) do { if (unlikely(!(c))) panic("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__); } while (0)

/* uart.c */
void uart_init(uint64_t base);
void uart_putc(char c);
int  uart_getc(void);       /* -1 if nothing is waiting */
void uart_intr(void);
bool uart_ready(void);

#endif
