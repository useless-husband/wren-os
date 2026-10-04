#!/usr/bin/env python3
"""Run user/bench.c and time the boot; print a Markdown table.

    tests/bench.py [--hypervisor qemu|leapvm] [--accel tcg|hvf] [--cpus N] [--boots N]

Boot time is wall-clock from launching the hypervisor process to the first
shell prompt (it includes the hypervisor's own start-up), median of N boots.
The other numbers come from the guest's own counter (CNTVCT_EL0)."""
from __future__ import annotations

import argparse
import platform
import re
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import BUILD, Machine  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hypervisor", default="qemu")
    ap.add_argument("--accel", default="tcg")
    ap.add_argument("--cpus", type=int, default=1)
    ap.add_argument("--boots", type=int, default=5)
    a = ap.parse_args()
    label = "LeapVM" if a.hypervisor == "leapvm" else f"QEMU {a.accel.upper()}"
    boots = []
    for _ in range(a.boots):
        t0 = time.monotonic()
        with Machine(cpus=a.cpus, hypervisor=a.hypervisor, accel=a.accel) as m:
            m.wait_prompt(120)
            boots.append(time.monotonic() - t0)
            m.poweroff()
    with Machine(cpus=a.cpus, hypervisor=a.hypervisor, accel=a.accel) as m:
        m.wait_prompt(120)
        out = m.run("bench", timeout=1200)
        m.poweroff()
    rows = [("boot to shell prompt (median of %d)" % a.boots, f"{statistics.median(boots) * 1000:.0f}", "ms")]
    for name, value, unit in re.findall(r"^BENCH (\S+)\s+([\d.]+) (\S+)", out, re.M):
        if name != "done":
            rows.append((name, value, unit))
    host = f"{platform.machine()} {platform.system()} {platform.release()}"
    lines = [f"Results: {label}, {a.cpus} vCPU, host {host}", "",
             f"| Measurement | {label} |", "|---|---:|"]
    lines += [f"| {n} | {v} {u} |" for n, v, u in rows]
    text = "\n".join(lines) + "\n"
    print(text)
    out_file = BUILD / f"bench-{a.hypervisor}-{a.accel if a.hypervisor == 'qemu' else 'hvf'}.md"
    out_file.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
