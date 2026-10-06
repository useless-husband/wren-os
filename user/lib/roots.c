#include "roots.h"
#include <stdint.h>

extern char __data_start[], _end[];             /* user/user.ld */
extern uintptr_t __stack_top;                   /* crt0.S */
void roots_with_registers(void (*fn)(void *ctx, void *sp), void *ctx);   /* regs.S */

struct scan {
    root_fn     fn;
    void       *ctx;
    const char *skip_lo, *skip_hi;
};

/* Runs below the register spill area: its own frame is never scanned. */
static void scan_from(void *arg, void *sp)
{
    struct scan *s = arg;
    s->fn(s->ctx, sp, (const void *)__stack_top);
    const char *lo = __data_start, *hi = _end;
    if (s->skip_lo >= lo && s->skip_hi <= hi && s->skip_lo < s->skip_hi) {
        s->fn(s->ctx, lo, s->skip_lo);
        s->fn(s->ctx, s->skip_hi, hi);
    } else {
        s->fn(s->ctx, lo, hi);
    }
}

void roots_scan(root_fn fn, void *ctx, const void *skip_lo, const void *skip_hi)
{
    struct scan s = {fn, ctx, skip_lo, skip_hi};
    roots_with_registers(scan_from, &s);
}
