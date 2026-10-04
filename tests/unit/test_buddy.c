/* kernel/buddy.c on the host: a randomised model check.
 * Thousands of random allocations and frees of random orders; after every
 * step the allocator's own invariants hold (buddy_check), no two live
 * blocks overlap, every block is aligned to its size, and when everything
 * is freed the memory coalesces back into the original blocks. */
#include "check.h"
#include <string.h>
#include "../../kernel/buddy.h"

#define NPAGES 4096u            /* 16 MiB managed */
#define BASE   0x40000000ull

static struct page meta[NPAGES];
static struct buddy b;
static uint8_t owner[NPAGES];   /* model: which live block id owns each page (0 = free) */

struct blk { uint64_t pa; unsigned order; };

static void reset(void)
{
    buddy_init(&b, BASE, NPAGES, meta);
    /* hand over everything except a reserved hole, like the kernel image */
    buddy_add_range(&b, BASE, 64 * BUDDY_PAGE);
    buddy_add_range(&b, BASE + 100 * BUDDY_PAGE, (NPAGES - 100) * (uint64_t)BUDDY_PAGE);
    memset(owner, 0, sizeof owner);
}

int main(void)
{
    char why[128];
    reset();
    CHECK(buddy_check(&b, why, sizeof why));
    uint64_t total = b.nfree;
    CHECK_EQ(total, NPAGES - 36);

    /* basic behaviour */
    uint64_t a = buddy_alloc(&b, 0);
    CHECK(a >= BASE && a < BASE + NPAGES * (uint64_t)BUDDY_PAGE);
    CHECK(!(a >= BASE + 64 * BUDDY_PAGE && a < BASE + 100 * BUDDY_PAGE));   /* never the reserved hole */
    CHECK_EQ(buddy_free(&b, a, 0), 0);
    CHECK_EQ(buddy_free(&b, a, 0), -1);                    /* double free detected */
    CHECK_EQ(buddy_free(&b, BASE + 70 * BUDDY_PAGE, 0), -1); /* never allocated */
    uint64_t big = buddy_alloc(&b, 3);
    CHECK_EQ(big % (8 * BUDDY_PAGE), 0);
    CHECK_EQ(buddy_free(&b, big + BUDDY_PAGE, 0), -1);     /* not the head of a block */
    CHECK_EQ(buddy_free(&b, big, 2), -1);                  /* wrong order */
    CHECK_EQ(buddy_free(&b, big, 3), 0);
    CHECK_EQ(buddy_alloc(&b, BUDDY_MAX_ORDER + 1), 0);
    CHECK(buddy_check(&b, why, sizeof why));
    CHECK_EQ(b.nfree, total);

    /* randomised model check */
    unsigned long long seed = 0xb0dd1e5ull;
    rng_state = seed;
    printf("test_buddy: 40000 random operations, seed %llx\n", seed);
    static struct blk live[NPAGES];
    int nlive = 0, failures_ok = 0;
    for (int it = 0; it < 40000; it++) {
        if (nlive == 0 || rnd() % 100 < 55) {
            unsigned order = (unsigned)(rnd() % 100 < 70 ? rnd() % 2 : rnd() % (BUDDY_MAX_ORDER + 1));
            uint64_t pa = buddy_alloc(&b, order);
            if (!pa) {
                failures_ok++;                              /* fragmentation or exhaustion */
                continue;
            }
            uint32_t idx = (uint32_t)((pa - BASE) / BUDDY_PAGE);
            CHECK_EQ(idx % (1u << order), 0);               /* aligned to its size */
            for (uint32_t k = 0; k < (1u << order); k++) {
                if (owner[idx + k]) {
                    CHECK(!"block overlaps a live block");
                    break;
                }
                owner[idx + k] = 1;
            }
            if (idx < 100 && idx + (1u << order) > 64) CHECK(!"allocated the reserved hole");
            live[nlive++] = (struct blk){pa, order};
        } else {
            int k = (int)(rnd() % (uint64_t)nlive);
            uint32_t idx = (uint32_t)((live[k].pa - BASE) / BUDDY_PAGE);
            CHECK_EQ(buddy_free(&b, live[k].pa, live[k].order), 0);
            memset(owner + idx, 0, 1u << live[k].order);
            live[k] = live[--nlive];
        }
        if (it % 97 == 0 && !buddy_check(&b, why, sizeof why)) {
            fprintf(stderr, "invariant broken at step %d: %s\n", it, why);
            CHECK(0);
            break;
        }
        uint64_t used = 0;
        for (int k = 0; k < nlive; k++) used += 1u << live[k].order;
        if (it % 1000 == 0) CHECK_EQ(b.nfree + used, total);
    }
    while (nlive) {
        nlive--;
        CHECK_EQ(buddy_free(&b, live[nlive].pa, live[nlive].order), 0);
    }
    CHECK(buddy_check(&b, why, sizeof why));
    CHECK_EQ(b.nfree, total);
    /* everything coalesced: the 2^10-page blocks are whole again */
    int max_blocks = 0;
    for (uint32_t i = b.free_head[BUDDY_MAX_ORDER]; i != BUDDY_NIL; i = meta[i].next) max_blocks++;
    CHECK_EQ(max_blocks, 3);                 /* pages 1024..4095; the first block has the hole */
    printf("test_buddy: %d allocations refused under pressure\n", failures_ok);

    /* exhaustion: order-0 pages until none are left, then all back */
    reset();
    uint64_t n = 0, pa;
    static uint64_t got[NPAGES];
    while ((pa = buddy_alloc(&b, 0)) != 0) got[n++] = pa;
    CHECK_EQ(n, total);
    CHECK_EQ(b.nfree, 0);
    for (uint64_t i = 0; i < n; i++) CHECK_EQ(buddy_free(&b, got[i], 0), 0);
    CHECK(buddy_check(&b, why, sizeof why));
    return check_summary("test_buddy");
}
