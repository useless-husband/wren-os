"""Model of the fswork workload, for checking a disk image after a crash.

The workload prints "OP k ..." before operation k and "OK k" after it.
Every operation is a short sequence of system calls, and every one of those
system calls is atomic and durable when it returns (one log transaction).
So after a crash the file system must equal

    all operations that printed OK, applied in order,
    plus some prefix (possibly empty, possibly complete) of the system
    calls of the one operation that was in flight.

allowed_states() lists those states; the test checks that the recovered
image (dumped by the host fsck) is one of them.
"""
from __future__ import annotations

import re

_CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0x82F63B78 if _c & 1 else _c >> 1
    _CRC_TABLE.append(_c)


_crc_cache: dict[bytes, int] = {}


def crc32c(data: bytes) -> int:
    hit = _crc_cache.get(data)
    if hit is not None:
        return hit
    crc = _crc32c(data)
    if len(_crc_cache) < 4096:
        _crc_cache[data] = crc
    return crc


def _crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for b in data:
        crc = _CRC_TABLE[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def pattern(n: int, tag: int) -> bytes:
    """Same bytes as pattern() in user/fswork.c (32-bit unsigned arithmetic)."""
    t = (tag * 40503) & 0xFFFFFFFF
    return bytes(((((i * 2654435761) & 0xFFFFFFFF) + t) & 0xFFFFFFFF) >> 11 & 0xFF for i in range(n))


class FS:
    def __init__(self):
        self.names: dict[str, int] = {}      # path -> inode id
        self.data: dict[int, bytearray] = {}
        self.dirs: set[str] = set()
        self.next_ino = 1

    def copy(self) -> "FS":
        c = FS()
        c.names = dict(self.names)
        c.data = {k: bytearray(v) for k, v in self.data.items()}
        c.dirs = set(self.dirs)
        c.next_ino = self.next_ino
        return c

    def snapshot(self) -> frozenset:
        """What fsck --dump would show under /w: (path, kind, size, crc)."""
        items = {(p, "file", len(self.data[i]), crc32c(bytes(self.data[i]))) for p, i in self.names.items()}
        items |= {(d, "dir", None, None) for d in self.dirs}
        return frozenset(items)


def steps(op: list[str]):
    """The system calls of one operation, each as a function FS -> None."""
    kind = op[0]
    if kind == "create":
        path, tag, size = op[1], int(op[2]), int(op[3])

        def opened(fs: FS):
            if path in fs.names:
                fs.data[fs.names[path]] = bytearray()
            else:
                fs.names[path] = fs.next_ino
                fs.data[fs.next_ino] = bytearray()
                fs.next_ino += 1

        def written(fs: FS):
            fs.data[fs.names[path]] = bytearray(pattern(size, tag))
        return [opened, written]
    if kind == "append":
        path, tag, size = op[1], int(op[2]), int(op[3])
        return [lambda fs: fs.data[fs.names[path]].extend(pattern(size, tag))]
    if kind == "rewrite":
        path, tag, size = op[1], int(op[2]), int(op[3])

        def rewrite(fs: FS):
            d = fs.data[fs.names[path]]
            new = pattern(size, tag)
            if len(d) < size:
                d.extend(b"\0" * (size - len(d)))
            d[:size] = new
        return [rewrite]
    if kind == "trunc":
        path = op[1]
        return [lambda fs: fs.data.__setitem__(fs.names[path], bytearray())]
    if kind == "unlink":
        path = op[1]

        def unlink(fs: FS):
            ino = fs.names.pop(path)
            if ino not in fs.names.values():
                del fs.data[ino]
        return [unlink]
    if kind == "link":
        old, new = op[1], op[2]
        return [lambda fs: fs.names.__setitem__(new, fs.names[old])]
    if kind == "mkdir":
        return [lambda fs: fs.dirs.add(op[1])]
    if kind == "rmdir":
        return [lambda fs: fs.dirs.discard(op[1])]
    raise ValueError(f"unknown operation {op}")


def parse(console: str):
    """Return (completed ops in order, in-flight op or None, finished?)."""
    ops: dict[int, list[str]] = {}
    done: list[int] = []
    for line in console.splitlines():
        m = re.match(r"OP (\d+) (.*)$", line.strip())
        if m:
            ops[int(m.group(1))] = m.group(2).split()
            continue
        m = re.match(r"OK (\d+)$", line.strip())
        if m:
            done.append(int(m.group(1)))
    for i, k in enumerate(done):
        if k != i:
            raise AssertionError(f"operations completed out of order: {done}")
    in_flight = ops.get(len(done))
    return [ops[k] for k in done], in_flight, "FSWORK DONE" in console


def allowed_states(console: str) -> list[frozenset]:
    completed, in_flight, _ = parse(console)
    fs = FS()
    fs.dirs.add("/w")
    for op in completed:
        for s in steps(op):
            s(fs)
    states = [fs.snapshot()]
    if in_flight:
        cur = fs.copy()
        for s in steps(in_flight):
            s(cur)
            states.append(cur.snapshot())
    return states


def parse_dump(text: str) -> frozenset:
    """fsck --dump output restricted to /w."""
    items = set()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) < 3 or not parts[0].startswith("/w"):
            continue
        if parts[1] == "dir":
            items.add((parts[0], "dir", None, None))
        else:
            items.add((parts[0], "file", int(parts[2]), int(parts[3], 16)))
    return frozenset(items)
