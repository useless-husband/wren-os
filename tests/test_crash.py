"""Crash consistency (see tests/crash.py for the method).  The number of
runs is WREN_CRASH_RUNS (default 40); `make crash` runs 300."""
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def test_crash_consistency():
    runs = int(os.environ.get("WREN_CRASH_RUNS", "40"))
    r = subprocess.run([sys.executable, str(HERE / "crash.py"), "--runs", str(runs), "--jobs", "4"],
                       capture_output=True, text=True, timeout=7200)
    print(r.stdout[-4000:])
    assert r.returncode == 0, r.stdout[-6000:] + r.stderr[-2000:]
    assert f"{runs}/{runs} runs consistent" in r.stdout
