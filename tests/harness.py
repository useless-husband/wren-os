"""Drive wren-os under QEMU (or LeapVM) through its serial console.

    with Machine(cpus=4) as m:
        m.wait_prompt()
        out = m.run("usertests -q", timeout=600)

A background thread collects everything the guest prints; expect() waits
for a regular expression in that output.  A kernel panic anywhere in the
output raises GuestPanic at the next expect(), with the backtrace already
translated to function names (llvm-addr2line on build/kernel.elf).
"""
from __future__ import annotations

import os
import platform
import re
import shutil
import signal
import subprocess
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
IMAGE = BUILD / "Image"
FS_IMG = BUILD / "fs.img"
KERNEL_ELF = BUILD / "kernel.elf"
PROMPT = "$ "

# Generous by default: CI runs QEMU in pure emulation (TCG) on shared runners.
TIMEOUT_SCALE = float(os.environ.get("WREN_TIMEOUT_SCALE", "1"))


def find_tool(name: str) -> str | None:
    for d in ("/opt/homebrew/opt/llvm/bin", "/usr/lib/llvm-18/bin", "/usr/bin"):
        p = Path(d) / name
        if p.exists():
            return str(p)
    return shutil.which(name)


def leapvm_path() -> Path | None:
    env = os.environ.get("LEAPVM")
    if env:
        return Path(env) if Path(env).exists() else None
    p = Path.home() / "Desktop" / "Claude專案" / "Mac自製Linux虛擬機 LeapVM" / "leapvm"
    return p if p.exists() else None


class GuestPanic(AssertionError):
    pass


class GuestTimeout(AssertionError):
    pass


def symbolize(text: str) -> str:
    """Append function names to the addresses in a panic backtrace."""
    tool = find_tool("llvm-addr2line")
    addrs = re.findall(r"^\s+(0xffffffffc[0-9a-f]+)\s*$", text, re.M)
    if not tool or not addrs or not KERNEL_ELF.exists():
        return text
    try:
        res = subprocess.run([tool, "-f", "-C", "-s", "-e", str(KERNEL_ELF), *addrs],
                             capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return text
    lines = res.stdout.splitlines()
    for i, a in enumerate(addrs):
        if 2 * i + 1 < len(lines):
            text = text.replace(a, f"{a} {lines[2 * i]} ({lines[2 * i + 1]})", 1)
    return text


class Machine:
    def __init__(self, cpus: int = 1, mem: str = "256M", disk: Path | None = None,
                 fresh_disk: bool = True, virtio: str = "legacy", bootargs: str = "",
                 hypervisor: str = "qemu", accel: str | None = None, log: Path | None = None,
                 tmpdir: Path | None = None, machine_opts: str = ""):
        self.cpus = cpus
        self.hypervisor = hypervisor
        self.out = bytearray()
        self.lock = threading.Lock()
        self.pos = 0                      # expect() searches from here
        tmpdir = Path(tmpdir or (BUILD / "tmp"))
        tmpdir.mkdir(parents=True, exist_ok=True)
        if disk is None or fresh_disk:
            src = disk or FS_IMG
            self.disk = tmpdir / f"disk-{os.getpid()}-{id(self)}.img"
            shutil.copyfile(src, self.disk)
            self.own_disk = True
        else:
            self.disk = Path(disk)
            self.own_disk = False
        self.log_path = log
        if hypervisor == "leapvm":
            lv = leapvm_path()
            if not lv:
                raise RuntimeError("LeapVM not found (set LEAPVM=/path/to/leapvm)")
            mem_mb = int(mem.rstrip("M"))
            cmd = [str(lv), "-k", str(IMAGE), "-c", str(cpus), "-m", str(mem_mb), "--no-net",
                   "--disk", str(self.disk)]
            if bootargs:
                cmd += ["-a", bootargs]
        else:
            if accel is None:
                accel = os.environ.get("WREN_ACCEL", "tcg")
            cpu = "host" if accel == "hvf" else "cortex-a72"
            machine = "virt,gic-version=3" + (f",{machine_opts}" if machine_opts else "")
            cmd = ["qemu-system-aarch64", "-machine", machine, "-accel", accel,
                   "-cpu", cpu, "-smp", str(cpus), "-m", mem, "-display", "none",
                   "-serial", "stdio", "-monitor", "none", "-no-reboot", "-kernel", str(IMAGE),
                   "-drive", f"file={self.disk},if=none,format=raw,id=d0",
                   "-device", "virtio-blk-device,drive=d0"]
            if virtio == "modern":
                cmd += ["-global", "virtio-mmio.force-legacy=false"]
            if bootargs:
                cmd += ["-append", bootargs]
        self.cmd = cmd
        self.start_time = time.monotonic()
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, start_new_session=True)
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    # ------------------------------------------------------------ plumbing
    def _read(self):
        assert self.proc.stdout
        while True:
            chunk = self.proc.stdout.read1(4096) if hasattr(self.proc.stdout, "read1") else \
                self.proc.stdout.read(1)
            if not chunk:
                break
            with self.lock:
                self.out += chunk

    def text(self) -> str:
        with self.lock:
            return self.out.decode("utf-8", "replace").replace("\r\n", "\n")

    def alive(self) -> bool:
        return self.proc.poll() is None

    def _check_panic(self, text: str):
        i = text.find("PANIC on cpu")
        if i >= 0:
            time.sleep(0.5)                       # let the backtrace finish printing
            raise GuestPanic(symbolize(self.text()[i:]))

    def expect(self, pattern: str, timeout: float = 30.0) -> re.Match:
        """Wait for pattern (a regex) to appear after the previous match."""
        deadline = time.monotonic() + timeout * TIMEOUT_SCALE
        rx = re.compile(pattern)
        while True:
            text = self.text()
            m = rx.search(text, self.pos)
            if m:
                self.pos = m.end()
                return m
            self._check_panic(text)
            if not self.alive():
                time.sleep(0.2)
                text = self.text()
                m = rx.search(text, self.pos)
                if m:
                    self.pos = m.end()
                    return m
                raise GuestTimeout(f"machine exited while waiting for {pattern!r}; tail:\n{text[-2000:]}")
            if time.monotonic() > deadline:
                raise GuestTimeout(f"timeout ({timeout}s) waiting for {pattern!r}; tail:\n{text[-2000:]}")
            time.sleep(0.02)

    def send(self, s: str):
        assert self.proc.stdin
        self.proc.stdin.write(s.encode())
        self.proc.stdin.flush()

    def wait_prompt(self, timeout: float = 60.0):
        self.expect(re.escape(PROMPT), timeout)

    def run(self, cmd: str, timeout: float = 60.0) -> str:
        """Type a command line, wait for the next prompt, return the output."""
        start = self.pos
        self.send(cmd + "\n")
        self.expect(re.escape(PROMPT), timeout)
        text = self.text()[start:self.pos]
        body = text.split("\n", 1)[1] if "\n" in text else ""     # drop the echoed command
        return body[: -len(PROMPT)] if body.endswith(PROMPT) else body

    def poweroff(self, timeout: float = 30.0) -> int:
        self.send("poweroff\n")
        return self.wait_exit(timeout)

    def wait_exit(self, timeout: float = 30.0) -> int:
        try:
            return self.proc.wait(timeout * TIMEOUT_SCALE)
        except subprocess.TimeoutExpired:
            self.kill()
            raise GuestTimeout("machine did not power off")

    def kill(self):
        """Pull the plug: SIGKILL the hypervisor process."""
        if self.alive():
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait()

    def close(self):
        self.kill()
        self.reader.join(timeout=2)
        for f in (self.proc.stdin, self.proc.stdout):
            try:
                if f:
                    f.close()
            except OSError:
                pass
        if self.log_path:
            self.log_path.parent.mkdir(parents=True, exist_ok=True)
            self.log_path.write_text(self.text())
        if self.own_disk and self.disk.exists():
            self.disk.unlink()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def host_is_macos() -> bool:
    return platform.system() == "Darwin"


def main(argv=None) -> int:
    """Boot, run one command, check its output:

        python3 tests/harness.py [--cpus N] [--leapvm] [--expect TEXT] [--timeout S] [--fsck] "command"

    Exit status 0 if TEXT appeared (and, with --fsck, the disk is clean after
    power-off); 1 otherwise, including kernel panics and timeouts."""
    import argparse
    import subprocess as sp
    ap = argparse.ArgumentParser()
    ap.add_argument("command")
    ap.add_argument("--cpus", type=int, default=1)
    ap.add_argument("--leapvm", action="store_true")
    ap.add_argument("--expect", default="")
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--fsck", action="store_true")
    a = ap.parse_args(argv)
    disk = BUILD / "tmp" / f"cli-{os.getpid()}.img"
    disk.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(FS_IMG, disk)
    ok = False
    try:
        with Machine(cpus=a.cpus, disk=disk, fresh_disk=False,
                     hypervisor="leapvm" if a.leapvm else "qemu") as m:
            m.wait_prompt(60)
            out = m.run(a.command, timeout=a.timeout)
            print(out)
            ok = a.expect in out
            m.poweroff()
        if ok and a.fsck:
            r = sp.run([str(BUILD / "host" / "fsck"), str(disk)], capture_output=True, text=True)
            print(r.stdout + r.stderr)
            ok = r.returncode == 0
    except (GuestPanic, GuestTimeout) as e:
        print(f"{type(e).__name__}: {e}")
        ok = False
    finally:
        disk.unlink(missing_ok=True)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
