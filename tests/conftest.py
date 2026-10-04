import os
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import BUILD, FS_IMG, IMAGE, ROOT  # noqa: E402


@pytest.fixture(scope="session", autouse=True)
def built():
    """Everything the system tests boot must be up to date."""
    if os.environ.get("WREN_SKIP_BUILD") != "1":
        subprocess.run(["make", "-s", "all"], cwd=ROOT, check=True)
    assert IMAGE.exists() and FS_IMG.exists(), "run `make all` first"
    (BUILD / "logs").mkdir(parents=True, exist_ok=True)
    return True


@pytest.fixture
def logdir():
    return BUILD / "logs"
