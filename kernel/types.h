#ifndef KERNEL_TYPES_H
#define KERNEL_TYPES_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t paddr_t;   /* physical address */
typedef uint64_t vaddr_t;   /* virtual address (user or kernel) */
typedef uint64_t pte_t;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ROUNDUP(x, a)   (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define ROUNDDOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define container_of(p, T, m) ((T *)((char *)(p) - offsetof(T, m)))
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define NORETURN __attribute__((noreturn))

#endif
