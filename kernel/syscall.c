#include "syscall.h"
#include "param.h"
#include "proc.h"
#include "vm.h"
#include <wren/errno.h>
#include <wren/fsformat.h>
#include <wren/syscall.h>

SYSCALL(fork); SYSCALL(exit); SYSCALL(waitpid); SYSCALL(pipe); SYSCALL(read); SYSCALL(write);
SYSCALL(close); SYSCALL(kill); SYSCALL(exec); SYSCALL(open); SYSCALL(mknod); SYSCALL(unlink);
SYSCALL(fstat); SYSCALL(link); SYSCALL(mkdir); SYSCALL(chdir); SYSCALL(dup); SYSCALL(dup2);
SYSCALL(getpid); SYSCALL(sbrk); SYSCALL(sleep); SYSCALL(uptime); SYSCALL(procinfo);
SYSCALL(poweroff); SYSCALL(getcpu); SYSCALL(lseek); SYSCALL(crashctl); SYSCALL(getppid); SYSCALL(kstat);

static const syscall_fn table[NSYSCALL] = {
    [SYS_fork] = sys_fork,         [SYS_exit] = sys_exit,         [SYS_waitpid] = sys_waitpid,
    [SYS_pipe] = sys_pipe,         [SYS_read] = sys_read,         [SYS_write] = sys_write,
    [SYS_close] = sys_close,       [SYS_kill] = sys_kill,         [SYS_exec] = sys_exec,
    [SYS_open] = sys_open,         [SYS_mknod] = sys_mknod,       [SYS_unlink] = sys_unlink,
    [SYS_fstat] = sys_fstat,       [SYS_link] = sys_link,         [SYS_mkdir] = sys_mkdir,
    [SYS_chdir] = sys_chdir,       [SYS_dup] = sys_dup,           [SYS_dup2] = sys_dup2,
    [SYS_getpid] = sys_getpid,     [SYS_sbrk] = sys_sbrk,         [SYS_sleep] = sys_sleep,
    [SYS_uptime] = sys_uptime,     [SYS_procinfo] = sys_procinfo, [SYS_poweroff] = sys_poweroff,
    [SYS_getcpu] = sys_getcpu,     [SYS_lseek] = sys_lseek,       [SYS_crashctl] = sys_crashctl,
    [SYS_getppid] = sys_getppid,   [SYS_kstat] = sys_kstat,
};

void syscall_dispatch(struct trapframe *tf)
{
    uint64_t n = tf->x[8];
    int64_t r = n < NSYSCALL && table[n] ? table[n](tf) : -ENOSYS;
    tf->x[0] = (uint64_t)r;
}

int fetch_path(struct trapframe *tf, int i, char *buf)
{
    int r = copyinstr(&myproc()->vm, buf, arg(tf, i), MAXPATH);
    if (r < 0) return r;
    /* Every component must fit a directory entry; we never truncate names. */
    int len = 0;
    for (const char *p = buf;; p++) {
        if (*p == '/' || *p == 0) {
            if (len > DIRSIZ) return -ENAMETOOLONG;
            len = 0;
            if (!*p) break;
        } else {
            len++;
        }
    }
    return 0;
}
