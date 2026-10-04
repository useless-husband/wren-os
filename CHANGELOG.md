# Changelog

## 0.1.0 - 2026-10-04

First complete version.

- Boot: arm64 Image header, EL2 to EL1, boot page tables with the MMU off, high-half kernel,
  device-tree discovery, break-before-make switch to the final tables.
- Memory: buddy page allocator with reference counts, per-process 4-level tables with ASIDs,
  copy-on-write fork, lazy heap and stack, W^X, kmalloc.
- Traps and SMP: exception vectors, system calls, page faults, GICv3, per-CPU virtual timer, IPIs,
  PSCI CPU_ON, ticket spinlocks with a deadlock detector, sleep locks, preemptive scheduler,
  FP/SIMD state switching.
- Processes: fork, exec (ELF), exit, waitpid, kill, sbrk, sleep, pipes, file descriptors, dup2, ps.
- Storage: virtio-blk (legacy and modern virtio-mmio), buffer cache, inode file system with double
  indirect blocks, write-ahead log with CRC-32C commit records and commit-before-return, host mkfs
  and fsck.
- User space: libc, sh (pipes, redirection, background jobs), ls cat echo grep wc mkdir rm ln kill
  ps sleep poweroff.
- Tests: host unit tests (FDT, buddy, ELF, lib) with sanitizers, 40 usertests, SMP stress, crash
  consistency with injected, torn and SIGKILL power cuts, kernel log recovery on crafted logs,
  LeapVM boot and crash tests, mutation check, benchmarks.
