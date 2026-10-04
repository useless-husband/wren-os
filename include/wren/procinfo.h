#ifndef WREN_PROCINFO_H
#define WREN_PROCINFO_H
#include <stdint.h>

/* One row of `ps`, filled in by SYS_procinfo. */
struct procinfo {
    int32_t  pid;
    int32_t  ppid;
    int32_t  state;      /* PS_* below */
    int32_t  cpu;        /* CPU it is running on, or last ran on */
    uint64_t mem;        /* bytes of user memory actually mapped */
    uint64_t ticks;      /* timer ticks spent running */
    char     name[16];
};

#define PS_RUNNABLE 1
#define PS_RUNNING  2
#define PS_SLEEPING 3
#define PS_ZOMBIE   4

/* SYS_kstat selectors */
#define KSTAT_FREE_PAGES   1
#define KSTAT_TOTAL_PAGES  2
#define KSTAT_NCPU         3
#define KSTAT_DISK_WRITES  4
#define KSTAT_DISK_READS   5
#define KSTAT_LOG_COMMITS  6
#define KSTAT_CTX_SWITCHES 7
#define KSTAT_COW_FAULTS   8
#define KSTAT_LAZY_FAULTS  9
#define KSTAT_TIMER_HZ    10

/* SYS_crashctl operations (fault injection for the crash-consistency tests) */
#define CRASH_DISARM 0
#define CRASH_AFTER  1   /* power off right before the Nth disk write from now */
#define CRASH_TORN   2   /* same, but the Nth write is torn: only `arg2` sectors land */

#endif
