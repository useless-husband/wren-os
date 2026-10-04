/* File-system system calls.  Each one that can modify the disk runs as a
 * single log transaction (log_begin_op ... log_end_op). */
#include "syscall.h"
#include "file.h"
#include "fs.h"
#include "log.h"
#include "printk.h"
#include "proc.h"
#include "vm.h"
#include <wren/errno.h>
#include <wren/fcntl.h>
#include <wren/stat.h>
#include "../lib/kstring.h"

static int fd_get(struct trapframe *tf, int i, struct file **f)
{
    int64_t fd = (int64_t)arg(tf, i);
    if (fd < 0 || fd >= NOFILE || !(*f = myproc()->ofile[fd])) return -EBADF;
    return (int)fd;
}

static int fd_alloc(struct file *f)
{
    struct proc *p = myproc();
    for (int fd = 0; fd < NOFILE; fd++)
        if (!p->ofile[fd]) {
            p->ofile[fd] = f;
            return fd;
        }
    return -EMFILE;
}

SYSCALL(dup)
{
    struct file *f;
    if (fd_get(tf, 0, &f) < 0) return -EBADF;
    int fd = fd_alloc(f);
    if (fd >= 0) file_dup(f);
    return fd;
}

SYSCALL(dup2)
{
    struct file *f;
    int old = fd_get(tf, 0, &f);
    int64_t nfd = (int64_t)arg(tf, 1);
    if (old < 0) return -EBADF;
    if (nfd < 0 || nfd >= NOFILE) return -EBADF;
    if (nfd == old) return nfd;
    struct proc *p = myproc();
    struct file *prev = p->ofile[nfd];
    p->ofile[nfd] = file_dup(f);
    if (prev) file_close(prev);
    return nfd;
}

SYSCALL(close)
{
    struct file *f;
    int fd = fd_get(tf, 0, &f);
    if (fd < 0) return fd;
    myproc()->ofile[fd] = NULL;
    file_close(f);
    return 0;
}

SYSCALL(read)
{
    struct file *f;
    if (fd_get(tf, 0, &f) < 0) return -EBADF;
    return file_read(f, arg(tf, 1), (int)MIN(arg(tf, 2), 0x7fffffffUL));
}

SYSCALL(write)
{
    struct file *f;
    if (fd_get(tf, 0, &f) < 0) return -EBADF;
    return file_write(f, arg(tf, 1), (int)MIN(arg(tf, 2), 0x7fffffffUL));
}

SYSCALL(fstat)
{
    struct file *f;
    if (fd_get(tf, 0, &f) < 0) return -EBADF;
    return file_stat(f, arg(tf, 1));
}

SYSCALL(lseek)
{
    struct file *f;
    if (fd_get(tf, 0, &f) < 0) return -EBADF;
    return file_seek(f, (int64_t)arg(tf, 1), (int)arg(tf, 2));
}

SYSCALL(pipe)
{
    struct file *rf, *wf;
    int r = pipe_alloc(&rf, &wf);
    if (r < 0) return r;
    int fds[2] = {fd_alloc(rf), -1};
    if (fds[0] >= 0) fds[1] = fd_alloc(wf);
    if (fds[1] < 0 || copyout(&myproc()->vm, arg(tf, 0), fds, sizeof fds) < 0) {
        int err = fds[1] < 0 ? -EMFILE : -EFAULT;
        if (fds[0] >= 0) myproc()->ofile[fds[0]] = NULL;
        if (fds[1] >= 0) myproc()->ofile[fds[1]] = NULL;
        file_close(rf);
        file_close(wf);
        return err;
    }
    return 0;
}

/* Create (or, for plain files, open) path.  On success *out is locked. */
static int create(const char *path, uint16_t type, uint16_t major, struct inode **out)
{
    char name[DIRSIZ + 1];
    struct inode *dp = nameiparent(path, name);
    if (!dp) return -ENOENT;
    ilock(dp);
    if (dp->nlink == 0) {          /* removed directory, still someone's cwd */
        iunlockput(dp);
        return -ENOENT;
    }
    struct inode *ip = dirlookup(dp, name, NULL);
    if (ip) {
        iunlockput(dp);
        ilock(ip);
        if (type == DI_FILE && (ip->type == DI_FILE || ip->type == DI_DEVICE)) {
            *out = ip;
            return 0;
        }
        iunlockput(ip);
        return -EEXIST;
    }
    if (!(ip = ialloc(dp->dev, type))) {
        iunlockput(dp);
        return -ENOSPC;
    }
    ilock(ip);
    ip->major = major;
    ip->nlink = type == DI_DIR ? 2 : 1;     /* nlink = directory entries naming it, incl. "." */
    iupdate(ip);
    int r = 0;
    if (type == DI_DIR) {
        r = dirlink(ip, ".", ip->inum);
        if (!r) r = dirlink(ip, "..", dp->inum);
    }
    if (!r) r = dirlink(dp, name, ip->inum);
    if (r) {                                   /* undo: the inode is freed by iput */
        ip->nlink = 0;
        iupdate(ip);
        iunlockput(ip);
        iunlockput(dp);
        return r;
    }
    if (type == DI_DIR) {                      /* the child's ".." names the parent */
        dp->nlink++;
        iupdate(dp);
    }
    iunlockput(dp);
    *out = ip;
    return 0;
}

SYSCALL(open)
{
    char path[MAXPATH];
    int r = fetch_path(tf, 0, path);
    if (r < 0) return r;
    int flags = (int)arg(tf, 1);
    int acc = flags & 3;
    if (acc == 3) return -EINVAL;
    struct inode *ip;
    log_begin_op();
    if (flags & O_CREATE) {
        r = create(path, DI_FILE, 0, &ip);
        if (r != 0) {
            log_end_op();
            return r;
        }
    } else {
        if (!(ip = namei(path))) {
            log_end_op();
            return -ENOENT;
        }
        ilock(ip);
        if (ip->type == DI_DIR && acc != O_RDONLY) {
            iunlockput(ip);
            log_end_op();
            return -EISDIR;
        }
    }
    if (ip->type == DI_DEVICE && ip->major >= NDEV) {
        iunlockput(ip);
        log_end_op();
        return -ENXIO;
    }
    struct file *f = file_alloc();
    int fd = f ? fd_alloc(f) : -ENFILE;
    if (fd < 0) {
        if (f) file_close(f);
        iunlockput(ip);
        log_end_op();
        return fd;
    }
    if ((flags & O_TRUNC) && ip->type == DI_FILE && acc != O_RDONLY) itrunc(ip);
    f->type = ip->type == DI_DEVICE ? FD_DEVICE : FD_INODE;
    f->major = ip->major;
    f->ip = ip;
    f->off = 0;
    f->readable = acc != O_WRONLY;
    f->writable = acc != O_RDONLY;
    f->append = (flags & O_APPEND) != 0;
    iunlock(ip);
    log_end_op();
    return fd;
}

SYSCALL(mkdir)
{
    char path[MAXPATH];
    int r = fetch_path(tf, 0, path);
    if (r < 0) return r;
    struct inode *ip;
    log_begin_op();
    r = create(path, DI_DIR, 0, &ip);
    if (r == 0) iunlockput(ip);
    log_end_op();
    return r;
}

SYSCALL(mknod)
{
    char path[MAXPATH];
    int r = fetch_path(tf, 0, path);
    if (r < 0) return r;
    if (arg(tf, 1) >= NDEV) return -EINVAL;
    struct inode *ip;
    log_begin_op();
    r = create(path, DI_DEVICE, (uint16_t)arg(tf, 1), &ip);
    if (r == 0) iunlockput(ip);
    log_end_op();
    return r;
}

SYSCALL(chdir)
{
    char path[MAXPATH];
    int r = fetch_path(tf, 0, path);
    if (r < 0) return r;
    struct proc *p = myproc();
    log_begin_op();
    struct inode *ip = namei(path);
    if (!ip) {
        log_end_op();
        return -ENOENT;
    }
    ilock(ip);
    if (ip->type != DI_DIR) {
        iunlockput(ip);
        log_end_op();
        return -ENOTDIR;
    }
    iunlock(ip);
    iput(p->cwd);
    log_end_op();
    p->cwd = ip;
    return 0;
}

SYSCALL(link)
{
    char old[MAXPATH], new[MAXPATH], name[DIRSIZ + 1];
    int r = fetch_path(tf, 0, old);
    if (r == 0) r = fetch_path(tf, 1, new);
    if (r < 0) return r;
    log_begin_op();
    struct inode *ip = namei(old);
    if (!ip) {
        log_end_op();
        return -ENOENT;
    }
    ilock(ip);
    if (ip->type == DI_DIR) {
        iunlockput(ip);
        log_end_op();
        return -EPERM;
    }
    ip->nlink++;
    iupdate(ip);
    iunlock(ip);
    struct inode *dp = nameiparent(new, name);
    r = -ENOENT;
    if (dp) {
        ilock(dp);
        r = dp->dev != ip->dev ? -EXDEV : dirlink(dp, name, ip->inum);
        iunlockput(dp);
    }
    if (r < 0) {
        ilock(ip);
        ip->nlink--;
        iupdate(ip);
        iunlock(ip);
    }
    iput(ip);
    log_end_op();
    return r;
}

SYSCALL(unlink)
{
    char path[MAXPATH], name[DIRSIZ + 1];
    int r = fetch_path(tf, 0, path);
    if (r < 0) return r;
    log_begin_op();
    struct inode *dp = nameiparent(path, name);
    if (!dp) {
        log_end_op();
        return -ENOENT;
    }
    ilock(dp);
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        iunlockput(dp);
        log_end_op();
        return -EINVAL;
    }
    uint32_t off;
    struct inode *ip = dirlookup(dp, name, &off);
    if (!ip) {
        iunlockput(dp);
        log_end_op();
        return -ENOENT;
    }
    ilock(ip);
    if (ip->nlink < 1) panic("unlink: inode %u has no links", ip->inum);
    if (ip->type == DI_DIR && !dir_empty(ip)) {
        iunlockput(ip);
        iunlockput(dp);
        log_end_op();
        return -ENOTEMPTY;
    }
    r = dir_unlink(dp, off);
    if (r == 0 && ip->type == DI_DIR) {
        dp->nlink--;            /* the child's ".." is gone */
        iupdate(dp);
    }
    iunlockput(dp);
    if (r == 0) {
        ip->nlink -= ip->type == DI_DIR ? 2 : 1;   /* a directory also loses its "." */
        if (ip->type == DI_DIR && ip->nlink != 0) panic("unlink: directory link count");
        iupdate(ip);
    }
    iunlockput(ip);
    log_end_op();
    return r;
}
