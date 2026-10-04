# Final project report: wren-os, a multicore Unix-like kernel for AArch64

*Written in the format of an operating-systems course final report (MIT 6.1810, Harvard CS 161,
Yale CPSC 422). This is an independent learning reimplementation; it is not course material and is
not affiliated with those courses.*

![Architecture](architecture.svg)

## 1. Goal

Build a small but complete kernel of the kind those courses end with, for 64-bit ARM instead of x86
or RISC-V, and show that it is correct with evidence a reviewer can rerun:

- boot on a multicore machine, turn on virtual memory, run user programs in their own address spaces;
- processes (`fork`, `exec`, `exit`, `wait`, `kill`), pipes and file descriptors, a shell;
- a disk file system that survives power cuts;
- one kernel binary for two different virtual machines (QEMU `virt` and LeapVM), which forces every
  device address to come from the device tree.

Every number below comes from a command in section 11, run on an Apple M5 (10 cores, 16 GB) with
macOS 27 that was shared with other jobs at the time.

## 2. Code tour

| File | Role |
|---|---|
| `kernel/boot.S` | Image header, EL2 to EL1, boot page tables, MMU on, secondary entry, TTBR1 switch trampoline |
| `kernel/entry.S`, `switch.S` | exception vectors and trap frame; `ctx_switch`; FP/SIMD save/restore |
| `kernel/main.c` | `kmain` (the boot sequence), `secondary_main` |
| `kernel/fdt.c`, `platform.c` | device tree parser; machine description |
| `kernel/buddy.c`, `kalloc.c` | physical pages with reference counts; `kmalloc` |
| `kernel/vm.c` | kernel tables, address spaces, ASIDs, COW, lazy faults, `copyin`/`copyout` |
| `kernel/trap.c`, `syscall.c`, `sysproc.c`, `sysfile.c`, `exec.c`, `elf.c` | traps and system calls, ELF loading |
| `kernel/proc.c`, `spinlock.c` | processes, scheduler, sleep/wakeup; ticket and sleep locks |
| `kernel/gic.c`, `psci.c`, `uart.c`, `console.c` | GICv3, timer, IPIs; PSCI; PL011; console line discipline |
| `kernel/virtio_blk.c`, `bio.c`, `log.c`, `fs.c`, `file.c`, `pipe.c` | storage stack, files, pipes |

In total about 5,700 lines of kernel C and assembly.

## 3. From power-on to the shell prompt

1. **The hypervisor** loads `build/Image` at a 2 MiB-aligned address plus `text_offset` (0): QEMU
   chooses `0x40200000`, LeapVM `0x40000000`. It writes a device tree into RAM and starts CPU 0 at
   the Image's first instruction with the MMU off and `x0` = the DTB's physical address.
2. **`primary_entry`** (`boot.S`) saves `x0`, drops to EL1 if it is at EL2, checks that the load
   address is 2 MiB aligned, and builds six pages of boot tables with the caches off: an identity map
   of the 1 GiB block holding the image, a linear map of physical `[0, 4 GiB)` at
   `0xffff000000000000` (block 0 as device memory, blocks 1-3 as normal memory), and the image at
   `0xffffffffc0000000` with 2 MiB blocks.
3. **`mmu_on`** programs `MAIR_EL1`, `TCR_EL1` (48-bit addresses in both halves, 4 KiB granule,
   physical address size from `ID_AA64MMFR0_EL1`), both TTBRs, enables FP/SIMD and turns on the MMU
   and caches. The next instruction still runs at the physical address (identity map); a jump through
   an absolute literal lands in the high half.
4. **`primary_high`** sets the stack and `VBAR_EL1`, zeroes `.bss`, records `kimage_voffset`, and
   calls `kmain(dtb, load_address)`.
5. **`kmain`** parses the DTB into `struct platform` and starts the UART at the address it found;
   gives all RAM except the image, the DTB and the page array to the buddy allocator; builds the real
   kernel tables (`kvm_init`) and switches to them with break-before-make (`kvm_install`); sets up the
   GIC distributor and CPU 0's redistributor; enables the UART interrupt; finds the virtio-blk device
   among the DTB's virtio slots; creates the first process from `initcode` (embedded in the image);
   starts the other CPUs with PSCI `CPU_ON` (each runs `secondary_entry`, the same `mmu_on`, then
   `kvm_install`, `gic_cpu_init`, `timer_cpu_init`); starts its own timer and enters the scheduler.
6. **The first process** returns from the scheduler into `fork_return`, which mounts the file system
   (this needs a process because it sleeps on disk I/O; recovery of the log happens here), sets the
   working directory, and drops to EL0 at `0x10000`.
7. **`initcode`** calls `exec("/bin/init")`. **`init`** opens `/dev/console` as file descriptors 0, 1
   and 2 and forks **`sh`**, which prints `$ `.

This takes 69 ms from starting the QEMU process (most of it QEMU itself) and 41 ms under LeapVM.

## 4. Memory layout

**Virtual (48-bit, 4 KiB pages, 4 levels):**

| Range | Contents | Attributes |
|---|---|---|
| `0x0000_0000_0001_0000` | program text | user R+X |
| next page boundaries | read-only data, then data and `.bss` | user R / user R+W, never executable |
| `heap_start .. brk` | heap, backed on first touch | user R+W |
| `0x3f_ff80_0000 .. 0x40_0000_0000` | stack, 8 MiB, backed on first touch | user R+W |
| `0xffff_0000_0000_0000 + pa` | linear map of RAM (2 MiB blocks where aligned) | kernel R+W, never executable |
| `0xffff_0000_0000_0000 + pa` | GIC, UART, RTC, virtio registers | Device-nGnRE |
| `0xffff_ffff_c000_0000` | kernel image: text / rodata / data and bss | R+X / R / R+W (4 KiB pages) |

Pages below `0x10000` (so null pointers) and between the heap and the stack are never mapped.

**Physical (QEMU, 256 MiB):** RAM at `0x40000000`; the image at `0x40200000` (372 KiB with `.bss`);
the `struct page` array (1 MiB for 256 MiB) right after it; the DTB at `0x48000000`; everything else
is managed by the buddy allocator (253 MiB free after boot). On LeapVM the image is at `0x40000000`
and the DTB at `0x40200000`.

## 5. The trap path

A system call, end to end (`getpid`, 66 ns round trip under HVF):

1. User code puts the number in `x8` and executes `svc #0`. The CPU switches to EL1, saves the
   return address in `ELR_EL1` and the processor state in `SPSR_EL1`, masks interrupts, and jumps to
   `VBAR_EL1 + 0x400` (synchronous exception from a lower level).
2. `el0_sync` (`entry.S`) pushes a 272-byte trap frame onto the process's kernel stack: `x0`-`x30`,
   `SP_EL0`, `ELR_EL1`, `SPSR_EL1`.
3. `user_sync_trap` (`trap.c`) reads `ESR_EL1`, re-enables interrupts, and for exception class `0x15`
   (SVC) calls `syscall_dispatch`, which indexes a table with `x8`, passes the trap frame (arguments
   are `x0`-`x5`) and stores the result in the frame's `x0`.
4. On the way out, `user_trap_exit` exits the process if it was killed and yields the CPU if the
   timer asked for it.
5. `user_return` masks interrupts, restores all registers from the frame and executes `eret`
   (followed by a speculation barrier).

A page fault takes the same path with exception class `0x24` (data abort) or `0x20` (instruction
abort); `vm_fault` either backs a lazy page, copies a COW page, or the process is killed with a
message naming the address and the access. An interrupt (`el0_irq`, `el1h_irq`) acknowledges the
GIC (`ICC_IAR1_EL1`), runs the handler registered for that interrupt number and signals the end
(`ICC_EOIR1_EL1`). A synchronous exception inside the kernel is always a bug: the kernel prints the
syndrome and a frame-pointer backtrace (the test harness turns it into function names) and stops
all CPUs.

## 6. Processes, scheduling and locking

- **Process table** of 64 entries, each with a 16 KiB kernel stack (a canary at its bottom is checked
  at every switch), a trap frame, saved callee-saved registers, saved FP/SIMD registers, an address
  space, 16 file descriptors and a working directory.
- **Scheduler:** each CPU runs a scheduler loop on its boot stack: take `proc_lock`, pop the head of
  the run queue, mark it running, load its page table and FP registers, `ctx_switch` to it. The
  process comes back to the loop through `sched()` when it sleeps, yields or exits.
- **Locking rule:** `proc_lock` is held across every switch, which serialises all scheduling state.
  `proc_sleep(chan, lk)` takes `proc_lock` before releasing `lk`, so a `proc_wakeup` on another CPU
  cannot slip in between the check and the sleep. Trade-offs: [DESIGN.md section 5](DESIGN.md#5-traps-scheduling-and-locking).
- **SMP:** CPU 0 starts the others with PSCI `CPU_ON` (the DTB's `psci` node says whether the
  conduit is `hvc` or `smc`). All device interrupts go to CPU 0; each CPU has its own timer
  interrupt. `ps` shows which CPU each process last ran on, and `usertests smp_spread` checks that
  CPU-bound processes really run on several CPUs.
- **Locks:** `proc` (process state), `pages` (buddy), `kheap`, `asid`, `ticks`, `ftable`, `icache`,
  `bcache`, `log`, `virtio-blk`, `console`, `console-out`, `printk`, one per pipe; one sleep lock per
  inode and per buffer.

## 7. The file system and its log

Layout, the commit protocol and the recovery rule are in [DESIGN.md section 6](DESIGN.md#6-file-system-and-the-log).
In short: each system call that modifies the disk is a transaction; its blocks go to the log, a
checksummed header commits it, then the blocks are written home; the system call returns after the
commit. At mount, a header whose checksum matches its data is replayed and any other header is
ignored. The disk image built by `make` is 64 MiB: 16,384 blocks, 1,024 inodes, 65 log blocks.

## 8. Verification

### 8.1 Strategy

| Layer | Tool | What it shows |
|---|---|---|
| Pure logic | 4 host C programs, UBSan (ASan too on Linux) | parsers and the allocator are right on normal input and safe on hostile input |
| One feature at a time | `usertests` (40 tests, in the guest) | each system call and each memory-management case behaves, including the failure cases |
| Interaction | `stress`, shell tests | many processes on many CPUs at once produce correct bytes, no panic, no deadlock, no leak |
| Durability | crash test, `fsck`, crafted-log recovery tests | the file system is consistent after any power cut and loses nothing that was acknowledged |
| Portability | boot matrix, LeapVM tests | nothing depends on one machine's addresses, its virtio version, or starting at EL1 |
| The tests themselves | mutation check | removing an important line makes a test fail |

### 8.2 Results

| Command | Result |
|---|---|
| `make unit` | test_lib 18, test_fdt 45, test_buddy 42,607, test_elf 72,434 checks; 0 failed |
| `make system` (pytest, 23 tests) | 23 passed in about a minute (including the 2 LeapVM tests on macOS; CI skips those with a reason) |
| `usertests` on 1 CPU / 4 CPUs | 40/40 and 40/40, about 2.5 s each under TCG |
| `make stress` (60 s, 4 CPUs, 12 workers) | `STRESS OK`, about 3,800 verified operations per worker, 853,868 context switches, 0 pages not returned, `fsck` clean |
| crash test, two batches of 300 | 600/600 consistent |
| `make mutants` | 8/8 caught |
| `make lint` | clean (extra warnings and the clang static analyzer, which found one real issue: section 10) |

### 8.3 The crash test

Workload: `fswork` performs 48 operations (600 in the SIGKILL runs) chosen by a seed over 24 file
names and 6 directories: create with content, append, rewrite the start, truncate, unlink, hard link,
mkdir, rmdir. Every write is at most 24 KiB, so each is one transaction.

Power cuts: *inject* (power off before disk write *N*, *N* uniform over the run's 698-768 writes),
*torn* (write *N* lands only 1-7 of its 8 sectors, then power off), *kill* (SIGKILL QEMU after a random
delay; the 600-operation workload takes about 0.45 s and does 6,100-6,900 writes).

Oracle: the host `fsck` (with its own log replay) and the kernel's recovery must both produce a
consistent file system, the same one, and it must be "all operations acknowledged with OK, plus a
prefix of the one in flight". For a `create` the allowed prefixes are: not there, created empty,
created with content (open and write are separate transactions).

| Power cut | Runs | Committed transaction replayed at mount | In-flight operation recovered as: none of it / first call / all of it |
|---|---:|---:|---|
| inject | 299 | 146 | 67 / 147 / 85 |
| torn | 141 | 68 | 21 / 73 / 47 |
| kill | 160 | 60 | 67 / 73 / 20 |
| **total** | **600** | **274** | 155 / 293 / 152 |

All 600 consistent, and the kernel's recovery and `fsck`'s matched in every run. The crash test never
produced a header that had to be *discarded* (the header fits in its block's first sector, so a torn
header write is either absent or complete); that path is covered separately by
`tests/test_fsck.py::test_kernel_recovery_of_crafted_log`, which writes a committed and a corrupt
transaction into the log by hand and checks that the kernel replays the first and discards the second.

### 8.4 Mutation check

| Mutant | Bug | Caught by |
|---|---|---|
| `no-tlb-flush-on-fork` | parent keeps stale writable TLB entries after fork | `usertests cow_isolation`: the parent's later writes reach the page the child shares |
| `no-page-allocator-lock` | buddy allocator called without its lock | `stress` on 4 CPUs: `PANIC: page_put: ... not an allocated page` |
| `no-log-commit-record` | the header that commits a transaction is never written | crash test: acknowledged operations missing or half-applied |
| `no-log-recovery` | mount ignores committed transactions | crash test: acknowledged operations lost |
| `no-cow-refcount` | fork shares pages without taking a reference | `usertests -q`: `PANIC: page_put ...` once the child exits |
| `no-fpu-save` | FP registers not saved on a switch | `usertests fp_state`: a worker sees corrupted FP results |
| `leak-indirect-block` | truncate forgets to free the indirect block | `usertests big_file`, then `fsck`: a block marked in use that nothing references |
| `no-pipe-reader-wakeup` | a reader does not wake a writer blocked on a full pipe | `usertests pipe_bulk` hangs; the harness times out |

Full output: [mutants.md](mutants.md).

### 8.5 LeapVM

`tests/test_leapvm.py` boots the same `build/Image` on LeapVM with 4 CPUs, 256 MiB and the disk image:
the kernel reports `booting on "LeapVM", image at 0x40000000`, finds the modern virtio transport at
`0x0d000000` and 4 CPUs; `usertests -q` passes (38 tests) and `stress 8` passes; after power-off the
disk passes `fsck`. A second test injects a power cut on LeapVM (`fswork 3 300`), boots the disk again
on LeapVM to recover it, and checks it with `fsck`. Both pass. LeapVM needed no changes. One
observation: LeapVM's exit statistics show no timer or `WFI` exits, because Hypervisor.framework
handles the virtual timer and the GIC itself; the guest's timer does run (`sleep` works).

## 9. Performance

Method: `user/bench.c` measures each operation in a loop with the guest's virtual counter
(`CNTVCT_EL0`, readable from EL0) on 1 guest CPU; `tests/bench.py` boots 5 times for the boot time
and prints a table. Full method and raw output: [benchmarks.md](benchmarks.md).

| Measurement | QEMU TCG | QEMU HVF | LeapVM |
|---|---:|---:|---:|
| Boot to prompt | 69 ms | 69 ms | 41 ms |
| `getpid` | 2,243 ns | 66 ns | 69 ns |
| context switch | 26.6 us | 355 ns | 346 ns |
| `fork`+`exit`+`wait` | 84 us | 5.3 us | 5.7 us |
| `fork`, 4 MiB touched heap | 194 us | 17.6 us | 15.8 us |
| `fork`+`exec`+`wait` | 379 us | 12.9 us | 11.1 us |
| lazy page fault | 3.7 us | 468 ns | 450 ns |
| COW page fault | 7.3 us | 858 ns | 840 ns |
| write 8 MiB (durable 32 KiB writes) | 10.5 MiB/s | 23.9 MiB/s | 10.8 MiB/s |
| read 8 MiB | 52.8 MiB/s | 111 MiB/s | 250 MiB/s |

Observations:

- Under Hypervisor.framework a system call never leaves the guest, so 66 ns is the real cost of `svc`,
  saving 34 registers, a table dispatch and `eret` on the M5. TCG is about 30 times slower; its
  numbers say more about QEMU than about the kernel.
- A context switch costs about 5 system calls: the pipe ping-pong does two switches per round trip,
  each going process, scheduler thread, process, plus the 528-byte FP save and restore.
- `fork` of a process with 4 MiB of touched heap costs 3.3 times a plain fork, not 1,024 page copies:
  only page-table entries are copied (and made read-only); a page is copied later, at 858 ns per COW
  fault, and only if it is written.
- File writes are bounded by the log: 6,152 disk writes for 2,048 data blocks (every block twice, plus
  two headers per 32 KiB transaction) and three flushes per transaction, each a host `fsync` for
  QEMU. Making every system call durable is the price; an explicit `fsync` with batched commits
  would raise throughput.

## 10. Challenges

- **Two loaders, one image.** QEMU and LeapVM load the kernel 2 MiB apart, and LeapVM puts the DTB
  right after the declared image size. Linking at a fixed virtual address with position-independent
  boot code solved the first; declaring `.bss` and the boot tables in `image_size` solved the second.
- **The timer interrupt on secondary CPUs.** A first version registered the timer handler on CPU 0
  after starting the other CPUs; a secondary's first tick would have found no handler, never re-armed
  the level-triggered timer, and stormed. Caught in review before it ran; the handler is now
  registered when the GIC is initialised.
- **The compiler removed the stack.** The stack-overflow test passed when it should not have: clang
  turned the deliberately deep recursion into a loop. An `asm` barrier that keeps each frame live
  fixed the test.
- **The crash test's own bugs.** The first 300-run batch reported 4 failures, all in SIGKILL runs and
  all with files "ahead" of the model. The cause was the harness, not the kernel: it read the console
  the moment QEMU died, before its reader thread had drained the pipe, so the last `OK` lines were
  missing. The next batch hit a second harness bug: a kill in the middle of an `OP` line. Both are
  fixed (drain the pipe; ignore an unterminated last line); 600 runs since then are all consistent.
- **Make 3.81.** A bare `.SECONDARY:` makes every target "intermediate", so missing files were not
  rebuilt; replaced with targeted `.PRECIOUS` rules.
- **The static analyzer** found that `sys_open` treated only negative results of `create` as errors;
  harmless today but fragile, so it now checks `!= 0`.
- **AddressSanitizer on macOS 27** hangs at start-up even for an empty program, so local unit tests
  use UBSan only; CI on Linux runs both.

## 11. Reproducing

```sh
make                                   # build
make unit                              # 8.2, row 1
make system                            # 8.2, row 2 (needs .venv with pytest, or PYTHON=python3)
make stress                            # 8.2, row 4
python3 tests/crash.py --runs 300                      # 8.3, seeds 1-300
python3 tests/crash.py --runs 300 --first-seed 1001    # 8.3, seeds 1001-1300
make mutants                           # 8.4
make bench ACCEL=tcg; make bench ACCEL=hvf; make bench HV=leapvm    # 9
```

## 12. Lessons

- Make every device address a lookup from the start. Hard-coding QEMU's UART "just for now" would
  have been the one line that made LeapVM fail.
- Write the checker before trusting the feature. `fsck` and the crash model found nothing wrong with
  the log, but they found two bugs in the test harness, and the mutation check shows they would have
  found a broken log.
- Choose the simple lock design, then measure. One scheduling lock is enough at this scale, and the
  reasoning about sleep/wakeup and exit/wait stays short.

## 13. Future work

- Per-CPU run queues with work stealing, and per-process locks.
- Signals and `mmap`; threads sharing an address space (which needs TLB shoot-down for an ASID that is
  live on several CPUs at once).
- `fsync` with batched commits instead of commit-on-return; installing committed blocks in the
  background.
- A crash test that reorders writes inside a flush interval, to exercise the checksum's second role.
- Reclaiming orphaned inodes at mount, and booting on a physical AArch64 board.
