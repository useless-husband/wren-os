/* Minimal unit-test helpers for the host-side tests. */
#ifndef UNIT_CHECK_H
#define UNIT_CHECK_H
#include <stdio.h>
#include <stdlib.h>

static int checks_run, checks_failed;

#define CHECK(cond) do { checks_run++; if (!(cond)) { checks_failed++; \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); checks_run++; \
    if (a_ != b_) { checks_failed++; fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", \
    __FILE__, __LINE__, #a, a_, b_); } } while (0)

static int check_summary(const char *suite)
{
    printf("%s: %d checks, %d failed\n", suite, checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}

/* xorshift64*: deterministic randomness; the seed is printed by each test. */
static unsigned long long rng_state = 88172645463325252ull;
static inline unsigned long long rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ull;
}

#endif
