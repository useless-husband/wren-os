"""The in-guest test suite (user/usertests.c) on 1 and 4 CPUs."""
import re

import pytest

from harness import Machine


@pytest.mark.parametrize("cpus", [1, 4])
def test_usertests(cpus, logdir):
    with Machine(cpus=cpus, log=logdir / f"usertests-{cpus}cpu.log") as m:
        m.wait_prompt(60)
        out = m.run("usertests", timeout=1200)
        failed = re.findall(r"test (\S+): FAILED", out)
        assert not failed, f"failed: {failed}\n{out[-3000:]}"
        summary = re.search(r"usertests: (\d+) passed, (\d+) failed", out)
        assert summary and int(summary.group(1)) >= 40 and summary.group(2) == "0", out[-2000:]
        assert "ALL TESTS PASSED" in out
        assert m.poweroff() == 0
