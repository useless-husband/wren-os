#ifndef LIB_FMT_H
#define LIB_FMT_H
#include <stdarg.h>
#include <stddef.h>

/* printf-style formatting into a character sink.
 * Supports %d %i %u %x %X %o %p %s %c %% with flags '-' '0', a width
 * (number or '*'), a precision for %s, and length modifiers l, ll, z.
 * Returns the number of characters produced. */
typedef void (*fmt_sink)(void *ctx, char c);
int vformat(fmt_sink sink, void *ctx, const char *fmt, va_list ap);

int vsnformat(char *buf, size_t size, const char *fmt, va_list ap);
int snformat(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif
