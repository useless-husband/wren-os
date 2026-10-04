/* Event counters, readable from user space through SYS_kstat. */
#ifndef KERNEL_STATS_H
#define KERNEL_STATS_H
#include "types.h"

enum {
    STAT_COW_FAULTS, STAT_LAZY_FAULTS, STAT_DISK_READS, STAT_DISK_WRITES,
    STAT_LOG_COMMITS, STAT_CTX_SWITCHES, NSTATS
};
extern uint64_t kstats[NSTATS];

static inline void stat_inc(int s) { __atomic_fetch_add(&kstats[s], 1, __ATOMIC_RELAXED); }
static inline uint64_t stat_get(int s) { return __atomic_load_n(&kstats[s], __ATOMIC_RELAXED); }

#endif
