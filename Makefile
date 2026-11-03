# Pixel Neko OS - Top-Level 64-Bit Makefile
# Standards: AMD64 Higher-Half Kernel, Limine v8.x Boot Protocol

CC ?= gcc
LD ?= ld
NASM ?= nasm
USER_CC ?= gcc
PIXEL_NEKO_BUILD_NUMBER ?= 140

# Compiler Flags adhering to Plan Section 9.1
CFLAGS ?= -m64 -march=core2 -mcmodel=kernel -mno-red-zone \
          -mgeneral-regs-only -fno-asynchronous-unwind-tables \
          -ffreestanding -nostdlib -nostartfiles \
          -fno-builtin -fno-stack-protector -fno-pie -fno-pic \
          -std=gnu11 -O2 -Wall -Wextra \
          -Iinclude -Iarch/x86_64 -Ikernel/core -Ikernel/mm -Idrivers/video -Idrivers/audio -Idrivers/acpi -Idrivers/storage

NASMFLAGS ?= -f elf64

# Source Files
ASM_SRCS = arch/x86_64/entry.asm \
           arch/x86_64/gdt_flush.asm \
           arch/x86_64/interrupts.asm \
           arch/x86_64/fpu_asm.asm \
           arch/x86_64/percpu_asm.asm \
           arch/x86_64/syscall_entry.asm \
           arch/x86_64/nmi.asm

C_SRCS   = kernel/core/main.c \
           kernel/core/log.c \
           kernel/core/string.c \
           kernel/core/panic.c \
           arch/x86_64/cpu.c \
           arch/x86_64/gdt.c \
           arch/x86_64/idt.c \
           arch/x86_64/apic.c \
           arch/x86_64/fpu.c \
           arch/x86_64/percpu.c \
           arch/x86_64/syscall.c \
           kernel/mm/pmm.c \
           kernel/mm/vmm.c \
           kernel/mm/heap.c \
           kernel/mm/dma.c \
           kernel/process/process.c \
           kernel/process/scheduler.c \
           kernel/process/elf.c \
           kernel/process/ipc.c \
           drivers/bus/device.c \
           drivers/bus/pci.c \
           drivers/input/input_manager.c \
           drivers/input/keyboard.c \
           drivers/input/mouse.c \
           drivers/usb/compat/usb_compat.c \
           drivers/usb/core/usb_core.c \
           drivers/usb/core/usb_hub.c \
           drivers/usb/core/usb_hid.c \
           drivers/usb/storage/usb_bot.c \
           drivers/usb/hcd/usb_uhci.c \
           drivers/usb/hcd/usb_ohci.c \
           drivers/usb/hcd/usb_ehci.c \
           drivers/usb/hcd/usb_xhci.c \
           drivers/storage/block.c \
           drivers/storage/ahci.c \
           drivers/storage/ata.c \
           drivers/storage/storage.c \
           fs/part.c \
           fs/fat.c \
           fs/exfat.c \
           fs/iso9660.c \
           fs/format.c \
           fs/vfs.c \
           fs/p5_test.c \
           drivers/acpi/acpi.c \
           drivers/video/boot_framebuffer.c \
           drivers/audio/speaker.c

USER_CFLAGS ?= -m64 -march=core2 -msse2 -mno-red-zone \
               -ffreestanding -nostdlib -nostartfiles \
               -fno-builtin -fno-stack-protector -fno-pie -fno-pic \
               -std=gnu11 -O2 -Wall -Wextra -Iinclude

USER_OBJ_DIR = build/userland
USER_OBJS = $(USER_OBJ_DIR)/start.o \
            $(USER_OBJ_DIR)/libneko.o \
            $(USER_OBJ_DIR)/init.o

# Object Files
ASM_OBJS = $(ASM_SRCS:.asm=.o)
C_OBJS   = $(C_SRCS:.c=.o)
USER_EMBED_OBJ = bin/userland/init_elf.o
OBJS     = $(ASM_OBJS) $(C_OBJS) $(USER_EMBED_OBJ)

TARGET   = bin/kernel.elf

.PHONY: all iso clean version

all: version $(TARGET)

version:
	@PIXEL_NEKO_BUILD_NUMBER=$(PIXEL_NEKO_BUILD_NUMBER) bash scripts/version_gen.sh

%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -o $@ $<

%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(USER_OBJ_DIR)/start.o: userland/start.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -o $@ $<

$(USER_OBJ_DIR)/%.o: userland/%.c
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c -o $@ $<

bin/init.elf: $(USER_OBJS) userland/user.ld
	@mkdir -p bin
	$(LD) -m elf_x86_64 -nostdlib -static --build-id=none -z noexecstack \
		-T userland/user.ld -o $@ $(USER_OBJS)

$(USER_EMBED_OBJ): bin/init.elf
	@mkdir -p $(dir $@)
	$(LD) -r -b binary -o $@ $<

LDFLAGS ?= -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T link64.ld

$(TARGET): $(OBJS) link64.ld
	@mkdir -p bin
	$(LD) $(LDFLAGS) -o $(TARGET) $(OBJS)
	@echo "Kernel successfully linked: $(TARGET)"

iso: all
	@bash scripts/make_iso.sh

clean:
	rm -rf bin build iso_root $(OBJS) *.iso *.log boot/limine/limine
