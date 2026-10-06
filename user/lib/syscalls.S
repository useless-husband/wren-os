/* System call stubs: number in x8, arguments already in x0..x5. */
#include <wren/syscall.h>

.macro STUB name, num
    .text
    .globl \name
    .type \name, %function
\name:
    mov     x8, #\num
    svc     #0
    ret
.endm

STUB fork, SYS_fork
STUB _exit, SYS_exit
STUB waitpid, SYS_waitpid
STUB pipe, SYS_pipe
STUB read, SYS_read
STUB write, SYS_write
STUB close, SYS_close
STUB kill, SYS_kill
STUB exec, SYS_exec
STUB open, SYS_open
STUB mknod, SYS_mknod
STUB unlink, SYS_unlink
STUB fstat, SYS_fstat
STUB link, SYS_link
STUB mkdir, SYS_mkdir
STUB chdir, SYS_chdir
STUB dup, SYS_dup
STUB dup2, SYS_dup2
STUB getpid, SYS_getpid
STUB sbrk, SYS_sbrk
STUB sleep, SYS_sleep
STUB uptime, SYS_uptime
STUB procinfo, SYS_procinfo
STUB poweroff, SYS_poweroff
STUB getcpu, SYS_getcpu
STUB lseek, SYS_lseek
STUB crashctl, SYS_crashctl
STUB getppid, SYS_getppid
STUB kstat, SYS_kstat
STUB personality, SYS_personality
