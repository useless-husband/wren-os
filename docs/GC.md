# A conservative garbage collector and a leak checker for wren-os

wren-os user programs are written in C. This note describes two additions to the user library that
work on unmodified C: a conservative mark-sweep collector (`gc_malloc`, in the style of the
Boehm-Demers-Weiser collector) and a leak checker for the ordinary `malloc` (in the style of
LeakSanitizer). Both use the same idea: treat every aligned word in the program's registers, stack
and globals as a possible pointer, and find everything reachable from there. It is a learning
reimplementation modelled on the final projects of Stanford's CS140E/CS240LX, where students build a
Boehm-style collector and a leak detector on their own operating system. Nothing in it is new.

| File | Role |
|---|---|
| `user/lib/regs.S` | spills the callee-saved registers, then calls back with the stack pointer |
| `user/lib/roots.c` | reports the roots: registers + stack, then `.data`/`.bss` |
| `user/lib/crt0.S`, `user/user.ld` | record the stack top; export `__data_start` and `_end` |
| `user/lib/gc.c` | the collector core: arena, size classes, marking, sweeping (host-testable) |
| `user/lib/gc_wren.c` | the wren-os front end: arena from `sbrk`, trigger, pause times |
| `user/lib/malloc.c` | `malloc`/`free` with allocation sites, and `leak_check()` |
| `user/leakcheck.c` | `leakcheck prog args...` runs any program with leak checking |
| `kernel/sysproc.c` | `personality()`: the one kernel change (27 lines with the header) |

## 1. Roots

A word is a root if the program could load it without following a pointer: it is in a register,
on the stack, or in a global.

- **Registers.** `roots_with_registers` (regs.S) pushes x19-x28, the frame pointer, the link
  register and d8-d15 (160 bytes, every slot written) and calls the scanner with `sp` pointing at
  that area. Only callee-saved registers need this: by the AArch64 procedure call standard the
  caller-saved ones (x0-x18, v0-v7, v16-v31) are dead across a call, so the compiler has already
  stored any value it still needs from them on the stack.
- **Stack.** The scan covers `[sp inside the spill helper, stack top)`. The stack top is the `sp`
  that exec handed to `_start`: crt0 stores it in `__stack_top` before calling `main`. Only `argv`
  and its strings lie above it, and they hold no heap pointers. Frames below the spill area (the
  scanner's own) are not scanned.
- **Globals.** `[__data_start, _end)` from the linker script: `.data` and `.bss`. Read-only data is
  skipped; it cannot hold a pointer created at run time. The collector's own state (`struct
  gc_heap`, which holds free-list heads and statistics) is cut out of that range, so it can never
  keep an object alive.

No kernel support was needed for any of this: exec already fixes the stack top and the linker
knows the segment bounds. An auxiliary-vector entry or a system call would only have duplicated
what crt0 can record for free.

## 2. Heap layout

```
4 GiB ->  [ page descriptors | mark stack | object pages ...                       ]
          one sbrk of 256 MiB, backed by the kernel page by page on first touch
```

- **Arena.** The first `gc_malloc` reserves 256 MiB with one `sbrk`. wren-os backs the heap lazily
  (`vm_fault`), so this is address space, not memory. The arena starts at 4 GiB: the gap below it
  costs nothing either, and it means that no value that fits in 32 bits (counters, sizes, small
  integers) can ever look like a heap pointer. `malloc` keeps working; its `sbrk` calls land above.
- **Small objects** (up to 2048 bytes) use 16 size classes (16, 32, 48, ..., 1024, 2048). A page
  holds objects of one class. Its descriptor has the class, the object size and count, and two
  256-bit bitmaps, `alloc` and `mark`, one bit per object. Free objects of a class are threaded on
  one free list through their first word.
- **Large objects** take whole contiguous pages: a `GP_LARGE` head page followed by `GP_CONT` pages
  that point back to it. Free pages below the top form a list of runs, allocated first fit.
- `gc_malloc` returns zeroed memory, so a new object never contains stale pointers.

## 3. Which words are pointers

The test for "word *w* keeps object *X* alive" (`lookup` in gc.c), in order:

1. *w* is inside the object pages handed out so far (one unsigned compare);
2. the page of *w* holds objects (small, large head, or a large object's continuation page);
3. small page: the slot index `offset / size` is below the object count (the unused tail of a page
   is not an object) and the slot's `alloc` bit is set (free slots are never objects);
4. **interior-pointer policy**: by default (as with the Boehm collector's `GC_all_interior_pointers`)
   any address inside the slot counts: from the first byte up to the end of the size-class slot, or
   anywhere in a large object's pages. A pointer one past the end of *X* is the start of the next
   slot, so it keeps the next object, not *X*. `gc_set_interior(false)` accepts object starts only.

Only 8-byte-aligned words are examined.

## 4. Mark and sweep

`gc_collect` stops the program (it is the program), so there is no concurrency to handle.

- **Mark.** Every root word that passes the test sets the object's mark bit and pushes the object
  on an explicit mark stack (8,192 entries), so marking needs no recursion. Popping an object scans
  its words the same way. Objects are marked before they are pushed, so cycles end.
- **Overflow.** A full mark stack does not lose work: the object stays marked and an overflow flag
  is set. After the stack drains, the collector rescans every marked object, pushing children it had
  missed, and repeats until a pass ends without overflow. Each overflowing pass has marked at least
  one new object, so this terminates. (The same recovery as the Boehm collector's.) The host test
  runs the model check with a 4-entry stack to exercise it.
- **Sweep.** Per page, `alloc &= mark` frees every unmarked object at once, and the mark bits are
  cleared. A small page with no survivors and every unmarked large object goes back to the free
  pages. The free lists are then rebuilt from the `alloc` bitmaps, neighbouring free pages are
  merged into runs, and a free run at the top of the arena lowers the top.
- **Trigger.** A collection runs when the bytes allocated since the last one reach the larger of
  256 KiB and the bytes that survived the last one, so the heap settles at about twice the live data.
  If the arena is full, `gc_malloc` collects once more before returning NULL.

## 5. The leak checker

`malloc` (moved from `ulib.c` to `malloc.c`) keeps its first-fit, address-ordered free list with
coalescing. Three things were added:

- Each block header is `{size | flags, link-or-site}`: while a block is allocated its second word
  holds the **allocation site**, the return address of the `malloc`/`calloc`/`realloc` call.
- The memory from `sbrk` is kept as a list of regions, and blocks tile each region with no gaps, so
  the heap can be walked block by block. A second `free` of the same block is reported instead of
  corrupting the free list.
- `leak_check()` lists every allocated block in a table (in fresh `sbrk` memory, outside the heap
  and outside the roots), marks from the same roots as the collector (any pointer into a block's
  payload counts), then follows pointers inside marked blocks. Every allocated block left unmarked
  is printed with its size and allocation site, followed by a summary line. The table is given back
  with a negative `sbrk`.

The report runs at exit when the process has the `PER_LEAKCHECK` personality flag, or whenever the
program calls `leak_check()`. **`personality()`** is a per-process flags word, as in Linux's
`personality(2)` (the mechanism behind `setarch -R`): `fork` copies it and `exec` keeps it.
`leakcheck prog args...` sets the flag and execs `prog`; the library's `exit()` checks it and runs
the report before the real `_exit`. Children inherit the flag, so `leakcheck sh` checks every
program started from that shell. This is the only kernel change; the alternatives were environment
variables (the exec ABI has no `envp`) or an inherited file descriptor as a signal (fragile).

Before scanning, the checker zeroes 4 KiB of dead stack below itself. Its own frames are built on
that memory next, and without the scrub an uninitialised slot in one of them could still hold a
pointer left by an earlier, deeper call and hide a leak. The block table is built in a separate
function whose frame is gone before the roots are scanned, for the same reason.

```
$ leakcheck leakdemo
leakdemo: planted 7 leaked blocks, 352 bytes
leak: 32 bytes at 0x161b0, allocated from pc 0x13eac
leak: 48 bytes at 0x161e0, allocated from pc 0x13ef8
...
leakcheck: 7 leaked blocks, 352 bytes; 6 blocks, 304 bytes still reachable
```

The sites are raw addresses; `llvm-symbolizer -e build/user/leakdemo.elf 0x13eac` names the function
and line. The system test does the same lookup in the ELF symbol table.

## 6. fork and several CPUs

wren-os processes have one thread each, so the collector needs no locks. A process can migrate
between CPUs in the middle of a collection; that changes nothing, because its registers are saved
and restored with it. After `fork` the child has a copy-on-write copy of the whole heap, including
the collector's state, and collects its copy on its own. Both are tested: `gctest fork`, all of
`gctest` on 4 CPUs, and four `gcdemo` processes at once on 4 CPUs. Threads would need a
stop-the-world handshake and per-thread stack bounds; there are no threads to need them.

## 7. Tests

| Test | What it checks |
|---|---|
| `tests/unit/test_gc.c` (host, UBSan/ASan) | size classes, alignment, zeroing; the interior policy word by word in both modes; a randomised model check (3 fixed seeds x 3 configurations x 20,000 operations): after every collection, an object is allocated **if and only if** a model of the graph says it is reachable from the explicit roots, and every survivor's fields and canary are intact; a 4-entry mark stack; a 50,000-node list; the heap shrinking back to zero pages; page runs splitting and merging; the trigger; a full arena. 314,649 checks |
| `gctest` on 1 and 4 CPUs (13 tests) | reachable objects intact after 40 collections; 10,511 garbage objects reclaimed; heap bounded over 300 rounds (34 MiB allocated, peak 100 pages, kernel free pages within one page of the start); objects held only in x19-x28 or d8 survive, and the same objects die without the registers; stack and `.data`/`.bss` roots; interior and one-past-the-end pointers; cycles; fork; a seeded random graph (0 unreachable nodes retained); large objects; zeroing; coexistence with `malloc` |
| `gcdemo 400`, and 4 x `gcdemo 150` at once | the high-water mark stops growing after the first quarter |
| `leakcheck leakdemo` | exactly the 7 planted blocks, each attributed (through the ELF symbol table) to the function that leaked it; nothing for the fixed variant; the same report on demand; the flag inherited through `leakcheck sh` |
| mutants (`make mutants`) | dropping the x27/x28 spill, the overflow rescan, or the leak checker's transitive marking each makes a named test fail |
| LeapVM | `gctest` and `leakcheck leakdemo` also run on LeapVM |

The in-guest tests that must show an object was *reclaimed* keep only an XOR-hidden copy of its
address, look it up through out-of-line helpers, and scrub the dead stack before collecting. The
first version did not, and three tests failed for a real reason: the compiler had kept the address
in a callee-saved register across the call, where it is a perfectly good root. That is the false
retention section below, seen from the inside.

## 8. Measurements

`make gcbench` (tests/gcbench.py, user/gcbench.c): a long-lived binary tree of 32,767 nodes, then
40 short-lived trees of depth 12 (8,191 nodes of 24 bytes each) built, checksummed and dropped;
with `malloc` every tree is freed node by node. One guest CPU on an Apple M5 running macOS 27,
shared with other jobs. Guest timer; the TCG column is emulated and only its ratios mean anything.

| Measurement | QEMU TCG (emulated) | QEMU HVF | LeapVM |
|---|---:|---:|---:|
| `malloc` + `free`, per node | 84 ns | 9 ns | 9 ns |
| `gc_malloc`, per node (collections included) | 130 ns | 14 ns | 12 ns |
| Collections during the run | 10 | 10 | 10 |
| Share of time in the collector | 33 % | 38 % | 38 % |
| Pause, mean / max | 1,418 / 1,574 us | 178 / 256 us | 164 / 212 us |
| Full collection, 1 MiB live (min of 5) | 1,342 us | 140 us | 145 us |
| Peak heap | 2,112 KiB | 2,112 KiB | 2,112 KiB |

`gcdemo 400` under TCG: 15 MiB allocated, 61 collections, heap high-water mark 324 KiB from round
100 to round 400, pauses 79 us on average, 558 us at most.

Reading: on this workload the collector costs about 1.3-1.6 times `malloc`/`free`, and the
difference is the time spent collecting. Most of that goes into marking the long-lived tree again in
every collection, which is exactly what a generational
collector avoids (and what this one, deliberately, does not do). The pause grows with the live
heap: about 140 us per MiB of small live objects under HVF.

## 9. Limits

- **No compaction**: objects never move, so a fragmented heap stays fragmented. Pages that become
  free are reused, but never returned to the kernel (wren-os has no `munmap` or `madvise`).
- **No concurrent, incremental or generational collection**: every collection marks the whole
  live heap with the program stopped. Pauses grow with the live data (section 8).
- **False retention.** Any word that happens to point into an object keeps it, and everything it
  reaches, alive: a dead variable still in a callee-saved register, an uninitialised stack slot
  with an old value, a 64-bit integer that lands in the arena. The 4 GiB arena base removes the
  32-bit cases; the rest is inherent to conservative collection. The leak checker has the mirror
  image: such a word hides a leak (false negative); it never invents one.
- **Pointers it cannot see.** Pointers stored only in `malloc`'d memory (the malloc heap is not
  scanned), hidden by XOR or arithmetic, stored at unaligned addresses (packed structs), or tagged
  in the top byte. A program that does this must keep a plain copy in a root. Likewise, the leak
  checker reports a block reachable only through such a pointer, or only from a `gc_malloc` object.
- **Fixed arena** of 256 MiB of address space, and a mark stack of 8,192 entries (overflow costs
  time, not correctness).
- **Reported sizes** are the usable block sizes, requests rounded up to 16 bytes. Sites are raw
  return addresses, one level deep (no full stack traces).
- **No threads**: correct only because wren-os processes have one.

## 10. Alternatives considered

- **Precise collection** needs to know which words are pointers: compiler-generated stack maps or a
  tagged representation. Neither exists for plain C; conservative scanning needs nothing from the
  compiler.
- **Reference counting** cannot reclaim cycles, and the tests build cycles on purpose.
- **Object headers instead of side bitmaps.** Mark bits in the descriptor keep objects exactly their
  class size and let the sweep free a whole page's worth of objects with a few AND instructions.
- **Leak mode from an inherited file descriptor** (no kernel change): any program that happens to
  have that descriptor open would start printing reports. A flags word is 27 lines and unambiguous.

## Related work

- **Boehm-Demers-Weiser conservative collector** ([bdwgc](https://github.com/ivmai/bdwgc)): the model
  for nearly everything here: conservative root and heap scanning, size-class pages with side
  bitmaps, interior-pointer recognition, mark-stack overflow recovery, `GC_clear_stack`-style
  scrubbing. bdwgc adds blacklisting of addresses that integers have hit, parallel and incremental
  marking, finalisation, thread support and much more; this is a few hundred lines.
- **Valgrind memcheck** and **LeakSanitizer** (part of AddressSanitizer in LLVM and GCC): leak
  detection by the same reachability scan at exit. memcheck runs the program on a synthetic CPU and
  sees every allocation and every register; LeakSanitizer interposes `malloc` and scans thread stacks
  and registers. Both distinguish "definitely" from "indirectly" lost blocks and print symbolized
  stack traces; this checker reports every unreachable block with one return address.
- **Stanford CS140E / CS240LX** (embedded operating systems on the Raspberry Pi): final projects in
  these courses include a Boehm-style conservative collector for C and a leak detector running on
  the students' own operating system. This addition follows that idea on wren-os; it uses none of
  the course code.
