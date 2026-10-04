# wren-os -- run every target from the repository root.
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

TARGET   := --target=aarch64-none-elf -march=armv8-a
WARN     := -Wall -Wextra -Werror -Wno-unused-parameter
COMMON   := $(TARGET) -std=c11 -O2 -g -ffreestanding -fno-builtin -nostdlib -nostdlibinc \
            -fno-pic -fno-pie -fno-stack-protector -fno-omit-frame-pointer $(WARN) -MMD -MP
KCFLAGS  := $(COMMON) -mgeneral-regs-only -mno-omit-leaf-frame-pointer -Ikernel -Iinclude
KASFLAGS := $(TARGET) -g -Ikernel -Iinclude -D__ASSEMBLER__ -MMD -MP

B := build

KERNEL_C := $(wildcard kernel/*.c)
KERNEL_S := $(wildcard kernel/*.S)
KLIB_C   := lib/string.c lib/fmt.c lib/crc32c.c
KOBJS    := $(KERNEL_S:kernel/%.S=$(B)/kernel/%.S.o) $(KERNEL_C:kernel/%.c=$(B)/kernel/%.o) \
            $(KLIB_C:lib/%.c=$(B)/klib/%.o)

UCFLAGS  := $(COMMON) -Iinclude -Iuser
UPROGS   := init sh echo cat ls wc grep mkdir rm ln kill ps sleep poweroff usertests
ULIB     := $(B)/user/lib/crt0.o $(B)/user/lib/syscalls.o $(B)/user/lib/ulib.o \
            $(B)/user/klib/string.o $(B)/user/klib/fmt.o
UELFS    := $(UPROGS:%=$(B)/user/%.elf)

FS_BLOCKS ?= 16384
FS_INODES ?= 1024

.PHONY: all kernel user fs tools clean qemu
.SECONDARY:

all: kernel fs

kernel: $(B)/Image
user: $(UELFS)
fs: $(B)/fs.img
tools: $(B)/host/mkfs

$(B)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(CLANG) $(KCFLAGS) -c -o $@ $<

$(B)/kernel/%.S.o: kernel/%.S
	@mkdir -p $(@D)
	$(CLANG) $(KASFLAGS) -c -o $@ $<

$(B)/klib/%.o: lib/%.c
	@mkdir -p $(@D)
	$(CLANG) $(KCFLAGS) -c -o $@ $<

$(B)/kernel.elf: $(KOBJS) kernel/kernel.ld
	$(LLD) -T kernel/kernel.ld --no-pie -z noexecstack -o $@ $(KOBJS)

# The first process's code is a user program embedded in the kernel image.
$(B)/kernel/initcode.S.o: $(B)/user/initcode.bin

$(B)/user/initcode.bin: user/initcode.S user/user.ld
	@mkdir -p $(@D)
	$(CLANG) $(KASFLAGS) -c -o $(B)/user/initcode.o $<
	$(LLD) -T user/user.ld -o $(B)/user/initcode.elf $(B)/user/initcode.o
	$(OBJCOPY) -O binary -j .text $(B)/user/initcode.elf $@

# ---------------------------------------------------------------- user space
$(B)/user/lib/%.o: user/lib/%.S
	@mkdir -p $(@D)
	$(CLANG) $(KASFLAGS) -c -o $@ $<

$(B)/user/lib/%.o: user/lib/%.c
	@mkdir -p $(@D)
	$(CLANG) $(UCFLAGS) -c -o $@ $<

$(B)/user/klib/%.o: lib/%.c
	@mkdir -p $(@D)
	$(CLANG) $(UCFLAGS) -c -o $@ $<

$(B)/user/%.o: user/%.c
	@mkdir -p $(@D)
	$(CLANG) $(UCFLAGS) -c -o $@ $<

$(B)/user/%.elf: $(B)/user/%.o $(ULIB) user/user.ld
	$(LLD) -T user/user.ld -o $@ $(ULIB) $<

# What goes on the disk: the same ELF without debug sections.
$(B)/user/bin/%: $(B)/user/%.elf
	@mkdir -p $(@D)
	$(OBJCOPY) --strip-debug $< $@

# ---------------------------------------------------------------- host tools
HOSTCFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude

$(B)/host/mkfs: tools/mkfs.c include/wren/fsformat.h
	@mkdir -p $(@D)
	$(HOSTCC) $(HOSTCFLAGS) -o $@ tools/mkfs.c

UBINS := $(UPROGS:%=$(B)/user/bin/%)

$(B)/fs.img: $(B)/host/mkfs $(UBINS) user/files/motd.txt
	$(B)/host/mkfs -o $@ -s $(FS_BLOCKS) -i $(FS_INODES) /motd.txt=user/files/motd.txt \
	  $(foreach p,$(UPROGS),/bin/$(p)=$(B)/user/bin/$(p))

# The raw Image is what both QEMU (-kernel) and LeapVM (-k) load.
$(B)/Image: $(B)/kernel.elf
	$(OBJCOPY) -O binary $< $@

CPUS ?= 4
MEM  ?= 256M
QEMU_MACHINE := -machine virt,gic-version=3 -cpu cortex-a72 -smp $(CPUS) -m $(MEM)

# VIRTIO=modern selects the virtio-mmio version 2 transport (LeapVM only has that one).
VIRTIO ?= legacy
QEMU_VIRTIO := $(if $(filter modern,$(VIRTIO)),-global virtio-mmio.force-legacy=false,)
QEMU_DISK = -drive file=$(B)/fs.img,if=none,format=raw,id=d0 -device virtio-blk-device,drive=d0 $(QEMU_VIRTIO)

qemu: $(B)/Image $(B)/fs.img
	$(QEMU) $(QEMU_MACHINE) -nographic -kernel $(B)/Image $(QEMU_DISK)

clean:
	rm -rf $(B)

-include $(shell find $(B) -name '*.d' 2>/dev/null)
