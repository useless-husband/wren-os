# wren-os -- run every target from the repository root.
#
#   make                build the kernel Image, the user programs, the disk image and host tools
#   make qemu           boot to the shell under QEMU (CPUS=4, VIRTIO=legacy|modern, ACCEL=tcg|hvf)
#   make leapvm         boot to the shell under LeapVM (macOS; LEAPVM=path/to/leapvm)
#   make test           unit tests + all system tests (what CI runs)
#   make unit           host unit tests only (FDT parser, buddy allocator, ELF checks, lib)
#   make system         QEMU system tests (pytest): boot, shell, usertests on 1 and 4 CPUs, ...
#   make crash          crash-consistency test, 300 seeded power cuts (CRASH_RUNS=...)
#   make stress         60 s SMP stress on 4 CPUs
#   make mutants        check that 8 deliberate bugs are each caught by a test
#   make bench          micro-benchmarks (ACCEL=tcg|hvf, or HV=leapvm)
#   make lint           strict warnings, clang static analyzer, Python syntax
#
# Every path is relative on purpose: the checkout may live in a directory
# whose name contains spaces or non-ASCII characters, which make cannot
# handle in absolute paths.

SHELL := /bin/bash

# Cross toolchain: clang + lld with the aarch64 ELF target.  Homebrew's LLVM
# is preferred on macOS (Apple's clang cannot link ELF).
LLVM_BIN := $(if $(wildcard /opt/homebrew/opt/llvm/bin/clang),/opt/homebrew/opt/llvm/bin/,)
CLANG    ?= $(LLVM_BIN)clang
LLD      ?= $(if $(wildcard /opt/homebrew/opt/lld/bin/ld.lld),/opt/homebrew/opt/lld/bin/ld.lld,ld.lld)
OBJCOPY  ?= $(LLVM_BIN)llvm-objcopy
HOSTCC   ?= cc
QEMU     ?= qemu-system-aarch64
VENV     ?= .venv
PYTHON   ?= $(if $(wildcard $(VENV)/bin/python),$(VENV)/bin/python,python3)
V        ?= 0
Q        := $(if $(filter 1,$(V)),,@)

TARGET   := --target=aarch64-none-elf -march=armv8-a
WARN     := -Wall -Wextra -Werror -Wno-unused-parameter
COMMON   := $(TARGET) -std=c11 -O2 -g -ffreestanding -fno-builtin -nostdlib -nostdlibinc \
            -fno-pic -fno-pie -fno-stack-protector -fno-omit-frame-pointer $(WARN) -MMD -MP
KCFLAGS  := $(COMMON) -mgeneral-regs-only -mno-omit-leaf-frame-pointer -Ikernel -Iinclude
KASFLAGS := $(TARGET) -g -Ikernel -Iinclude -D__ASSEMBLER__ -MMD -MP
UCFLAGS  := $(COMMON) -Iinclude -Iuser

B := build

KERNEL_C := $(wildcard kernel/*.c)
KERNEL_S := $(wildcard kernel/*.S)
KLIB_C   := lib/string.c lib/fmt.c lib/crc32c.c
KOBJS    := $(KERNEL_S:kernel/%.S=$(B)/kernel/%.S.o) $(KERNEL_C:kernel/%.c=$(B)/kernel/%.o) \
            $(KLIB_C:lib/%.c=$(B)/klib/%.o)

UPROGS   := init sh echo cat ls wc grep mkdir rm ln kill ps sleep poweroff usertests fswork stress bench
ULIB     := $(B)/user/lib/crt0.o $(B)/user/lib/syscalls.o $(B)/user/lib/ulib.o \
            $(B)/user/klib/string.o $(B)/user/klib/fmt.o
UELFS    := $(UPROGS:%=$(B)/user/%.elf)
UBINS    := $(UPROGS:%=$(B)/user/bin/%)

FS_BLOCKS ?= 16384
FS_INODES ?= 1024

.PHONY: all kernel user fs tools clean qemu leapvm test unit system crash stress mutants bench lint venv
.PRECIOUS: $(B)/%.o $(B)/user/%.o $(B)/user/%.elf

all: kernel fs tools

kernel: $(B)/Image
user: $(UELFS)
fs: $(B)/fs.img
tools: $(B)/host/mkfs $(B)/host/fsck

# ------------------------------------------------------------------ kernel
$(B)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	@echo "  CC      $<"
	$(Q)$(CLANG) $(KCFLAGS) -c -o $@ $<

$(B)/kernel/%.S.o: kernel/%.S
	@mkdir -p $(@D)
	@echo "  AS      $<"
	$(Q)$(CLANG) $(KASFLAGS) -c -o $@ $<

$(B)/klib/%.o: lib/%.c
	@mkdir -p $(@D)
	@echo "  CC      $< (kernel)"
	$(Q)$(CLANG) $(KCFLAGS) -c -o $@ $<

$(B)/kernel.elf: $(KOBJS) kernel/kernel.ld
	@echo "  LD      $@"
	$(Q)$(LLD) -T kernel/kernel.ld --no-pie -z noexecstack -o $@ $(KOBJS)

# The raw Image is what both QEMU (-kernel) and LeapVM (-k) load.
$(B)/Image: $(B)/kernel.elf
	@echo "  IMAGE   $@"
	$(Q)$(OBJCOPY) -O binary $< $@

# The first process's code is a user program embedded in the kernel image.
$(B)/kernel/initcode.S.o: $(B)/user/initcode.bin

$(B)/user/initcode.bin: user/initcode.S user/user.ld
	@mkdir -p $(@D)
	$(Q)$(CLANG) $(KASFLAGS) -c -o $(B)/user/initcode.o $<
	$(Q)$(LLD) -T user/user.ld -o $(B)/user/initcode.elf $(B)/user/initcode.o
	$(Q)$(OBJCOPY) -O binary -j .text $(B)/user/initcode.elf $@

# -------------------------------------------------------------- user space
$(B)/user/lib/%.o: user/lib/%.S
	@mkdir -p $(@D)
	$(Q)$(CLANG) $(KASFLAGS) -c -o $@ $<

$(B)/user/lib/%.o: user/lib/%.c
	@mkdir -p $(@D)
	@echo "  CC      $<"
	$(Q)$(CLANG) $(UCFLAGS) -c -o $@ $<

$(B)/user/klib/%.o: lib/%.c
	@mkdir -p $(@D)
	$(Q)$(CLANG) $(UCFLAGS) -c -o $@ $<

$(B)/user/%.o: user/%.c
	@mkdir -p $(@D)
	@echo "  CC      $<"
	$(Q)$(CLANG) $(UCFLAGS) -c -o $@ $<

$(B)/user/%.elf: $(B)/user/%.o $(ULIB) user/user.ld
	$(Q)$(LLD) -T user/user.ld -o $@ $(ULIB) $<

# What goes on the disk: the same ELF without debug sections.
$(B)/user/bin/%: $(B)/user/%.elf
	@mkdir -p $(@D)
	$(Q)$(OBJCOPY) --strip-debug $< $@

# -------------------------------------------------------------- host tools
HOSTCFLAGS := -std=c11 -O2 -g -Wall -Wextra -Iinclude

$(B)/host/mkfs: tools/mkfs.c include/wren/fsformat.h
	@mkdir -p $(@D)
	@echo "  HOSTCC  $<"
	$(Q)$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/mkfs.c

$(B)/host/fsck: tools/fsck.c lib/crc32c.c include/wren/fsformat.h
	@mkdir -p $(@D)
	@echo "  HOSTCC  $<"
	$(Q)$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/fsck.c lib/crc32c.c

$(B)/fs.img: $(B)/host/mkfs $(UBINS) user/files/motd.txt
	@echo "  MKFS    $@"
	$(Q)$(B)/host/mkfs -o $@ -s $(FS_BLOCKS) -i $(FS_INODES) /motd.txt=user/files/motd.txt \
	  $(foreach p,$(UPROGS),/bin/$(p)=$(B)/user/bin/$(p)) >/dev/null

# -------------------------------------------------------------- running
CPUS   ?= 4
MEM    ?= 256M
ACCEL  ?= tcg
VIRTIO ?= legacy
QEMU_CPU := $(if $(filter hvf,$(ACCEL)),host,cortex-a72)
QEMU_MACHINE := -machine virt,gic-version=3 -accel $(ACCEL) -cpu $(QEMU_CPU) -smp $(CPUS) -m $(MEM)
# VIRTIO=modern selects the virtio-mmio version 2 transport (LeapVM only has that one).
QEMU_VIRTIO := $(if $(filter modern,$(VIRTIO)),-global virtio-mmio.force-legacy=false,)
QEMU_DISK = -drive file=$(B)/disk.img,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0 $(QEMU_VIRTIO)

# Interactive sessions use their own copy of the disk, so files you create
# survive reboots while build/fs.img stays pristine for the tests.  A newer
# fs.img (new programs) replaces it.
$(B)/disk.img: $(B)/fs.img
	cp $< $@

qemu: all $(B)/disk.img
	@echo "wren-os under QEMU: quit with Ctrl-A then x"
	$(QEMU) $(QEMU_MACHINE) -nographic -kernel $(B)/Image $(QEMU_DISK)

# Default: the LeapVM checkout in a project folder on the Desktop; override with LEAPVM=...
LEAPVM ?= $(shell ls -d "$$HOME"/Desktop/*/*LeapVM/leapvm 2>/dev/null | head -1)
leapvm: all $(B)/disk.img
	@echo "wren-os under LeapVM: quit with Ctrl-A then x"
	"$(LEAPVM)" -k $(B)/Image -c $(CPUS) -m $(patsubst %M,%,$(MEM)) --no-net --disk $(B)/disk.img

# ------------------------------------------------------------------ tests
# AddressSanitizer + UBSan on Linux (CI).  On macOS 27 the ASan runtime hangs
# at startup even for an empty program, so there the unit tests use UBSan only.
comma      := ,
SANITIZE   ?= $(if $(filter Darwin,$(shell uname -s)),undefined,address$(comma)undefined)
UNITFLAGS  := -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Iinclude -fno-omit-frame-pointer \
              -fsanitize=$(SANITIZE) -fno-sanitize-recover=undefined
UNIT_BINS  := $(B)/host/test_lib $(B)/host/test_fdt $(B)/host/test_buddy $(B)/host/test_elf

$(B)/host/test_lib: tests/unit/test_lib.c lib/crc32c.c lib/fmt.c
$(B)/host/test_fdt: tests/unit/test_fdt.c kernel/fdt.c kernel/platform.c lib/fmt.c
$(B)/host/test_buddy: tests/unit/test_buddy.c kernel/buddy.c lib/fmt.c
$(B)/host/test_elf: tests/unit/test_elf.c kernel/elf.c
$(UNIT_BINS): tests/unit/check.h
	@mkdir -p $(@D)
	@echo "  HOSTCC  $@ ($(SANITIZE) sanitizer)"
	$(Q)$(HOSTCC) $(UNITFLAGS) -o $@ $(filter %.c,$^)

# QEMU's own device tree for the machine we test on: generated, not committed.
$(B)/qemu-virt.dtb:
	@mkdir -p $(@D)
	$(Q)$(QEMU) -machine virt,gic-version=3,dumpdtb=$@ -cpu cortex-a72 -smp 4 -m 256M -display none >/dev/null

unit: $(UNIT_BINS) $(B)/qemu-virt.dtb $(UELFS)
	$(B)/host/test_lib
	$(B)/host/test_fdt $(B)/qemu-virt.dtb
	$(B)/host/test_buddy
	$(B)/host/test_elf $(UELFS)

venv: $(VENV)/bin/python
$(VENV)/bin/python: requirements-dev.txt
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install -q -r requirements-dev.txt
	@touch $@

system: all
	WREN_SKIP_BUILD=1 $(PYTHON) -m pytest -v tests

test: unit system

CRASH_RUNS ?= 300
crash: all
	$(PYTHON) tests/crash.py --runs $(CRASH_RUNS) --jobs 4

stress: all
	$(PYTHON) tests/harness.py --cpus 4 --expect "STRESS OK" --fsck --timeout 600 "stress 60"

mutants: all
	$(PYTHON) tests/mutants.py

HV ?= qemu
bench: all
	$(PYTHON) tests/bench.py --hypervisor $(HV) --accel $(ACCEL)

LINTWARN := -Wshadow -Wpointer-arith -Wundef -Wvla -Wformat=2 -Wnull-dereference \
            -Wmissing-prototypes -Wstrict-prototypes -Wimplicit-fallthrough -Wunreachable-code
lint: all
	$(Q)for f in $(KERNEL_C) $(KLIB_C); do \
	  $(CLANG) $(filter-out -MMD -MP,$(KCFLAGS)) $(LINTWARN) -fsyntax-only $$f || exit 1; done
	$(Q)for f in user/*.c user/lib/*.c; do \
	  $(CLANG) $(filter-out -MMD -MP,$(UCFLAGS)) $(LINTWARN) -fsyntax-only $$f || exit 1; done
	$(Q)rm -f $(B)/analyzer.txt; for f in $(KERNEL_C); do \
	  $(CLANG) --analyze $(filter-out -MMD -MP,$(KCFLAGS)) -Xclang -analyzer-output=text $$f -o /dev/null 2>>$(B)/analyzer.txt; done; \
	  if grep -E "warning|error" $(B)/analyzer.txt; then exit 1; fi
	$(Q)$(PYTHON) -m py_compile tests/*.py
	@echo "lint: clean (strict warnings, static analyzer)"

clean:
	rm -rf $(B)

-include $(shell find $(B) -name '*.d' 2>/dev/null)
