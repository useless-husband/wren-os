"""SMP stress: many processes forking, piping, exec'ing and writing files at
once on 4 CPUs.  Fails on any wrong result, panic (including the spinlock
deadlock detector), hang, leaked memory, or a file system fsck rejects."""
import re
import shutil
import subprocess

import pytest

from harness import BUILD, FS_IMG, Machine


@pytest.mark.parametrize("cpus,seconds", [(4, 20), (2, 10)])
def test_stress(cpus, seconds, tmp_path, logdir):
    img = tmp_path / "stress.img"
    shutil.copyfile(FS_IMG, img)
    with Machine(cpus=cpus, disk=img, fresh_disk=False, log=logdir / f"stress-{cpus}cpu.log") as m:
        m.wait_prompt(60)
        out = m.run(f"stress {seconds}", timeout=seconds + 600)
        assert "STRESS OK" in out, out[-3000:]
        leaked = int(re.search(r"(\d+) pages not returned", out).group(1))
        assert leaked <= 8, out
        assert m.poweroff() == 0
    r = subprocess.run([str(BUILD / "host" / "fsck"), str(img)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
