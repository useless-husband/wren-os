/* System call numbers: the ABI between user programs and the kernel.
 * Calling convention: number in x8, arguments in x0..x5, `svc #0`,
 * result in x0 (negative values are errors). */
#ifndef WREN_SYSCALL_H
#define WREN_SYSCALL_H

#define SYS_fork      1
#define SYS_exit      2
#define SYS_waitpid   3
#define SYS_pipe      4
#define SYS_read      5
#define SYS_write     6
#define SYS_close     7
#define SYS_kill      8
#define SYS_exec      9
#define SYS_open     10
#define SYS_mknod    11
#define SYS_unlink   12
#define SYS_fstat    13
#define SYS_link     14
#define SYS_mkdir    15
#define SYS_chdir    16
#define SYS_dup      17
#define SYS_dup2     18
#define SYS_getpid   19
#define SYS_sbrk     20
#define SYS_sleep    21   /* milliseconds */
#define SYS_uptime   22   /* milliseconds since boot */
#define SYS_procinfo 23
#define SYS_poweroff 24
#define SYS_getcpu   25
#define SYS_lseek    26
#define SYS_crashctl 27   /* test-only fault injection, see kernel/virtio_blk.c */
#define SYS_getppid  28
#define SYS_kstat    29   /* kernel counters for tests and benchmarks */
#define SYS_personality 30 /* per-process flags kept across fork and exec */

#define NSYSCALL     31

/* personality() flags.  Like Linux personality(2) (used by setarch), the
 * word is inherited by fork and survives exec, so a launcher can set it
 * and exec the real program.  PER_QUERY reads it without changing it. */
#define PER_LEAKCHECK 0x1u   /* the user library reports leaked malloc blocks at exit */
#define PER_MASK      0x1u
#define PER_QUERY     0xffffffffu

#endif
