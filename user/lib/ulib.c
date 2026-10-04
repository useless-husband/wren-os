#include "ulib.h"

/* printf builds the whole message, then issues one write(), so lines
 * from concurrent processes do not interleave mid-line. */
struct outbuf {
    int  fd;
    int  len, total;
    char buf[512];
};

static void flush_out(struct outbuf *o)
{
    if (o->len) write_all(o->fd, o->buf, (size_t)o->len);
    o->len = 0;
}

static void out_sink(void *ctx, char c)
{
    struct outbuf *o = ctx;
    if (o->len == (int)sizeof o->buf) flush_out(o);
    o->buf[o->len++] = c;
    o->total++;
}

int vfprintf(int fd, const char *fmt, va_list ap)
{
    struct outbuf o = {.fd = fd};
    vformat(out_sink, &o, fmt, ap);
    flush_out(&o);
    return o.total;
}

int fprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(1, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnformat(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

long write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;
    while (done < n) {
        long r = write(fd, p + done, n - done);
        if (r <= 0) return done ? (long)done : r;
        done += (size_t)r;
    }
    return (long)done;
}

int readline(int fd, char *buf, int max)
{
    int n = 0;
    while (n < max - 1) {
        char c;
        long r = read(fd, &c, 1);
        if (r <= 0) break;
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = 0;
    return n;
}

const char *strerror(int err)
{
    if (err < 0) err = -err;
    switch (err) {
    case 0: return "success";
    case EPERM: return "operation not permitted";
    case ENOENT: return "no such file or directory";
    case ESRCH: return "no such process";
    case EINTR: return "interrupted";
    case EIO: return "i/o error";
    case ENXIO: return "no such device";
    case E2BIG: return "argument list too long";
    case ENOEXEC: return "exec format error";
    case EBADF: return "bad file descriptor";
    case ECHILD: return "no child processes";
    case EAGAIN: return "resource temporarily unavailable";
    case ENOMEM: return "out of memory";
    case EACCES: return "permission denied";
    case EFAULT: return "bad address";
    case EEXIST: return "file exists";
    case EXDEV: return "cross-device link";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "is a directory";
    case EINVAL: return "invalid argument";
    case ENFILE: return "file table full";
    case EMFILE: return "too many open files";
    case EFBIG: return "file too large";
    case ENOSPC: return "no space left on device";
    case ESPIPE: return "illegal seek";
    case EPIPE: return "broken pipe";
    case ENAMETOOLONG: return "file name too long";
    case ENOSYS: return "function not implemented";
    case ENOTEMPTY: return "directory not empty";
    default: return "unknown error";
    }
}

bool isdigit_c(int c) { return c >= '0' && c <= '9'; }
bool isspace_c(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

long atol(const char *s)
{
    while (isspace_c(*s)) s++;
    bool neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    long v = 0;
    while (isdigit_c(*s)) v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)atol(s); }

uint64_t cycles(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

uint64_t cycles_freq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

uint64_t now_ns(void)
{
    uint64_t c = cycles(), f = cycles_freq();
    return c / f * 1000000000ull + c % f * 1000000000ull / f;
}

/* ---- heap: first-fit free list with coalescing, grown with sbrk ---- */

struct block {
    size_t        size;    /* bytes of payload */
    struct block *next;    /* next free block (address order), when free */
};
#define HDR sizeof(struct block)
static struct block *free_list;

static void insert_free(struct block *b)
{
    struct block *prev = NULL, *cur = free_list;
    while (cur && cur < b) {
        prev = cur;
        cur = cur->next;
    }
    b->next = cur;
    if (prev) prev->next = b;
    else free_list = b;
    if (cur && (char *)b + HDR + b->size == (char *)cur) {         /* merge with the next block */
        b->size += HDR + cur->size;
        b->next = cur->next;
    }
    if (prev && (char *)prev + HDR + prev->size == (char *)b) {   /* and with the previous one */
        prev->size += HDR + b->size;
        prev->next = b->next;
    }
}

void *malloc(size_t n)
{
    if (n == 0) n = 1;
    n = (n + 15) & ~(size_t)15;
    for (struct block **pp = &free_list; *pp; pp = &(*pp)->next) {
        struct block *b = *pp;
        if (b->size < n) continue;
        if (b->size >= n + HDR + 32) {           /* split */
            struct block *rest = (struct block *)((char *)b + HDR + n);
            rest->size = b->size - n - HDR;
            rest->next = b->next;
            b->size = n;
            *pp = rest;
        } else {
            *pp = b->next;
        }
        return (char *)b + HDR;
    }
    size_t grow = n + HDR < 65536 ? 65536 : n + HDR;
    char *p = sbrk((long)grow);
    if (sbrk_failed(p)) return NULL;
    struct block *b = (struct block *)p;
    b->size = grow - HDR;
    insert_free(b);
    return malloc(n);
}

void free(void *ptr)
{
    if (ptr) insert_free((struct block *)((char *)ptr - HDR));
}

void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) return NULL;
    void *p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    if (!ptr) return malloc(n);
    struct block *b = (struct block *)((char *)ptr - HDR);
    if (b->size >= n) return ptr;
    void *q = malloc(n);
    if (q) {
        memcpy(q, ptr, b->size);
        free(ptr);
    }
    return q;
}
