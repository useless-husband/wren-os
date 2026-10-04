/* Compile-time limits. */
#ifndef KERNEL_PARAM_H
#define KERNEL_PARAM_H

#define NCPU          8      /* most CPUs we bring up */
#define NPROC        64      /* process table size */
#define NOFILE       16      /* open files per process */
#define NFILE       128      /* open files system-wide */
#define NINODE       96      /* in-memory inode cache */
#define NBUF        160      /* buffer cache blocks (4 KiB each) */
#define NDEV          4      /* device major numbers */
#define MAXARG       32      /* exec argv entries */
#define MAXPATH     128
#define KSTACK_PAGES  4      /* 16 KiB kernel stack per process */
#define HZ          100      /* timer interrupts per second */
#define PIPESIZE   4096

#define CONSOLE_MAJOR 1

#endif
