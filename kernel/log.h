/* Write-ahead (redo) log: makes each file-system operation atomic and
 * durable across crashes.  Usage inside a system call:
 *     log_begin_op(); ... bread / modify / log_write(b) / brelse ...; log_end_op();
 * log_end_op() returns only after the transaction holding the operation is
 * on disk, so a successful system call is never lost by a crash. */
#ifndef KERNEL_LOG_H
#define KERNEL_LOG_H
#include "types.h"

struct buf;
struct superblock;

void log_init(uint32_t dev, const struct superblock *sb);   /* runs recovery */
void log_begin_op(void);
void log_end_op(void);
void log_write(struct buf *b);   /* instead of bwrite() for file-system blocks */

#endif
