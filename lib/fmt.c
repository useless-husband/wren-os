#include "fmt.h"
#include <stdint.h>

static int emit_n(fmt_sink sink, void *ctx, char c, int n)
{
    for (int i = 0; i < n; i++) sink(ctx, c);
    return n > 0 ? n : 0;
}


int vformat(fmt_sink sink, void *ctx, const char *fmt, va_list ap)
{
    int count = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            sink(ctx, *p);
            count++;
            continue;
        }
        p++;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else break;
        }
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            p++;
            prec = 0;
            while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }
        while (*p == 'l' || *p == 'z') { lng++; p++; }

        char tmp[24];
        const char *s = tmp;
        int len = 0, neg = 0;
        uint64_t u = 0;
        unsigned base = 10;
        int upper = 0;
        switch (*p) {
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            neg = v < 0;
            u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            goto number;
        }
        case 'u': u = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); goto number;
        case 'X': upper = 1; /* fall through */
        case 'x': base = 16; u = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); goto number;
        case 'o': base = 8; u = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); goto number;
        case 'p':
            base = 16;
            u = (uint64_t)(uintptr_t)va_arg(ap, void *);
            sink(ctx, '0');
            sink(ctx, 'x');
            count += 2;
            goto number;
        number: {
            const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            char *e = tmp + sizeof tmp;
            do { *--e = digits[u % base]; u /= base; } while (u);
            if (neg) {
                if (zero && !left) {          /* sign goes before the zero padding */
                    sink(ctx, '-');
                    count++;
                    width--;
                } else {
                    *--e = '-';
                }
            }
            s = e;
            len = (int)(tmp + sizeof tmp - e);
            break;
        }
        case 's':
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (s[len] && (prec < 0 || len < prec)) len++;
            zero = 0;
            break;
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            len = 1;
            zero = 0;
            break;
        case '%':
            tmp[0] = '%';
            len = 1;
            break;
        default:              /* unknown conversion: print it verbatim */
            sink(ctx, '%');
            count++;
            if (!*p) return count;
            tmp[0] = *p;
            len = 1;
            break;
        }
        if (!left) count += emit_n(sink, ctx, zero ? '0' : ' ', width - len);
        for (int i = 0; i < len; i++) sink(ctx, s[i]);
        count += len;
        if (left) count += emit_n(sink, ctx, ' ', width - len);
    }
    return count;
}

struct bufctx {
    char  *buf;
    size_t size, pos;
};

static void buf_sink(void *ctx, char c)
{
    struct bufctx *b = ctx;
    if (b->pos + 1 < b->size) b->buf[b->pos] = c;
    b->pos++;
}

int vsnformat(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct bufctx b = {buf, size, 0};
    int n = vformat(buf_sink, &b, fmt, ap);
    if (size) buf[b.pos < size ? b.pos : size - 1] = 0;
    return n;
}

int snformat(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnformat(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
