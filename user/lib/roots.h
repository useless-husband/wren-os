/* Conservative roots shared by the garbage collector (gc_wren.c) and the
 * leak checker (malloc.c): every place a live pointer can hide in a
 * single-threaded wren-os program. */
#ifndef ROOTS_H
#define ROOTS_H

/* Called with [lo, hi) ranges of memory to scan; lo and hi need not be aligned. */
typedef void (*root_fn)(void *ctx, const void *lo, const void *hi);

/* Report, in this order: the callee-saved registers and the whole live
 * stack (one range, from inside this call up to the stack top recorded by
 * crt0), then the program's writable globals (.data and .bss) minus
 * [skip_lo, skip_hi), which lets a caller hide its own bookkeeping. */
void roots_scan(root_fn fn, void *ctx, const void *skip_lo, const void *skip_hi);

#endif
