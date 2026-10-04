#include "file.h"
#include "fs.h"
#include "log.h"
#include "printk.h"
#include "proc.h"
#include "spinlock.h"
#include <wren/errno.h>
#include <wren/fcntl.h>
#include <wren/stat.h>

struct devsw devsw[NDEV];
static struct {
    struct spinlock lock;
    struct file     file[NFILE];
} ftable;

void file_init(void) { spin_init(&ftable.lock, "ftable"); }

struct file *file_alloc(void)
{
    spin_lock(&ftable.lock);
    for (struct file *f = ftable.file; f < ftable.file + NFILE; f++)
        if (f->ref == 0) {
            f->ref = 1;
            spin_unlock(&ftable.lock);
            return f;
        }
    spin_unlock(&ftable.lock);
    return NULL;
}

struct file *file_dup(struct file *f)
{
    spin_lock(&ftable.lock);
    if (f->ref < 1) panic("file_dup: free file");
    f->ref++;
    spin_unlock(&ftable.lock);
    return f;
}

void file_close(struct file *f)
{
    spin_lock(&ftable.lock);
    if (f->ref < 1) panic("file_close: free file");
    if (--f->ref > 0) {
        spin_unlock(&ftable.lock);
        return;
    }
    struct file copy = *f;
    f->type = FD_NONE;
    f->ref = 0;
    spin_unlock(&ftable.lock);
    if (copy.type == FD_PIPE) {
        pipe_close(copy.pipe, copy.writable);
    } else if (copy.type == FD_INODE || copy.type == FD_DEVICE) {
        log_begin_op();
        iput(copy.ip);
        log_end_op();
    }
}

int file_stat(struct file *f, uint64_t ustat)
{
    if (f->type != FD_INODE && f->type != FD_DEVICE) return -EINVAL;
    struct stat st;
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    return copyout(&myproc()->vm, ustat, &st, sizeof st);
}

int file_read(struct file *f, uint64_t udst, int n)
{
    if (!f->readable) return -EBADF;
    if (n < 0) return -EINVAL;
    switch (f->type) {
    case FD_PIPE:
        return pipe_read(f->pipe, udst, n);
    case FD_DEVICE:
        if (f->major < 0 || f->major >= NDEV || !devsw[f->major].read) return -EINVAL;
        return devsw[f->major].read(udst, n);
    case FD_INODE: {
        ilock(f->ip);
        int r = readi(f->ip, true, udst, f->off, (uint32_t)n);
        if (r > 0) f->off += (uint32_t)r;
        iunlock(f->ip);
        return r;
    }
    default:
        panic("file_read: bad type");
    }
}

int file_write(struct file *f, uint64_t usrc, int n)
{
    if (!f->writable) return -EBADF;
    if (n < 0) return -EINVAL;
    switch (f->type) {
    case FD_PIPE:
        return pipe_write(f->pipe, usrc, n);
    case FD_DEVICE:
        if (f->major < 0 || f->major >= NDEV || !devsw[f->major].write) return -EINVAL;
        return devsw[f->major].write(usrc, n);
    case FD_INODE: {
        /* One transaction per MAXWRITE_TXN bytes: each chunk is atomic, a
         * larger write may be cut at a chunk boundary by a crash. */
        int done = 0;
        while (done < n) {
            int chunk = MIN(n - done, MAXWRITE_TXN);
            log_begin_op();
            ilock(f->ip);
            if (f->append) f->off = f->ip->size;
            int r = writei(f->ip, true, usrc + (uint64_t)done, f->off, (uint32_t)chunk);
            if (r > 0) f->off += (uint32_t)r;
            iunlock(f->ip);
            log_end_op();
            if (r < 0) return done ? done : r;
            done += r;
            if (r != chunk) break;
        }
        return done;
    }
    default:
        panic("file_write: bad type");
    }
}

int64_t file_seek(struct file *f, int64_t off, int whence)
{
    if (f->type != FD_INODE) return -ESPIPE;
    ilock(f->ip);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->off : whence == SEEK_END ? f->ip->size : -1;
    iunlock(f->ip);
    if (base < 0 || base + off < 0 || base + off > UINT32_MAX) return -EINVAL;
    f->off = (uint32_t)(base + off);
    return f->off;
}
