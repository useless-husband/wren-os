# Benchmarks

## Method

- Hardware: Apple M5 (10 cores, 16 GB), macOS 27. The machine was shared with other jobs while these
  were taken (load average around 8), so treat differences under about 20 % as noise.
- Guest: 1 CPU, 256 MiB, the disk image built by `make` (a 64 MiB file).
- Configurations: QEMU 11.1 `virt` with TCG (pure emulation, what CI uses); QEMU with HVF (the guest
  runs on the real CPU through Apple's Hypervisor.framework); LeapVM (also Hypervisor.framework).
- Commands: `make bench ACCEL=tcg`, `make bench ACCEL=hvf`, `make bench HV=leapvm`
  (`tests/bench.py`, which runs `user/bench.c`).
- Timing: inside the guest, `CNTVCT_EL0` (the virtual counter, 62.5 MHz under QEMU, 24 MHz under
  Hypervisor.framework), read directly from user mode. Each micro-benchmark is a loop (200,000
  `getpid` calls, 20,000 pipe round trips, 300 forks, 100 fork+exec, 4,096 page faults, 8 MiB of
  file data) divided by its count. Boot time is wall-clock on the host from starting the hypervisor
  process to the first `$ ` prompt, median of 5 boots.

| Benchmark | What exactly is timed |
|---|---|
| `syscall_getpid` | one `getpid()` |
| `ctx_switch_via_pipe` | half of a 1-byte ping-pong between two processes over two pipes (one CPU, so every round trip is two context switches) |
| `fork_exit_wait` | `fork()`, child `exit(0)`, parent `waitpid()` |
| `fork_4MiB_heap` | the same with 1,024 touched heap pages in the parent (copy-on-write) |
| `fork_exec_wait` | `fork()`, child `exec("/bin/sleep", "0")`, parent `waitpid()` |
| `lazy_page_fault` | first write to one page of a fresh `sbrk` region |
| `cow_page_fault` | first write to one page shared with the parent |
| `fs_write_8MiB` | 256 `write()`s of 32 KiB to a new file, then `close()`; every write is committed when it returns |
| `fs_read_8MiB` | reading the file back in 32 KiB `read()`s (the buffer cache holds 640 KiB, so most of it comes from the disk) |
| `fs_create_write_unlink` | `open(O_CREATE)` + 100-byte `write` + `close`, 200 files, then 200 `unlink`s; per file |

## Results

| Measurement | QEMU TCG | QEMU HVF | LeapVM |
|---|---:|---:|---:|
| Boot to shell prompt (median of 5) | 69 ms | 69 ms | 41 ms |
| `syscall_getpid` | 2,243 ns | 66 ns | 69 ns |
| `pipe_roundtrip` | 53.1 us | 710 ns | 691 ns |
| `ctx_switch_via_pipe` | 26.6 us | 355 ns | 346 ns |
| `fork_exit_wait` | 83.9 us | 5.3 us | 5.7 us |
| `fork_4MiB_heap` | 194.2 us | 17.6 us | 15.8 us |
| `fork_exec_wait` | 378.8 us | 12.9 us | 11.1 us |
| `lazy_page_fault` | 3,720 ns | 468 ns | 450 ns |
| `cow_page_fault` | 7,287 ns | 858 ns | 840 ns |
| `fs_write_8MiB` | 10.5 MiB/s | 23.9 MiB/s | 10.8 MiB/s |
| `fs_write_disk_writes` | 6,152 | 6,152 | 6,152 |
| `fs_write_commits` | 257 | 257 | 257 |
| `fs_read_8MiB` | 52.8 MiB/s | 111.1 MiB/s | 250.3 MiB/s |
| `fs_create_write_unlink` | 4,549 us | 1,571 us | 2,378 us |

An earlier run on a less loaded machine gave, for QEMU TCG: `getpid` 1,411 ns, context switch 17.1 us,
`fork` 57.9 us; for QEMU HVF: 68 ns, 308 ns, 3.6 us; for LeapVM: 69 ns, 304 ns, 3.1 us. The
emulated (TCG) numbers move with host load much more than the virtualized ones.

## Reading the numbers

- **TCG vs. virtualization.** TCG translates every guest instruction, so it is 10-35 times slower on
  CPU-bound paths. It is the configuration CI can run, not a performance measurement of the kernel.
- **QEMU-HVF vs. LeapVM.** Both run guest code natively; system calls, page faults and context
  switches never leave the guest, so they agree within noise. They differ in the disk: each
  hypervisor implements virtio-blk and its flush in its own way, which shows in the file-system
  rows, and LeapVM starts faster.
- **Write amplification.** 2,048 data blocks cost 6,152 block writes: 2,048 to the log, 2,048 home,
  two header writes for each of the 257 transactions, plus bitmap, inode and indirect blocks. Each
  transaction also issues three flush requests. The design choice behind it (every system call is
  durable when it returns) is discussed in [DESIGN.md](DESIGN.md#6-file-system-and-the-log).
