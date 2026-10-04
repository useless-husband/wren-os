/* The wren-os user C library: system calls, strings, formatted output,
 * a heap allocator and a few helpers.  System calls return a negative
 * error code (see <wren/errno.h>) on failure. */
#ifndef ULIB_H
#define ULIB_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <wren/errno.h>
#include <wren/fcntl.h>
#include <wren/procinfo.h>
#include <wren/stat.h>
#include "../../lib/kstring.h"
#include "../../lib/fmt.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* system calls */
int     fork(void);
_Noreturn void exit(int status);
int     waitpid(int pid, int *status, int options);
int     pipe(int fds[2]);
long    read(int fd, void *buf, size_t n);
long    write(int fd, const void *buf, size_t n);
int     close(int fd);
int     kill(int pid);
int     exec(const char *path, char *const argv[]);
int     open(const char *path, int flags);
int     mknod(const char *path, int major);
int     unlink(const char *path);
int     fstat(int fd, struct stat *st);
int     link(const char *old, const char *new);
int     mkdir(const char *path);
int     chdir(const char *path);
int     dup(int fd);
int     dup2(int fd, int newfd);
int     getpid(void);
void   *sbrk(long incr);       /* returns the old break, or (void *)-errno */
int     sleep(long ms);
long    uptime(void);          /* milliseconds */
int     procinfo(struct procinfo *rows, int max);
_Noreturn void poweroff(void);
int     getcpu(void);
long    lseek(int fd, long off, int whence);
int     crashctl(int op, long a1, long a2);
int     getppid(void);
long    kstat(int which);

static inline int wait(int *status) { return waitpid(-1, status, 0); }
static inline bool sbrk_failed(void *p) { return (intptr_t)p < 0 && (intptr_t)p > -4096; }

/* I/O */
int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  fprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  vfprintf(int fd, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  readline(int fd, char *buf, int max);     /* reads up to '\n'; returns length, 0 at EOF */
long write_all(int fd, const void *buf, size_t n);
const char *strerror(int err);                /* err may be negative */

/* memory */
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);

/* misc */
int      atoi(const char *s);
long     atol(const char *s);
bool     isdigit_c(int c);
bool     isspace_c(int c);
uint64_t cycles(void);          /* CNTVCT_EL0, readable from user mode */
uint64_t cycles_freq(void);
uint64_t now_ns(void);

/* deterministic pseudo-random numbers for tests (xorshift64*) */
struct rng { uint64_t s; };
static inline void rng_seed(struct rng *r, uint64_t seed) { r->s = seed ? seed : 0x9e3779b97f4a7c15ull; }
static inline uint64_t rng_next(struct rng *r)
{
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 2685821657736338717ull;
}
static inline uint64_t rng_range(struct rng *r, uint64_t n) { return n ? rng_next(r) % n : 0; }

#endif
