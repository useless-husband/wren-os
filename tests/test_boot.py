"""Boot to the shell under QEMU in several configurations and exercise the
shell: pipes, redirection, background jobs and the utilities."""
import re

import pytest

from harness import Machine


@pytest.mark.parametrize("cpus,virtio,machine_opts", [
    (1, "legacy", ""),
    (4, "legacy", ""),
    (4, "modern", ""),                     # virtio-mmio version 2, the one LeapVM has
    (2, "legacy", "virtualization=on"),    # entered at EL2: exercises the drop to EL1
])
def test_boot_configurations(cpus, virtio, machine_opts, logdir):
    name = f"boot-{cpus}cpu-{virtio}{'-el2' if machine_opts else ''}"
    with Machine(cpus=cpus, virtio=virtio, machine_opts=machine_opts, log=logdir / f"{name}.log") as m:
        m.wait_prompt(60)
        boot = m.text()
        assert f"smp: {cpus} cpu(s) online" in boot
        assert ("modern transport" if virtio == "modern" else "legacy transport") in boot
        assert "ok" in m.run("echo ok")
        assert m.poweroff() == 0


def test_shell_features(logdir):
    with Machine(cpus=2, log=logdir / "shell.log") as m:
        m.wait_prompt(60)
        out = m.run("ls /bin")
        for prog in ("sh", "cat", "grep", "wc", "usertests", "stress"):
            assert re.search(rf"^{prog}\s+file", out, re.M), prog
        assert m.run("cat /motd.txt | wc").split()[:3] == ["4", "48", "274"]
        assert m.run("cat /motd.txt | grep -c file") .strip() == "1"
        assert m.run("grep -n '^Every' /motd.txt").strip().startswith("3:Every")
        assert m.run("grep -i 'WREN' /motd.txt | wc").split()[0] == "2"
        m.run("echo first > /out.txt")
        m.run("echo second >> /out.txt")
        assert m.run("cat < /out.txt") == "first\nsecond\n"
        assert m.run("mkdir /d; ln /out.txt /d/link; cat /d/link | wc").split()[0] == "2"
        assert "No such file" not in m.run("rm /d/link; rm /d")
        assert "no such file" in m.run("cat /d/link")
        # a pipeline of four processes
        assert m.run("cat /motd.txt | grep e | grep -v Try | wc").split()[0] == "3"
        # background job: the prompt comes back immediately, completion is reported later
        out = m.run("sleep 300 &")
        assert re.search(r"\[1\] \d+", out)
        ps = m.run("ps")
        assert re.search(r"sleeping\s+\d+\s+\d+\s+\d+\s+sleep", ps), ps
        # finished jobs are reported just before the next prompt
        assert "[1] Done" in m.run("sleep 500") + m.run("echo after")
        # kill a background job by pid
        pid = re.search(r"\[2\] (\d+)", m.run("sleep 100000 &")).group(1)
        assert "Running" in m.run("jobs")
        text = m.run(f"kill {pid}") + m.run("sleep 50") + m.run("echo x")
        assert "[2] Done" in text
        assert "not found" not in m.run("cd /bin; ls sh; cd /")
        assert "no such file" in m.run("nonexistent-command")
        assert m.poweroff() == 0


def test_kernel_reports_platform(logdir):
    with Machine(cpus=1, log=logdir / "platform.log") as m:
        m.wait_prompt(60)
        t = m.text()
        assert re.search(r'booting on "linux,dummy-virt", image at 0x40200000', t)
        assert "virtio-blk: 0xa003e00" in t      # the last virtio-mmio slot, found via the DTB
        m.poweroff()
