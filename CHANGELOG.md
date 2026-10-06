# Changelog

## Unreleased (branch feat/gc)

- User library: a conservative mark-sweep garbage collector (`gc_malloc`, `gc_collect`,
  `gc_get_stats`): roots from the callee-saved registers, the stack and `.data`/`.bss`; a lazily
  backed arena above 4 GiB with 16 size classes and page runs for large objects; side bitmaps;
  switchable interior-pointer recognition; mark-stack overflow recovery; threshold trigger.
- User library: `malloc` moved to `malloc.c` and records allocation sites; double frees are reported;
  `leak_check()` reports unreachable blocks with size and site. `leakcheck prog` runs any program
  with the report at exit.
- Kernel: `personality()` system call, a flags word kept across fork and exec (27 lines).
- Programs: `gcdemo`, `gctest`, `gcbench`, `leakdemo`, `leakcheck`.
- Tests: host model check of the collector core; `gctest` on 1 and 4 CPUs; bounded-heap, leak-report
  and inheritance system tests; GC tests on LeapVM; three new mutants (11/11 caught); `make gcbench`.
- Docs: docs/GC.md; README sections; a new chapter in the beginner's guide.

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
