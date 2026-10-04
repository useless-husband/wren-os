#!/usr/bin/env python3
"""Mutation check: do the tests notice real bugs?

Each mutant is a small deliberate bug (one statement removed or changed).
For each, the source tree is copied to build/mutants/<name>/, the bug is
applied there, everything is rebuilt, and the test that should catch it is
run.  A mutant is "caught" when that test fails.  The working tree is never
modified.  Results go to build/mutants.md.

Usage: tests/mutants.py [name ...]
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "mutants"

PY = sys.executable
GUEST = [PY, "tests/harness.py"]

MUTANTS = [
    {
        "name": "no-tlb-flush-on-fork",
        "what": "fork makes the parent's pages copy-on-write but does not flush the parent's TLB",
        "file": "kernel/vm.c",
        "old": "    tlb_flush_asid(src->asid);\n    dst->heap_start",
        "new": "    dst->heap_start",
        "test": GUEST + ["--cpus", "1", "--expect", "ALL TESTS PASSED", "usertests cow_isolation cow_sharing"],
        "test_name": "usertests cow_isolation (1 cpu)",
    },
    {
        "name": "no-page-allocator-lock",
        "what": "the physical page allocator is called without its spinlock",
        "file": "kernel/kalloc.c",
        "old": "    spin_lock(&mem_lock);\n    paddr_t pa = buddy_alloc(&mem, order);\n    spin_unlock(&mem_lock);",
        "new": "    paddr_t pa = buddy_alloc(&mem, order);",
        "test": GUEST + ["--cpus", "4", "--expect", "STRESS OK", "--timeout", "300", "stress 20"],
        "test_name": "stress 20 s on 4 cpus",
    },
    {
        "name": "no-log-commit-record",
        "what": "the log never writes its commit record (header); blocks go home unprotected",
        "file": "kernel/log.c",
        "old": "    write_header(n, seq, lg.block, crc);                    /* 2. commit point */",
        "new": "    (void)crc;                                               /* 2. commit point removed */",
        "test": [PY, "tests/crash.py", "--runs", "40", "--jobs", "4"],
        "test_name": "crash test, 40 runs",
    },
    {
        "name": "no-log-recovery",
        "what": "mount ignores a committed transaction in the log instead of replaying it",
        "file": "kernel/log.c",
        "old": "    if (!ok || crc != h.crc) {",
        "new": "    if (1) {",
        "test": [PY, "tests/crash.py", "--runs", "40", "--jobs", "4"],
        "test_name": "crash test, 40 runs",
    },
    {
        "name": "no-cow-refcount",
        "what": "fork shares pages without taking a reference on them",
        "file": "kernel/vm.c",
        "old": "        page_get(t[i] & PTE_ADDR_MASK);\n",
        "new": "",
        "test": GUEST + ["--cpus", "2", "--expect", "ALL TESTS PASSED", "usertests -q"],
        "test_name": "usertests -q (2 cpus)",
    },
    {
        "name": "no-fpu-save",
        "what": "the scheduler does not save a process's FP/SIMD registers when switching away",
        "file": "kernel/proc.c",
        "old": "    fpu_save(&p->fp);\n    ctx_switch(&p->ctx, &c->sched_ctx);",
        "new": "    ctx_switch(&p->ctx, &c->sched_ctx);",
        "test": GUEST + ["--cpus", "1", "--expect", "ALL TESTS PASSED", "usertests fp_state"],
        "test_name": "usertests fp_state (1 cpu)",
    },
    {
        "name": "leak-indirect-block",
        "what": "truncating a file frees the blocks an indirect block points to, but not the indirect block",
        "file": "kernel/fs.c",
        "old": "    brelse(b);\n    bfree(dev, ind);",
        "new": "    brelse(b);",
        "test": GUEST + ["--cpus", "1", "--expect", "ALL TESTS PASSED", "--fsck", "usertests big_file"],
        "test_name": "usertests big_file, then host fsck",
    },
    {
        "name": "no-pipe-reader-wakeup",
        "what": "a reader draining a full pipe does not wake the blocked writer",
        "file": "kernel/pipe.c",
        "old": "    proc_wakeup(&pi->nwrite);\n    spin_unlock(&pi->lock);\n    return done;\n}",
        "new": "    spin_unlock(&pi->lock);\n    return done;\n}",
        "test": GUEST + ["--cpus", "2", "--expect", "ALL TESTS PASSED", "--timeout", "90", "usertests pipe_bulk"],
        "test_name": "usertests pipe_bulk (hang -> timeout)",
    },
]


def copy_tree(dest: Path):
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(ROOT, dest, ignore=shutil.ignore_patterns("build", ".git", ".venv", "__pycache__",
                                                               ".pytest_cache", "*.img"))


def run_mutant(m: dict) -> dict:
    dest = OUT / m["name"]
    copy_tree(dest)
    src = dest / m["file"]
    text = src.read_text()
    if text.count(m["old"]) != 1:
        return {**m, "result": "ERROR", "detail": f"pattern found {text.count(m['old'])} times in {m['file']}"}
    src.write_text(text.replace(m["old"], m["new"]))
    b = subprocess.run(["make", "-s", "-j4", "all"], cwd=dest, capture_output=True, text=True)
    if b.returncode != 0:
        return {**m, "result": "BUILD FAILED", "detail": (b.stdout + b.stderr)[-1500:]}
    t0 = time.monotonic()
    r = subprocess.run(m["test"], cwd=dest, capture_output=True, text=True, timeout=3600)
    elapsed = time.monotonic() - t0
    out = r.stdout + r.stderr
    evidence = ""
    for key in ("runs consistent", "PANIC", "FAIL", "FAILED", "GuestTimeout", "inconsisten", "matches no allowed state",
                "fsck: ", "corrupted", "segmentation"):
        i = out.find(key)
        if i >= 0:
            evidence = out[i:i + 160].splitlines()[0]
            break
    shutil.rmtree(dest, ignore_errors=True)
    return {**m, "result": "caught" if r.returncode != 0 else "SURVIVED", "detail": evidence,
            "seconds": elapsed}


def main() -> int:
    wanted = sys.argv[1:]
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    for m in MUTANTS:
        if wanted and m["name"] not in wanted:
            continue
        print(f"mutant {m['name']}: {m['what']} ...", flush=True)
        res = run_mutant(m)
        print(f"  -> {res['result']} by {m['test_name']} {res.get('detail', '')}", flush=True)
        rows.append(res)
    lines = ["| Mutant | Bug introduced | Test | Result | Evidence |", "|---|---|---|---|---|"]
    for r in rows:
        ev = r.get("detail", "").replace("|", "/")
        lines.append(f"| `{r['name']}` | {r['what']} | {r['test_name']} | {r['result']} | {ev} |")
    table = "\n".join(lines) + "\n"
    (ROOT / "build" / "mutants.md").write_text(table)
    print(table)
    caught = sum(r["result"] == "caught" for r in rows)
    print(f"mutants: {caught}/{len(rows)} caught")
    return 0 if caught == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
