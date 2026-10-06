"""The garbage collector and the leak checker, inside wren-os.

gctest (user/gctest.c) checks the collector against the real conservative
roots on 1 and 4 CPUs; gcdemo shows the heap staying bounded, also with four
copies running at once on four CPUs; the leak checker must report exactly
the leaks planted in user/leakdemo.c, attributed to the functions that made
them (looked up in the ELF symbol table), and nothing on the clean variant."""
import re
import struct
from collections import Counter

import pytest

from harness import BUILD, Machine


@pytest.mark.parametrize("cpus", [1, 4])
def test_gctest(cpus, logdir):
    with Machine(cpus=cpus, log=logdir / f"gctest-{cpus}cpu.log") as m:
        m.wait_prompt(60)
        out = m.run("gctest", timeout=600)
        failed = re.findall(r"test (\S+): FAILED", out)
        assert not failed, f"failed: {failed}\n{out[-3000:]}"
        summary = re.search(r"gctest: (\d+) passed, (\d+) failed", out)
        assert summary and int(summary.group(1)) >= 13 and summary.group(2) == "0", out[-2000:]
        assert "ALL GC TESTS PASSED" in out
        assert m.poweroff() == 0


def test_gcdemo_bounded(logdir):
    with Machine(cpus=4, log=logdir / "gcdemo.log") as m:
        m.wait_prompt(60)
        out = m.run("gcdemo 400", timeout=600)
        assert "GCDEMO OK" in out, out[-2000:]
        coll = int(re.search(r"(\d+) collections, pause", out).group(1))
        assert coll >= 20, out[-2000:]
        # four collecting processes at once, migrating between the four CPUs
        out = m.run("gcdemo 150 & gcdemo 150 & gcdemo 150 & gcdemo 150 & wait", timeout=900)
        assert out.count("GCDEMO OK") == 4, out[-3000:]
        assert "UNBOUNDED" not in out and "damaged" not in out
        assert m.poweroff() == 0


def elf_functions(path):
    """{name: (start, end)} for the function symbols of an ELF64 file."""
    data = path.read_bytes()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    funcs = {}
    for sh in sections:
        if sh[1] != 2:                                  # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 24):
            name_off, info, _, _, value, size = struct.unpack_from("<IBBHQQ", data, off)
            if info & 0xF == 2 and size:                # STT_FUNC
                end = data.index(b"\0", strtab[4] + name_off)
                funcs[data[strtab[4] + name_off:end].decode()] = (value, value + size)
    return funcs


def leaks(out, funcs):
    """Counter of (size, function) for every 'leak:' line."""
    found = Counter()
    for size, pc in re.findall(r"^leak: (\d+) bytes at 0x[0-9a-f]+, allocated from pc 0x([0-9a-f]+)", out, re.M):
        pc = int(pc, 16)
        name = next((n for n, (lo, hi) in funcs.items() if lo <= pc < hi), hex(pc))
        found[(int(size), name)] += 1
    return found


PLANTED = Counter({(32, "plant_block"): 1, (48, "plant_list"): 4, (64, "plant_cycle"): 2})
SUMMARY = "leakcheck: 7 leaked blocks, 352 bytes; 6 blocks, 304 bytes still reachable"


def test_leakcheck(logdir):
    funcs = elf_functions(BUILD / "user" / "leakdemo.elf")
    assert {"plant_block", "plant_list", "plant_cycle"} <= funcs.keys()
    with Machine(cpus=2, log=logdir / "leakcheck.log") as m:
        m.wait_prompt(60)
        out = m.run("leakdemo", timeout=60)              # checker off: no report
        assert "planted 7" in out and "leak:" not in out and "leakcheck:" not in out
        out = m.run("leakcheck leakdemo", timeout=60)    # report at exit
        assert leaks(out, funcs) == PLANTED, out
        assert SUMMARY in out
        out = m.run("leakdemo now", timeout=60)          # report on demand
        assert leaks(out, funcs) == PLANTED, out
        assert SUMMARY in out
        out = m.run("leakcheck leakdemo clean", timeout=60)
        assert "leak:" not in out, out
        assert "leakcheck: no leaks; 6 blocks, 304 bytes still reachable" in out, out
        out = m.run("leakcheck ls /", timeout=60)        # any program
        assert "motd.txt" in out and "leakcheck: no leaks" in out, out
        # the flag survives fork and exec: a shell started under leakcheck
        # checks every program it runs, and itself when it exits
        m.send("leakcheck sh\n")
        m.wait_prompt(30)
        out = m.run("leakdemo", timeout=60)
        assert leaks(out, funcs) == PLANTED, out
        out = m.run("exit", timeout=60)
        assert "leakcheck: no leaks" in out, out
        out = m.run("leakcheck nosuchprogram", timeout=60)
        assert "leakcheck: nosuchprogram: no such file or directory" in out, out
        assert m.poweroff() == 0
