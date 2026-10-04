# Design notes

This file records the decisions behind wren-os: what the hard problems were, what was chosen, and
what was considered and rejected. The narrative tour (boot sequence, memory map, trap path, results)
is in [report.md](report.md).

## 1. Constraints

- One kernel `Image` must boot unmodified on QEMU `virt` and on LeapVM. They differ in load address
  (`0x40200000` vs `0x40000000`), device addresses (UART `0x09000000` vs `0x0c000000`, virtio from
  `0x0a000000` vs `0x0d000000`, redistributors `0x080a0000` vs `0x0a000000`), and virtio transport
  (QEMU defaults to the legacy version 1 interface; LeapVM implements only version 2).
- Tests must run in CI on x86-64 Linux, where QEMU emulates the CPU (TCG) and is several times
  slower than on the development machine.
- Teaching scale: a reader should be able to hold each subsystem in their head.

## 2. Boot and address space

**Problem: position.** The loader picks the physical address. Options:

1. Link at a physical address and require loaders to use it. Rejected: QEMU and LeapVM already
   disagree, and the arm64 boot protocol only promises a 2 MiB aligned base.
2. Compile position-independent and relocate at run time. Rejected: a PIE kernel needs a relocation
   pass before any C runs.
3. **Chosen:** link the kernel at a fixed virtual address, `KIMAGE_VBASE = 0xffffffffc0000000`, and
   map it there from wherever it was loaded (this is what Linux does with `kimage_voffset`). The code
   that runs before the MMU is on (`boot.S`) only uses PC-relative addressing (`adr`, `adrp`), so it
   works at any physical address. `V2P()` knows two regions: image addresses (subtract
   `kimage_voffset`) and the linear map (subtract `LINEAR_BASE`).

The header's `image_size` covers `.bss` and the boot page tables. This matters on LeapVM, which puts
the device tree directly after `image_size`: a smaller value would place the DTB inside `.bss`,
which the kernel zeroes on entry.

**Two halves.** The kernel lives in TTBR1 (top of the address space): a linear map of all RAM at
`0xffff000000000000 + pa`, device registers in the same map with Device-nGnRE attributes, and the
image at `KIMAGE_VBASE`. Each process has its own TTBR0 table tagged with an 8-bit ASID, so a context
switch is one register write and needs no TLB flush. Rejected: xv6's approach of mapping the kernel
into every process's table; on AArch64 the split TTBR0/TTBR1 makes that unnecessary.

**Boot page tables, then the real ones.** `boot.S` builds six pages of tables with 1 GiB and 2 MiB
blocks (identity map for the instruction that enables the MMU, a linear map of the first 4 GiB, the
image) with caches off, invalidating the cache lines before and after writing them so no stale line
can shadow them. `kvm_init()` then builds exact tables from the device tree with 4 KiB pages for the
image (text RX, rodata RO, data RW+XN: W^X in the kernel too). Switching TTBR1 while running on it
is done with break-before-make: a trampoline that runs from the identity map points TTBR1 at an
empty table, invalidates the TLB, then installs the new table, so the TLB never holds a 2 MiB and a
4 KiB entry for the same address (a TLB conflict).

**EL2.** If entered at EL2 (QEMU with `virtualization=on`), `drop_to_el1` gives EL1 the timers and
the GIC system registers and returns to EL1 with `eret`. Tested in CI.

**Caches with the MMU off.** On QEMU's TCG there are no caches to get wrong; under
Hypervisor.framework (QEMU-HVF, LeapVM) there are. Secondary CPUs read nothing that CPU 0 wrote with
caches on: their logical id arrives in the PSCI `context` register, and their stacks are computed
from it.

## 3. Device discovery

`fdt.c` makes one validating pass over the DTB: every offset, length and string is checked against
the header's block boundaries, so a corrupt blob yields an error code, never an out-of-bounds read.
It records nodes and properties in flat arrays; queries are linear scans (the QEMU `virt` tree has 63
nodes). `platform.c` turns that into a `struct platform`: memory, CPUs (MPIDR values), PSCI method,
GICv3 regions, the virtual timer's PPI, the UART (from `/chosen/stdout-path`), the RTC and every
virtio-mmio slot. Interrupt specifiers are decoded with the GIC's 3-cell binding.

The parser and the platform code have no kernel dependencies and are compiled into host unit tests,
which parse a tree written to match LeapVM's `fdt.c` byte for byte, the tree QEMU itself generates
(`-machine dumpdtb`, created at test time), and 20,000 randomly corrupted blobs under UBSan/ASan.

## 4. Memory

**Physical pages: a buddy allocator** (orders 0 to 10, i.e. 4 KiB to 4 MiB) with a 16-byte
`struct page` per frame holding the free-list links, the order, the state (free head / allocated
head / tail / reserved), a reference count, and the kmalloc size class. Multi-page allocations are
needed for kernel stacks (16 KiB) and virtqueues (8 KiB, physically contiguous); a plain free list
cannot provide them. The allocator core has no locks or kernel dependencies so it can be
model-checked on the host: 40,000 random allocations and frees with an overlap check and the
allocator's own invariant checker (`buddy_check`) after every 97 steps.

**Copy-on-write fork.** `vm_clone_cow` shares every page: writable ones become read-only with a
software bit (`PTE_COW`, bit 55, ignored by the hardware) in both parent and child, and their
reference count goes up. A write fault on a COW page copies it, or, if the reference count is 1,
simply makes it writable again. The parent's TLB must be flushed after its entries become read-only
(`tlbi aside1is`), otherwise it keeps writing through stale writable entries into the page its child
now shares; this is one of the mutation tests and the COW isolation test catches its removal.
Changing a live mapping to a new physical page uses break-before-make: invalid, TLB invalidate, new
entry.

**Lazy allocation.** `sbrk` only moves the break; the heap `[heap_start, brk)` and the stack region
(8 MiB below `USER_TOP`) are backed on first touch. Below the stack region nothing is mapped, so
runaway recursion faults instead of overwriting the heap.

**Kernel access to user memory.** The kernel never dereferences a user pointer. `copyin`,
`copyout` and `copyinstr` walk the process's table in software, fault pages in through the same
`vm_fault` the trap handler uses, and copy through the linear map. A bad pointer becomes `-EFAULT`.
Rejected: letting the kernel touch user addresses directly and recovering from faults with an
exception table. It is faster, but it needs fault fixups in the kernel trap handler and PAN
handling, and a mistake turns into a kernel crash.

**TLB maintenance on SMP.** All user TLB invalidations use the inner-shareable broadcast forms
(`tlbi vae1is`, `tlbi aside1is`), so there are no shoot-down IPIs. When a process exits, its CPU
first leaves its tables (TTBR0 points at an empty table), then frees them, then invalidates the ASID
before it can be reused. The scheduler also leaves a process's tables whenever it switches away, so
no CPU's TTBR0 ever points at freed memory.

**FP/SIMD.** The kernel is compiled with `-mgeneral-regs-only`, so a process's FP registers are
untouched between its trap into the kernel and the next context switch. They are saved in `sched()`
and restored by the scheduler before switching to the next process: 528 bytes per switch, nothing
per system call. Rejected: lazy FP switching with a trap on first use, which saves work but needs
per-CPU ownership tracking that is easy to get wrong on SMP.

## 5. Traps, scheduling and locking

The vector table saves all 31 general registers, SP_EL0, ELR and SPSR (272 bytes) on the current
kernel stack. For a process in user mode SP_EL1 sits at the top of its own 16 KiB kernel stack, so
its trap frame is always at a fixed place, and `fork` builds the child's trap frame there.

**Scheduler.** One scheduler thread per CPU (on that CPU's boot stack), one FIFO run queue, 100 Hz
tick on every CPU. A process gives up the CPU through `sched()`, which switches to its CPU's
scheduler thread; the scheduler picks the next runnable process. A dying process can therefore be
switched away from its own stack, and its parent frees the stack later. An idle CPU waits in `WFI`
with interrupts masked (a pending interrupt still wakes it), and `make_runnable` sends one idle CPU
an SGI so new work does not wait for the next tick.

**One lock for process state.** `proc_lock` protects every process's state, wait channel, parent,
exit status and `killed` flag, the run queue and pid allocation. It is held across `ctx_switch`:
whoever switches away holds it, whoever resumes releases it. That one rule closes the classic races:
a wakeup cannot be lost between "check condition" and "go to sleep" (the sleeper takes `proc_lock`
before releasing its condition lock, and `wakeup` needs `proc_lock`), and an exiting process cannot
be reaped while still running on its kernel stack. Rejected: per-process locks (as in xv6-riscv)
and per-CPU run queues with work stealing (as in Chickadee and Linux). Both scale better, but the
lock ordering between a process lock, a run-queue lock and the waker's own lock is subtle, and at 4
to 8 CPUs the measured context switch is already dominated by the trap path, not by lock contention.

**Spinlocks** are ticket locks (`fetch_add` a ticket, wait for `serving`), fair under contention.
Holding one disables interrupts on that CPU (nested with `irq_push`/`irq_pop`), so an interrupt
handler can never spin on a lock its own CPU holds. A waiter that spins for 20 seconds panics with
the lock's name and holder instead of hanging: the stress tests would otherwise just time out with no
clue. Lock order: anything, then `proc_lock`, then `pages`/`kheap`.

**Preemption** happens on the way back to user mode (timer interrupt or system call). The kernel
itself is not preempted: a system call runs until it sleeps or returns. That keeps kernel code free
of "could I be moved to another CPU here" reasoning; every kernel path is short or sleeps on I/O.

## 6. File system and the log

**Layout** (`include/wren/fsformat.h`, shared by kernel, `mkfs` and `fsck`): 4 KiB blocks; block 1 is
the superblock; then the log (1 header block + 64 data blocks), the inode table (64-byte inodes),
the free-block bitmap, data. Inodes have 11 direct pointers, one indirect and one double indirect
block (largest file about 4 GiB). Directory entries are 32 bytes: a 4-byte inode number and a 28-byte
name. A link count is the number of directory entries naming the inode, including a directory's own
`.` and its children's `..`, which is exactly what `fsck` recounts.

**Transactions.** Every system call that changes the disk is one transaction (`log_begin_op` /
`log_end_op`); concurrent system calls join the same transaction (group commit), and
`log_begin_op` waits if the log could overflow, reserving 16 blocks per operation. A `write` is cut
into 32 KiB transactions so that each fits (9 data blocks, the inode, 2 bitmap blocks, 3 indirect
blocks). `log_write` replaces `bwrite` inside a transaction: it records the block number (absorbing
repeated writes of the same block) and pins the buffer so it cannot be evicted before commit.

**Commit protocol** (the last operation to end commits):

1. write each dirty block to its slot in the log area; flush;
2. write the header: number of blocks, sequence number, block numbers, and a CRC-32C over the header
   fields and all logged data; flush. This single-block write is the commit point;
3. write each block to its home location; flush;
4. write a header with `n = 0`.

**Recovery** at mount reads the header; if `n > 0`, the block numbers are inside the file system and
the CRC matches the logged data, it redoes step 3; otherwise it discards the transaction. Then it
clears the header.

**Why a checksum.** xv6 trusts the header because it writes data, then header, in order. The CRC
makes two more situations safe: a device that reorders or caches writes (a header can never be
taken as committed unless its data is there too), and step 4 not reaching the disk before the next
transaction starts overwriting the log area (the stale header no longer matches and is ignored), so
step 4 needs no flush of its own. The host `fsck` implements the same rule independently, and the
crash test checks that the kernel's recovery and `fsck`'s produce the same state.

**Durable on return.** In xv6, an operation that ends while others are still running returns before
its transaction commits, so a successful system call can still be lost by a crash. Here
`log_end_op` sleeps until the transaction containing the operation has committed. That gives a
simple, testable contract (the crash test's "everything that printed OK must survive") at the cost of
latency for concurrent writers.

**Buffer cache.** 160 buffers of one page each (physically contiguous, so a buffer is a single DMA
segment), an LRU list and a sleep lock per buffer. A buffer pinned by the log has a non-zero
reference count and is never recycled.

**virtio-blk.** One virtqueue of 64 descriptors, three-descriptor requests (header, data, status), any
number in flight; the submitting process sleeps until the interrupt handler marks its request done.
Legacy (version 1): the queue is one contiguous area announced through `QueuePFN` after writing
`GuestPageSize`. Modern (version 2): three separate addresses, `QueueReady`, `FEATURES_OK` and
`VIRTIO_F_VERSION_1`. The driver negotiates `VIRTIO_BLK_F_FLUSH` and issues real flush requests.

**Crash injection** (`SYS_crashctl`): the next *N*-th disk write powers the machine off through PSCI
`SYSTEM_OFF` before it is submitted, or submits only its first *k* sectors and then powers off (a
torn write). It is a test hook, documented as such; normal programs never call it.

## 7. Testing strategy

- **Host unit tests** for code that is pure logic: FDT parser, platform discovery, buddy allocator,
  ELF validation, CRC-32C, the formatter. Randomised parts use fixed, printed seeds.
- **In-guest tests** (`usertests`): each test runs in its own child, so a crash or a hang in one
  cannot hide the others; a failure is either an explicit check or the kernel killing the child.
- **System tests** in Python drive QEMU or LeapVM over the serial console with an expect-style
  harness; a kernel panic anywhere fails the test and its backtrace is symbolised with
  `llvm-addr2line`.
- **Crash consistency** with a model of what must survive (section 6 and `tests/crashmodel.py`).
- **Mutation check**: deliberate bugs that the tests must catch, so the tests are known to have
  teeth.

## 8. Things deliberately left out

Signals, threads, `mmap`, sockets, permissions, `rename`, symbolic links, timestamps, a network
driver, kernel preemption, per-CPU run queues, returning slab pages, RAM above 4 GiB. Each would add
a subsystem without deepening the ones the course-style project is about.
