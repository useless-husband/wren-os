/* The console device (/dev/console, major 1): line-buffered input from the
 * UART interrupt with simple editing, unbuffered output. */
#include "console.h"
#include "file.h"
#include "printk.h"
#include "proc.h"
#include "spinlock.h"
#include "vm.h"
#include <wren/errno.h>

#define CTRL(x) ((x) - '@')
#define CBUF 256

static struct {
    struct spinlock lock;
    char     buf[CBUF];
    uint32_t r;     /* next byte to read */
    uint32_t w;     /* end of completed lines */
    uint32_t e;     /* end of the line being edited */
} cons;

static struct spinlock out_lock = SPINLOCK_INIT("console-out");

static void echo(char c)
{
    if (c == '\n') uart_putc('\r');
    uart_putc(c);
}

void console_input(char c)
{
    spin_lock(&cons.lock);
    switch (c) {
    case CTRL('U'):                     /* erase the line */
        while (cons.e != cons.w && cons.buf[(cons.e - 1) % CBUF] != '\n') {
            cons.e--;
            echo('\b'); echo(' '); echo('\b');
        }
        break;
    case 0x7f:
    case '\b':
        if (cons.e != cons.w) {
            cons.e--;
            echo('\b'); echo(' '); echo('\b');
        }
        break;
    default:
        if (c == 0 || cons.e - cons.r >= CBUF) break;
        if (c == '\r') c = '\n';
        cons.buf[cons.e++ % CBUF] = c;
        if (c != CTRL('D')) echo(c);
        if (c == '\n' || c == CTRL('D') || cons.e - cons.r == CBUF) {
            cons.w = cons.e;
            proc_wakeup(&cons.r);
        }
    }
    spin_unlock(&cons.lock);
}

static int console_read(uint64_t udst, int n)
{
    struct proc *p = myproc();
    int got = 0;
    spin_lock(&cons.lock);
    while (got < n) {
        while (cons.r == cons.w) {
            if (proc_killed(p)) {
                spin_unlock(&cons.lock);
                return -EINTR;
            }
            proc_sleep(&cons.r, &cons.lock);
        }
        char c = cons.buf[cons.r++ % CBUF];
        if (c == CTRL('D')) {            /* end of file */
            if (got > 0) cons.r--;       /* report the data first, EOF on the next read */
            break;
        }
        if (copyout(&p->vm, udst + (uint64_t)got, &c, 1) < 0) {
            if (!got) got = -EFAULT;
            break;
        }
        got++;
        if (c == '\n') break;
    }
    spin_unlock(&cons.lock);
    return got;
}

static int console_write(uint64_t usrc, int n)
{
    struct proc *p = myproc();
    char chunk[128];
    int done = 0;
    while (done < n) {
        int m = MIN(n - done, (int)sizeof chunk);
        if (copyin(&p->vm, chunk, usrc + (uint64_t)done, (size_t)m) < 0) return done ? done : -EFAULT;
        spin_lock(&out_lock);
        for (int i = 0; i < m; i++) echo(chunk[i]);
        spin_unlock(&out_lock);
        done += m;
    }
    return done;
}

void console_init(void)
{
    spin_init(&cons.lock, "console");
    devsw[CONSOLE_MAJOR].read = console_read;
    devsw[CONSOLE_MAJOR].write = console_write;
}
