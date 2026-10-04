#include "file.h"
#include "kalloc.h"
#include "proc.h"
#include "spinlock.h"
#include <wren/errno.h>

struct pipe {
    struct spinlock lock;
    uint32_t nread, nwrite;     /* free-running byte counters */
    bool     readopen, writeopen;
    char     data[PIPESIZE];
};

int pipe_alloc(struct file **rf, struct file **wf)
{
    struct pipe *pi = kmalloc(sizeof *pi);
    *rf = file_alloc();
    *wf = *rf ? file_alloc() : NULL;
    if (!pi || !*wf) {
        if (pi) kfree(pi);
        if (*rf) file_close(*rf);
        return -ENFILE;
    }
    spin_init(&pi->lock, "pipe");
    pi->readopen = pi->writeopen = true;
    **rf = (struct file){.type = FD_PIPE, .ref = 1, .readable = true, .pipe = pi};
    **wf = (struct file){.type = FD_PIPE, .ref = 1, .writable = true, .pipe = pi};
    return 0;
}

void pipe_close(struct pipe *pi, bool writer)
{
    spin_lock(&pi->lock);
    if (writer) {
        pi->writeopen = false;
        proc_wakeup(&pi->nread);
    } else {
        pi->readopen = false;
        proc_wakeup(&pi->nwrite);
    }
    bool last = !pi->readopen && !pi->writeopen;
    spin_unlock(&pi->lock);
    if (last) kfree(pi);
}

int pipe_write(struct pipe *pi, uint64_t usrc, int n)
{
    struct proc *p = myproc();
    int done = 0;
    spin_lock(&pi->lock);
    while (done < n) {
        if (!pi->readopen) {
            spin_unlock(&pi->lock);
            return done ? done : -EPIPE;
        }
        if (proc_killed(p)) {
            spin_unlock(&pi->lock);
            return -EINTR;
        }
        uint32_t space = PIPESIZE - (pi->nwrite - pi->nread);
        if (space == 0) {
            proc_wakeup(&pi->nread);
            proc_sleep(&pi->nwrite, &pi->lock);
            continue;
        }
        /* Copy straight into the ring, at most up to its end. */
        uint32_t at = pi->nwrite % PIPESIZE;
        uint32_t m = MIN(MIN(space, (uint32_t)(n - done)), PIPESIZE - at);
        if (copyin(&p->vm, pi->data + at, usrc + (uint64_t)done, m) < 0) {
            spin_unlock(&pi->lock);
            return done ? done : -EFAULT;
        }
        pi->nwrite += m;
        done += (int)m;
    }
    proc_wakeup(&pi->nread);
    spin_unlock(&pi->lock);
    return done;
}

int pipe_read(struct pipe *pi, uint64_t udst, int n)
{
    struct proc *p = myproc();
    spin_lock(&pi->lock);
    while (pi->nread == pi->nwrite && pi->writeopen) {
        if (proc_killed(p)) {
            spin_unlock(&pi->lock);
            return -EINTR;
        }
        proc_sleep(&pi->nread, &pi->lock);
    }
    int done = 0;
    while (done < n && pi->nread != pi->nwrite) {
        uint32_t at = pi->nread % PIPESIZE;
        uint32_t m = MIN(MIN(pi->nwrite - pi->nread, (uint32_t)(n - done)), PIPESIZE - at);
        if (copyout(&p->vm, udst + (uint64_t)done, pi->data + at, m) < 0) {
            if (!done) done = -EFAULT;
            break;
        }
        pi->nread += m;
        done += (int)m;
    }
    proc_wakeup(&pi->nwrite);
    spin_unlock(&pi->lock);
    return done;
}
