"""Boot the same kernel Image on LeapVM, the owner's own Apple Silicon
hypervisor (macOS only).  LeapVM differs from QEMU virt in its memory map,
load address, and virtio transport (modern only), so this proves the kernel
really discovers its machine from the device tree."""
import re
import shutil
import subprocess

import pytest

from harness import BUILD, FS_IMG, Machine, host_is_macos, leapvm_path

pytestmark = pytest.mark.skipif(not host_is_macos() or leapvm_path() is None,
                                reason="LeapVM runs only on macOS on Apple Silicon and was not found "
                                       "(set LEAPVM=/path/to/leapvm)")


def test_leapvm_boot_and_tests(tmp_path, logdir):
    img = tmp_path / "leap.img"
    shutil.copyfile(FS_IMG, img)
    with Machine(cpus=4, hypervisor="leapvm", disk=img, fresh_disk=False, log=logdir / "leapvm.log") as m:
        m.wait_prompt(60)
        boot = m.text()
        assert 'booting on "LeapVM", image at 0x40000000' in boot
        assert "modern transport" in boot
        assert "smp: 4 cpu(s) online" in boot
        out = m.run("usertests -q", timeout=900)
        assert "ALL TESTS PASSED" in out, out[-3000:]
        out = m.run("stress 8", timeout=600)
        assert "STRESS OK" in out, out[-3000:]
        assert m.poweroff() == 0
    r = subprocess.run([str(BUILD / "host" / "fsck"), str(img)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr


def test_leapvm_crash_recovery(tmp_path, logdir):
    """Power cut injected on LeapVM, recovered on LeapVM, checked on the host."""
    img = tmp_path / "leapcrash.img"
    shutil.copyfile(FS_IMG, img)
    with Machine(cpus=2, hypervisor="leapvm", disk=img, fresh_disk=False, log=logdir / "leapvm-crash.log") as m:
        m.wait_prompt(60)
        m.send("fswork 3 300\n")
        m.expect(r"\[crash\] power cut", 120)
        m.wait_exit(60)
    with Machine(cpus=2, hypervisor="leapvm", disk=img, fresh_disk=False) as m:
        m.wait_prompt(60)
        assert re.search(r"log: (replayed|discarding)|fs: ", m.text())
        m.poweroff()
    r = subprocess.run([str(BUILD / "host" / "fsck"), str(img)], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
