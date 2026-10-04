/* ARM PL011 UART (both QEMU virt and LeapVM emulate one).
 * Output is polled; input arrives by interrupt and goes to the console. */
#include "printk.h"
#include "arch.h"
#include "memlayout.h"
#include "console.h"

#define DR    0x000
#define FR    0x018
#define IBRD  0x024
#define FBRD  0x028
#define LCRH  0x02c
#define CR    0x030
#define IMSC  0x038
#define ICR   0x044

#define FR_RXFE (1u << 4)
#define FR_TXFF (1u << 5)

static uint64_t uart;   /* VA of the registers, 0 until uart_init */

bool uart_ready(void) { return uart != 0; }

void uart_init(uint64_t base)
{
    uart = (uint64_t)P2V(base);
    mmio_write32(uart + CR, 0);                       /* disable while configuring */
    mmio_write32(uart + IBRD, 13);                    /* 115200 baud from a 24 MHz clock */
    mmio_write32(uart + FBRD, 1);
    mmio_write32(uart + LCRH, (3u << 5) | (1u << 4)); /* 8 bits, FIFOs on */
    mmio_write32(uart + ICR, 0x7ff);
    mmio_write32(uart + IMSC, (1u << 4) | (1u << 6)); /* receive and receive-timeout interrupts */
    mmio_write32(uart + CR, (1u << 0) | (1u << 8) | (1u << 9));  /* UARTEN | TXE | RXE */
}

void uart_putc(char c)
{
    if (!uart) return;
    while (mmio_read32(uart + FR) & FR_TXFF) cpu_relax();
    mmio_write32(uart + DR, (uint8_t)c);
}

int uart_getc(void)
{
    if (!uart || (mmio_read32(uart + FR) & FR_RXFE)) return -1;
    return (int)(mmio_read32(uart + DR) & 0xff);
}

/* Acknowledge first, then drain. QEMU's PL011 raises the receive interrupt
 * only when the FIFO goes from empty to one byte; clearing it after the
 * drain loop can erase the interrupt of a byte that arrived in between, and
 * no further interrupt ever comes because the FIFO is never empty again. */
void uart_intr(void)
{
    int c;
    mmio_write32(uart + ICR, (1u << 4) | (1u << 6));
    while ((c = uart_getc()) >= 0) console_input((char)c);
}
