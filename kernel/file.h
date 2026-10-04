/* Open files: what a file descriptor points to. */
#ifndef KERNEL_FILE_H
#define KERNEL_FILE_H
#include "types.h"
#include "param.h"

struct inode;
struct pipe;

struct file {
    enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
    int           ref;
    bool          readable, writable, append;
    struct pipe  *pipe;
    struct inode *ip;
    uint32_t      off;       /* FD_INODE */
    int           major;     /* FD_DEVICE */
};

/* Character devices, by major number. */
struct devsw {
    int (*read)(uint64_t udst, int n);
    int (*write)(uint64_t usrc, int n);
};
extern struct devsw devsw[NDEV];

void         file_init(void);
struct file *file_alloc(void);
struct file *file_dup(struct file *f);
void         file_close(struct file *f);
int          file_stat(struct file *f, uint64_t ustat);
int          file_read(struct file *f, uint64_t udst, int n);
int          file_write(struct file *f, uint64_t usrc, int n);
int64_t      file_seek(struct file *f, int64_t off, int whence);

int  pipe_alloc(struct file **rf, struct file **wf);
void pipe_close(struct pipe *pi, bool writer);
int  pipe_read(struct pipe *pi, uint64_t udst, int n);
int  pipe_write(struct pipe *pi, uint64_t usrc, int n);

#endif
