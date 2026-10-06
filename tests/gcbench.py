#!/usr/bin/env python3
"""Run user/gcbench.c (gc_malloc vs malloc/free, collection pauses) and
print a Markdown table.

    tests/gcbench.py [--hypervisor qemu|leapvm] [--accel tcg|hvf] [--depth D] [--trees N]

All times come from the guest's own counter (CNTVCT_EL0).  Under QEMU TCG
the guest is emulated: those numbers are emulator numbers, good for ratios
between the two allocators, not as absolute speeds."""
from __future__ import annotations

import argparse
import platform
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import BUILD, Machine  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hypervisor", default="qemu")
    ap.add_argument("--accel", default="tcg")
    ap.add_argument("--depth", type=int, default=12)
    ap.add_argument("--trees", type=int, default=40)
    a = ap.parse_args()
    if a.hypervisor == "leapvm":
        label = "LeapVM (Hypervisor.framework)"
    else:
        label = f"QEMU {a.accel.upper()}" + (" (emulated)" if a.accel == "tcg" else "")
    with Machine(cpus=1, hypervisor=a.hypervisor, accel=a.accel) as m:
        m.wait_prompt(120)
        out = m.run(f"gcbench {a.depth} {a.trees}", timeout=1800)
        m.poweroff()
    head = re.search(r"^gcbench: .*$", out, re.M)
    rows = [(n, v, u) for n, v, u in re.findall(r"^BENCH (\S+)\s+([\d.]+) (.+)$", out, re.M) if n != "done"]
    host = f"{platform.machine()} {platform.system()} {platform.release()}"
    lines = [f"Results: {label}, 1 vCPU, host {host}", "", head.group(0) if head else "", "",
             f"| Measurement | {label} |", "|---|---:|"]
    lines += [f"| {n} | {v} {u} |" for n, v, u in rows]
    text = "\n".join(lines) + "\n"
    print(text)
    tag = a.accel if a.hypervisor == "qemu" else "hvf"
    (BUILD / f"gcbench-{a.hypervisor}-{tag}.md").write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
