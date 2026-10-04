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

.PHONY: all kernel clean qemu
.SECONDARY:

all: kernel

kernel: $(B)/Image

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

# The raw Image is what both QEMU (-kernel) and LeapVM (-k) load.
$(B)/Image: $(B)/kernel.elf
	$(OBJCOPY) -O binary $< $@

CPUS ?= 4
MEM  ?= 256M
QEMU_MACHINE := -machine virt,gic-version=3 -cpu cortex-a72 -smp $(CPUS) -m $(MEM)

qemu: $(B)/Image
	$(QEMU) $(QEMU_MACHINE) -nographic -kernel $(B)/Image

clean:
	rm -rf $(B)

-include $(shell find $(B) -name '*.d' 2>/dev/null)
