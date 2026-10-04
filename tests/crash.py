#!/usr/bin/env python3
"""Crash-consistency test: cut the power at many points during file-system
writes, then prove the file system is consistent and lost nothing that was
committed.

For each run (seed printed, so any failure can be replayed alone):
  1. boot a fresh disk image and start `fswork W` (workload W of 5);
  2. cut the power, in one of three ways:
       inject - the kernel powers off right before disk write N
       torn   - same, but write N is cut short after k of its 8 sectors
       kill   - SIGKILL the hypervisor process after a random delay
  3. run the host fsck on the crashed image (it replays the log in memory,
     independently of the kernel) - must be consistent;
  4. boot the crashed image again (the kernel recovers the log at mount),
     power off cleanly, run fsck again - must be consistent;
  5. both recovered states must be identical, and must equal the state
     after every operation that reported OK plus some prefix of the one in
     flight (tests/crashmodel.py).

Usage: tests/crash.py [--runs N] [--first-seed S] [--jobs J] [--seeds a,b,...]
"""
from __future__ import annotations

import argparse
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import BUILD, FS_IMG, GuestPanic, GuestTimeout, Machine  # noqa: E402
import crashmodel  # noqa: E402

FSCK = BUILD / "host" / "fsck"
WORKLOADS = 5
CPUS = 2
KILL_OPS = 600      # long enough that a random SIGKILL lands in the middle of it


def fsck(img: Path) -> tuple[int, str, str]:
    r = subprocess.run([str(FSCK), "--dump", str(img)], capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout, r.stderr


class Workloads:
    """Dry-run each workload once to learn how many disk writes and how
    long it takes, so crash points can be spread over the whole run."""

    def __init__(self, tmp: Path):
        self.writes: dict[int, int] = {}
        self.seconds: dict[int, float] = {}
        self.tmp = tmp
        self.lock = threading.Lock()

    def get(self, w: int, nops: int = 48) -> tuple[int, float]:
        key = (w, nops)
        with self.lock:
            if key not in self.writes:
                with Machine(cpus=CPUS, tmpdir=self.tmp) as m:
                    m.wait_prompt(120)
                    m.send(f"fswork {w} 0 0 {nops}\n")
                    m.expect("FSWORK START", 60)
                    t0 = time.monotonic()
                    m.expect(r"FSWORK DONE writes=(\d+)|FSWORK ERROR", 600)
                    self.seconds[key] = time.monotonic() - t0
                    mm = re.search(r"FSWORK DONE writes=(\d+)", m.text())
                    if not mm:
                        raise AssertionError(f"dry run of workload {w} failed:\n{m.text()[-2000:]}")
                    self.writes[key] = int(mm.group(1))
            return self.writes[key], self.seconds[key]


def one_run(seed: int, workloads: Workloads, tmp: Path) -> dict:
    rnd = random.Random(seed)
    w = seed % WORKLOADS + 1
    mode = rnd.choice(["inject", "inject", "torn", "kill"])
    nops = KILL_OPS if mode == "kill" else 48
    total_writes, seconds = workloads.get(w, nops)
    desc = {"seed": seed, "workload": w, "mode": mode}
    img = tmp / f"crash-{seed}.img"
    shutil.copyfile(FS_IMG, img)
    try:
        with Machine(cpus=CPUS, disk=img, fresh_disk=False, tmpdir=tmp) as m:
            m.wait_prompt(120)
            if mode == "kill":
                delay = rnd.uniform(0.0, seconds * 1.1)
                desc["delay_s"] = round(delay, 3)
                m.send(f"fswork {w} 0 0 {nops}\n")
                m.expect("FSWORK START", 60)
                time.sleep(delay)
                m.kill()
            else:
                n = rnd.randint(1, total_writes)
                torn = rnd.randint(1, 7) if mode == "torn" else 0
                desc["crash_before_write"] = n
                if torn:
                    desc["torn_sectors"] = torn
                m.send(f"fswork {w} {n} {torn}\n")
                m.expect(r"\[crash\] power cut|FSWORK DONE", 600)
                if "FSWORK DONE" in m.text():          # crash point not reached
                    m.wait_prompt(60)
                    m.poweroff(60)
                else:
                    m.wait_exit(60)
            console = m.text()
        completed, in_flight, finished = crashmodel.parse(console)
        desc["ops_committed"] = len(completed)
        desc["in_flight"] = " ".join(in_flight) if in_flight else None
        if "FSWORK ERROR" in console:
            raise AssertionError("workload reported an error:\n" + console[-1500:])

        rc, dump_host, err = fsck(img)                      # 3. reference recovery
        if rc != 0:
            raise AssertionError(f"fsck of the crashed image failed:\n{err}")
        with Machine(cpus=CPUS, disk=img, fresh_disk=False, tmpdir=tmp) as m:   # 4. kernel recovery
            m.wait_prompt(120)
            recovery = re.findall(r"log: (replayed|discarding).*", m.text())
            m.poweroff(60)
        desc["kernel_recovery"] = recovery[0] if recovery else "log empty"
        rc, dump_kernel, err = fsck(img)
        if rc != 0:
            raise AssertionError(f"fsck after kernel recovery failed:\n{err}")
        got_host, got_kernel = crashmodel.parse_dump(dump_host), crashmodel.parse_dump(dump_kernel)
        if got_host != got_kernel:
            raise AssertionError(f"kernel recovery differs from fsck's replay:\n"
                                 f"only fsck: {sorted(got_host - got_kernel)}\n"
                                 f"only kernel: {sorted(got_kernel - got_host)}")
        allowed = crashmodel.allowed_states(console)
        if got_kernel not in allowed:
            best = min(allowed, key=lambda s: len(s ^ got_kernel))
            raise AssertionError(f"recovered state matches no allowed state "
                                 f"(committed ops lost or a partial operation is visible):\n"
                                 f"missing: {sorted(best - got_kernel)[:6]}\n"
                                 f"unexpected: {sorted(got_kernel - best)[:6]}")
        desc["prefix_of_in_flight"] = allowed.index(got_kernel)
        desc["ok"] = True
    except (AssertionError, GuestPanic, GuestTimeout) as e:
        desc["ok"] = False
        desc["error"] = str(e)
    finally:
        if img.exists():
            img.unlink()
    return desc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=100)
    ap.add_argument("--first-seed", type=int, default=1)
    ap.add_argument("--seeds", type=str, default="")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()
    seeds = [int(s) for s in args.seeds.split(",") if s] or \
        list(range(args.first_seed, args.first_seed + args.runs))
    tmp = Path(tempfile.mkdtemp(prefix="wren-crash-", dir=BUILD))
    workloads = Workloads(tmp)
    for w in range(1, WORKLOADS + 1):
        n, s = workloads.get(w)
        nk, sk = workloads.get(w, KILL_OPS)
        print(f"workload {w}: {n} disk writes in {s:.2f} s; {KILL_OPS}-op variant: {nk} writes in {sk:.2f} s",
              flush=True)
    failures = 0
    counts: dict[str, int] = {}
    t0 = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for d in pool.map(lambda s: one_run(s, workloads, tmp), seeds):
            counts[d["mode"]] = counts.get(d["mode"], 0) + 1
            where = (f"before write {d.get('crash_before_write')}" + (f" torn {d['torn_sectors']}/8" if "torn_sectors" in d else "")
                     if d["mode"] != "kill" else f"SIGKILL after {d.get('delay_s')} s")
            if d["ok"]:
                print(f"seed {d['seed']:4d} workload {d['workload']} {d['mode']:6s} {where:30s} "
                      f"ops committed {d['ops_committed']:2d}, in-flight prefix {d['prefix_of_in_flight']}, "
                      f"{d['kernel_recovery']}: OK", flush=True)
            else:
                failures += 1
                print(f"seed {d['seed']:4d} workload {d['workload']} {d['mode']} {where}: FAILED\n{d['error']}",
                      flush=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"crash test: {len(seeds) - failures}/{len(seeds)} runs consistent "
          f"({', '.join(f'{k} {v}' for k, v in sorted(counts.items()))}) in {time.monotonic() - t0:.0f} s")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
