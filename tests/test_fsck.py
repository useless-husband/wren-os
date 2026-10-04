"""The host fsck must accept fresh images and catch each kind of damage."""
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

from harness import BUILD, FS_IMG
import crashmodel

FSCK = BUILD / "host" / "fsck"
BSIZE = 4096


def run_fsck(img):
    return subprocess.run([str(FSCK), str(img)], capture_output=True, text=True)


class Image:
    def __init__(self, path: Path):
        self.path = path
        self.data = bytearray(path.read_bytes())
        (self.magic, self.size, self.nblocks, self.ninodes, self.nlog, self.logstart,
         self.inodestart, self.bmapstart, self.datastart) = struct.unpack_from("<9I", self.data, BSIZE)

    def save(self):
        self.path.write_bytes(self.data)

    def inode_off(self, inum):
        return (self.inodestart + inum // 64) * BSIZE + (inum % 64) * 64

    def inode(self, inum):
        t, major, nlink, _, size = struct.unpack_from("<4HI", self.data, self.inode_off(inum))
        addrs = struct.unpack_from("<13I", self.data, self.inode_off(inum) + 12)
        return t, nlink, size, list(addrs)

    def set_nlink(self, inum, n):
        struct.pack_into("<H", self.data, self.inode_off(inum) + 4, n)

    def set_addr(self, inum, k, b):
        struct.pack_into("<I", self.data, self.inode_off(inum) + 12 + 4 * k, b)

    def flip_bitmap(self, b):
        self.data[self.bmapstart * BSIZE + b // 8] ^= 1 << (b % 8)

    def dirents(self, inum):
        t, _, size, addrs = self.inode(inum)
        out = []
        for off in range(0, size, 32):
            blk = addrs[off // BSIZE]
            pos = blk * BSIZE + off % BSIZE
            ino = struct.unpack_from("<I", self.data, pos)[0]
            name = self.data[pos + 4:pos + 32].rstrip(b"\0").decode()
            out.append((pos, ino, name))
        return out

    def lookup(self, path):
        inum = 1
        for comp in path.strip("/").split("/"):
            inum = next(i for _, i, n in self.dirents(inum) if n == comp)
        return inum


@pytest.fixture
def img(tmp_path):
    p = tmp_path / "fs.img"
    shutil.copyfile(FS_IMG, p)
    return p


def test_fresh_image_is_clean(img):
    r = run_fsck(img)
    assert r.returncode == 0, r.stderr
    assert "clean" in r.stdout


def test_detects_leaked_block(img):
    im = Image(img)
    im.flip_bitmap(im.size - 1)             # marked used, owned by nobody
    im.save()
    r = run_fsck(img)
    assert r.returncode == 1 and "unreferenced" in r.stderr


def test_detects_block_in_use_but_free(img):
    im = Image(img)
    _, _, _, addrs = im.inode(im.lookup("/motd.txt"))
    im.flip_bitmap(addrs[0])
    im.save()
    r = run_fsck(img)
    assert r.returncode == 1 and "bitmap says free" in r.stderr


def test_detects_wrong_link_count(img):
    im = Image(img)
    im.set_nlink(im.lookup("/bin/sh"), 2)
    im.save()
    r = run_fsck(img)
    assert r.returncode == 1 and "link count 2 but 1" in r.stderr


def test_detects_doubly_owned_block(img):
    im = Image(img)
    a = im.inode(im.lookup("/bin/cat"))[3]
    im.set_addr(im.lookup("/bin/echo"), 0, a[0])
    im.save()
    r = run_fsck(img)
    assert r.returncode == 1 and "owned by inode" in r.stderr


def test_detects_pointer_outside_data_area(img):
    im = Image(img)
    im.set_addr(im.lookup("/motd.txt"), 0, 3)    # inside the log
    im.save()
    r = run_fsck(img)
    assert r.returncode == 1 and "outside the data area" in r.stderr


def test_detects_entry_to_free_inode(img):
    im = Image(img)
    pos, ino, name = next(e for e in im.dirents(1) if e[2] == "motd.txt")
    struct.pack_into("<I", im.data, pos, im.ninodes - 1)
    im.save()
    r = run_fsck(img)
    assert r.returncode == 1 and "free or invalid inode" in r.stderr


def _write_log(im: Image, blocks: dict, seq: int, good_crc: bool):
    """Put a transaction in the log: blocks maps home block -> 4 KiB contents."""
    homes = list(blocks)
    for i, h in enumerate(homes):
        im.data[(im.logstart + 1 + i) * BSIZE:(im.logstart + 2 + i) * BSIZE] = blocks[h]
    hdr = struct.pack("<IIQ", 0x474F4C57, len(homes), seq)
    crc_input = hdr[:4] + hdr[4:8] + hdr[8:16] + struct.pack(f"<{len(homes)}I", *homes)
    crc = crashmodel._crc32c(crc_input + b"".join(blocks[h] for h in homes))
    if not good_crc:
        crc ^= 1
    head = struct.pack("<IIQI", 0x474F4C57, len(homes), seq, crc) + struct.pack(f"<{len(homes)}I", *homes)
    im.data[im.logstart * BSIZE:(im.logstart + 1) * BSIZE] = head.ljust(BSIZE, b"\0")


def test_replays_committed_log_and_ignores_uncommitted(img):
    im = Image(img)
    motd = im.lookup("/motd.txt")
    blk = im.inode(motd)[3][0]
    new = bytearray(im.data[blk * BSIZE:(blk + 1) * BSIZE])
    new[0:7] = b"REPLAY!"
    _write_log(im, {blk: bytes(new)}, seq=77, good_crc=False)
    im.save()
    r = subprocess.run([str(FSCK), "--dump", str(img)], capture_output=True, text=True)
    assert r.returncode == 0 and "not committed" in r.stdout
    _write_log(im, {blk: bytes(new)}, seq=77, good_crc=True)
    im.save()
    r = subprocess.run([str(FSCK), "--dump", str(img)], capture_output=True, text=True)
    assert r.returncode == 0 and "replayed committed transaction 77" in r.stdout
    size = im.inode(motd)[2]
    want = crashmodel.crc32c(bytes(new[:size]))
    line = next(l for l in r.stdout.splitlines() if l.startswith("/motd.txt"))
    assert int(line.split()[3], 16) == want
