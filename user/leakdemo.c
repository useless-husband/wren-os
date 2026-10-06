/* leakdemo: planted leaks for the leak checker.
 *
 *   leakcheck leakdemo         report at exit: exactly the 7 planted blocks
 *   leakcheck leakdemo clean   the same program, leaks fixed: nothing to report
 *   leakdemo now               report on demand, from inside main
 *
 * Planted (all sizes are multiples of 16, so they are reported exactly):
 *   plant_block   1 block  of 32 bytes, its only pointer overwritten
 *   plant_list    4 blocks of 48 bytes, a list whose head is dropped
 *   plant_cycle   2 blocks of 64 bytes that point at each other
 * Still reachable at exit, never reported: 3 list cells from a global, a
 * block held only through a pointer into its middle, and a block held only
 * from inside another reachable block (6 blocks, 304 bytes). */
#include "lib/ulib.h"

struct cell {
    struct cell *next;
    long         v[4];               /* 40 bytes: a 48-byte block */
};

static struct cell *kept_list;
static char *volatile kept_middle;  /* volatile: the store is the point */
static void **kept_table;
static bool clean;

static __attribute__((noinline)) void plant_block(void)
{
    char *volatile p = malloc(32);
    p[0] = 1;
    if (clean) free(p);
    p = NULL;
}

static __attribute__((noinline)) void plant_list(void)
{
    struct cell *head = NULL;
    for (int i = 0; i < 4; i++) {
        struct cell *c = malloc(sizeof *c);
        c->next = head;
        head = c;
    }
    while (clean && head) {
        struct cell *n = head->next;
        free(head);
        head = n;
    }
}

static __attribute__((noinline)) void plant_cycle(void)
{
    void **a = malloc(64), **b = malloc(64);
    a[0] = b;
    b[0] = a;
    if (clean) {
        free(a);
        free(b);
    }
}

static __attribute__((noinline)) void keep_some(void)
{
    for (int i = 0; i < 3; i++) {
        struct cell *c = malloc(sizeof *c);
        c->next = kept_list;
        kept_list = c;
    }
    kept_middle = (char *)malloc(80) + 40;
    kept_table = malloc(16);
    kept_table[0] = malloc(64);
    for (int i = 0; i < 10; i++) free(malloc(100 + (size_t)i * 16));   /* freed: never reported */
}

int main(int argc, char **argv)
{
    clean = argc > 1 && strcmp(argv[1], "clean") == 0;
    keep_some();
    plant_block();
    plant_list();
    plant_cycle();
    printf("leakdemo: %s\n", clean ? "no leaks planted" : "planted 7 leaked blocks, 352 bytes");
    if (argc > 1 && strcmp(argv[1], "now") == 0) leak_check();
    return 0;
}
