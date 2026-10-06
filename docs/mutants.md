# Mutation check

Do the tests notice real bugs?  `make mutants` (`tests/mutants.py`) copies the source tree to
`build/mutants/<name>/`, introduces one deliberate bug there (a statement removed or changed; the
working tree is never touched), rebuilds everything, and runs the test that should catch it.  A
mutant is **caught** when that test fails.  The exact edit of each mutant is in the `MUTANTS` list
in `tests/mutants.py`.

Result of the last run (on the development machine; CI runs the same command):

| Mutant | Bug introduced | Test | Result | Evidence |
|---|---|---|---|---|
| `no-tlb-flush-on-fork` | fork makes the parent's pages copy-on-write but does not flush the parent's TLB | usertests cow_isolation (1 cpu) | caught | FAIL user/usertests.c:336: check failed: memcmp(heap, ref, N) == 0 |
| `no-page-allocator-lock` | the physical page allocator is called without its spinlock | stress 20 s on 4 cpus | caught | GuestPanic: PANIC on cpu 0: kernel stack overflow in pid 26 |
| `no-log-commit-record` | the log never writes its commit record (header); blocks go home unprotected | crash test, 40 runs | caught | crash test: 28/40 runs consistent (inject 22, kill 9, torn 9) in 4 s |
| `no-log-recovery` | mount ignores a committed transaction in the log instead of replaying it | crash test, 40 runs | caught | crash test: 22/40 runs consistent (inject 22, kill 9, torn 9) in 4 s |
| `no-cow-refcount` | fork shares pages without taking a reference on them | usertests -q (2 cpus) | caught | GuestPanic: PANIC on cpu 0: page_put: 0x48138000 not an allocated page |
| `no-fpu-save` | the scheduler does not save a process's FP/SIMD registers when switching away | usertests fp_state (1 cpu) | caught | FAIL user/usertests.c:517: FP worker 0 saw corrupted registers |
| `leak-indirect-block` | truncating a file frees the blocks an indirect block points to, but not the indirect block | usertests big_file, then host fsck | caught | fsck: 3 inconsistencies |
| `no-pipe-reader-wakeup` | a reader draining a full pipe does not wake the blocked writer | usertests pipe_bulk (hang -> timeout) | caught | GuestTimeout: timeout (90.0s) waiting for '\\$\\ '; tail: |
| `gc-no-register-spill` | the root scan stores zeros instead of x27 and x28, so pointers held only there are missed | gctest register_root (1 cpu) | caught | FAIL user/gctest.c:243: object held in x27 was not kept |
| `gc-no-overflow-rescan` | after a mark-stack overflow the collector does not rescan, so children of dropped objects die | host test_gc (model check, 4-entry mark stack) | caught | seed 1 step 47: 5 mismatches |
| `leak-no-transitive-marking` | the leak checker marks blocks the roots point to but not the blocks those point to | leakcheck leakdemo clean (1 cpu) | caught | leakcheck: 3 leaked blocks, 160 bytes; 3 blocks, 144 bytes still reachable |

Notes:

- `no-page-allocator-lock` is a real data race, so the symptom changes from run to run (a panic in
  `page_put` on a page the corrupted free list handed out twice, or a kernel data abort on a
  scribbled pointer). It was caught in each of the 3 runs I made; a race can in principle slip
  through a single 20-second run.
- The two log mutants are caught by the crash test's oracle, not by a crash: without the commit
  record, operations that printed `OK` are missing or half-applied after a power cut; without
  recovery, committed transactions are lost. 12 and 18 of the 40 runs failed respectively in the last run.
- `no-pipe-reader-wakeup` turns into a hang; the harness's timeout is what reports it.
- `gc-no-register-spill` drops x27 and x28 from the spill area. An earlier version of the test held
  its object in x19 only, and the mutant survived: `gc_collect`'s own prologue had saved x19 on the
  stack on the way down, so the root scan found it anyway. The test now holds objects in all of
  x19-x28 and d8 (see docs/GC.md).
