# Pixel Neko OS - Top-Level 64-Bit Makefile
# Standards: AMD64 Higher-Half Kernel, Limine v8.x Boot Protocol

CC ?= gcc
LD ?= ld
NASM ?= nasm

# Compiler Flags adhering to Plan Section 9.1
CFLAGS ?= -m64 -march=core2 -mcmodel=kernel -mno-red-zone \
          -mgeneral-regs-only -fno-asynchronous-unwind-tables \
          -ffreestanding -nostdlib -nostartfiles \
          -fno-builtin -fno-stack-protector -fno-pie -fno-pic \
          -std=gnu11 -O2 -Wall -Wextra \
          -Iinclude -Iarch/x86_64 -Ikernel/core -Ikernel/mm -Idrivers/video -Idrivers/audio -Idrivers/acpi

NASMFLAGS ?= -f elf64

# Source Files
ASM_SRCS = arch/x86_64/entry.asm \
           arch/x86_64/gdt_flush.asm \
           arch/x86_64/interrupts.asm

C_SRCS   = kernel/core/main.c \
           kernel/core/log.c \
           kernel/core/string.c \
           kernel/core/panic.c \
           arch/x86_64/cpu.c \
           arch/x86_64/gdt.c \
           arch/x86_64/idt.c \
           arch/x86_64/apic.c \
           kernel/mm/pmm.c \
           kernel/mm/vmm.c \
           kernel/mm/heap.c \
           kernel/mm/dma.c \
           drivers/acpi/acpi.c \
           drivers/video/boot_framebuffer.c \
           drivers/audio/speaker.c

# Object Files
ASM_OBJS = $(ASM_SRCS:.asm=.o)
C_OBJS   = $(C_SRCS:.c=.o)
OBJS     = $(ASM_OBJS) $(C_OBJS)

TARGET   = bin/kernel.elf

.PHONY: all iso clean version

all: version $(TARGET)

version:
	@bash scripts/version_gen.sh

%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -o $@ $<

%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

LDFLAGS ?= -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T link64.ld

$(TARGET): $(OBJS) link64.ld
	@mkdir -p bin
	$(LD) $(LDFLAGS) -o $(TARGET) $(OBJS)
	@echo "Kernel successfully linked: $(TARGET)"

iso: all
	@bash scripts/make_iso.sh

clean:
	rm -rf bin iso_root $(OBJS) *.iso *.log boot/limine/limine
