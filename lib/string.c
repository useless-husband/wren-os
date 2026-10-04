/* Freestanding memory/string routines.  Built with -fno-builtin so the
 * compiler cannot turn these loops back into calls to themselves. */
#include "kstring.h"
#include <stdint.h>

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    uint64_t pat = (unsigned char)c;
    pat |= pat << 8;
    pat |= pat << 16;
    pat |= pat << 32;
    while (n && ((uintptr_t)d & 7)) { *d++ = (unsigned char)c; n--; }
    for (; n >= 32; n -= 32, d += 32) {
        ((uint64_t *)d)[0] = pat;
        ((uint64_t *)d)[1] = pat;
        ((uint64_t *)d)[2] = pat;
        ((uint64_t *)d)[3] = pat;
    }
    for (; n >= 8; n -= 8, d += 8) *(uint64_t *)d = pat;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        for (; n >= 32; n -= 32, d += 32, s += 32) {
            uint64_t a = ((const uint64_t *)s)[0], b = ((const uint64_t *)s)[1];
            uint64_t c = ((const uint64_t *)s)[2], e = ((const uint64_t *)s)[3];
            ((uint64_t *)d)[0] = a;
            ((uint64_t *)d)[1] = b;
            ((uint64_t *)d)[2] = c;
            ((uint64_t *)d)[3] = e;
        }
        for (; n >= 8; n -= 8, d += 8, s += 8) *(uint64_t *)d = *(const uint64_t *)s;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) return memcpy(dst, src, n);
    d += n;
    s += n;
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t have = strnlen(dst, size);
    if (have == size) return size + strlen(src);
    return have + strlcpy(dst + have, src, size - have);
}
