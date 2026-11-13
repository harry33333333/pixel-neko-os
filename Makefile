# Pixel Neko OS - Top-Level 64-Bit Makefile
# Standards: AMD64 Higher-Half Kernel, Limine v8.x Boot Protocol

CC ?= gcc
LD ?= ld
NASM ?= nasm
USER_CC ?= gcc
PYTHON3 ?= python3
PIXEL_NEKO_BUILD_NUMBER ?= 210
PIXEL_NEKO_BUILD_SUFFIX ?= a

# Compiler Flags adhering to Plan Section 9.1
CFLAGS ?= -m64 -march=core2 -mcmodel=kernel -mno-red-zone \
          -mgeneral-regs-only -fno-asynchronous-unwind-tables \
          -ffreestanding -nostdlib -nostartfiles \
          -fno-builtin -fno-stack-protector -fno-pie -fno-pic \
          -std=gnu11 -O2 -Wall -Wextra \
          -DUACPI_USE_BUILTIN_STRING \
          -DUACPI_PLAIN_LOG_BUFFER_SIZE=256 \
          -DUACPI_STATIC_TABLE_ARRAY_LEN=32 \
          -Iinclude -Iarch/x86_64 -Ikernel/core -Ikernel/mm -Iwm -Iapps \
          -Idrivers/video -Idrivers/audio -Idrivers/acpi -Idrivers/storage \
          -Idrivers/acpi/uacpi/include -Isdk/ddk/include

NASMFLAGS ?= -f elf64

# Source Files
UACPI_SRCS = drivers/acpi/uacpi/source/tables.c \
             drivers/acpi/uacpi/source/types.c \
             drivers/acpi/uacpi/source/uacpi.c \
             drivers/acpi/uacpi/source/utilities.c \
             drivers/acpi/uacpi/source/interpreter.c \
             drivers/acpi/uacpi/source/opcodes.c \
             drivers/acpi/uacpi/source/namespace.c \
             drivers/acpi/uacpi/source/stdlib.c \
             drivers/acpi/uacpi/source/opregion.c \
             drivers/acpi/uacpi/source/default_handlers.c \
             drivers/acpi/uacpi/source/io.c \
             drivers/acpi/uacpi/source/notify.c \
             drivers/acpi/uacpi/source/sleep.c \
             drivers/acpi/uacpi/source/registers.c \
             drivers/acpi/uacpi/source/resources.c \
             drivers/acpi/uacpi/source/event.c \
             drivers/acpi/uacpi/source/mutex.c \
             drivers/acpi/uacpi/source/osi.c

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
           kernel/process/pxe.c \
           kernel/splash.c \
           kernel/driver.c \
           kernel/pdv.c \
           kernel/ui_compat.c \
           kernel/ui_timer.c \
           kernel/ui_settings.c \
           kernel/ui_power.c \
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
           drivers/storage/isadma.c \
           drivers/storage/storage.c \
           fs/part.c \
           fs/fat.c \
           fs/exfat.c \
           fs/iso9660.c \
           fs/format.c \
           fs/vfs.c \
           fs/p5_test.c \
           drivers/acpi/acpi.c \
           drivers/acpi/uacpi_glue.c \
           drivers/video/boot_framebuffer.c \
           drivers/video/gfx_accel.c \
           drivers/audio/speaker.c \
           drivers/audio/wav_dec.c \
           drivers/audio/flac_dec.c \
           drivers/audio/mp3_dec.c \
           drivers/audio/audio.c \
           wm/font.c \
           wm/wallpaper.c \
           wm/image_dec.c \
           wm/wm.c \
           apps/calc.c \
           apps/cpanel.c \
           apps/editor.c \
           apps/filemanager.c \
           apps/sysinfo.c \
           apps/puzzle.c \
           apps/imageviewer.c \
           apps/audioplayer.c \
           apps/installer.c

USER_CFLAGS ?= -m64 -march=core2 -msse2 -mno-red-zone \
               -ffreestanding -nostdlib -nostartfiles \
               -fno-builtin -fno-stack-protector -fno-pie -fno-pic \
               -std=gnu11 -O2 -Wall -Wextra -Iinclude

USER_OBJ_DIR = build/userland
USER_OBJS = $(USER_OBJ_DIR)/start.o \
            $(USER_OBJ_DIR)/libneko.o \
            $(USER_OBJ_DIR)/init.o

# Standalone Loadable Drivers (*.pdv in PDEVICE\)
PDV_CFLAGS ?= -m64 -march=core2 -mcmodel=kernel -mno-red-zone \
              -mgeneral-regs-only -fno-asynchronous-unwind-tables \
              -ffreestanding -nostdlib -nostartfiles \
              -fno-builtin -fno-stack-protector -fno-pie -fno-pic \
              -std=gnu11 -O2 -Wall -Wextra -Isdk/ddk/include

PDV_DRIVERS = vmwgfx mach64 nvg7x fdc ac97 ct4810 cmi8738 hdaudio
PDV_OUT_DIR = build/pdevice
PDV_TARGETS = $(patsubst %,$(PDV_OUT_DIR)/%.pdv,$(PDV_DRIVERS))

# Native PXE SDK & Example Executables (*.pxe in DEMO\)
PXE_OUT_DIR = build/pxe_examples
PXE_EXAMPLES = $(PXE_OUT_DIR)/hello.pxe $(PXE_OUT_DIR)/mini_taskmon.pxe
PXE_SDK_STAMP = build/pxe_sdk.stamp
PXE_SDK_DEPS = sdk/pxe_app.mk \
               include/neko_abi.h \
               sdk/crt/crt0_r3.asm sdk/crt/crt0_r0.asm sdk/crt/pxe_pie.ld \
               sdk/libpxe/runtime_r3.c sdk/libpxe/runtime_r0.c \
               sdk/libpxe/gfx.c sdk/libpxe/str.c \
               $(wildcard sdk/include/pxe/*.h)

# Object Files
ASM_OBJS = $(ASM_SRCS:.asm=.o)
C_OBJS   = $(C_SRCS:.c=.o) $(UACPI_SRCS:.c=.o)
USER_EMBED_OBJ = bin/userland/init_elf.o
OBJS     = $(ASM_OBJS) $(C_OBJS) $(USER_EMBED_OBJ)

TARGET   = bin/kernel.elf

.PHONY: all pdv pxe-sdk pxe-examples iso clean version

all: version $(TARGET) $(PDV_TARGETS) $(PXE_EXAMPLES)

pdv: $(PDV_TARGETS)

pxe-sdk: $(PXE_SDK_STAMP)

pxe-examples: $(PXE_EXAMPLES)

$(PXE_SDK_STAMP): $(PXE_SDK_DEPS)
	@mkdir -p build
	$(MAKE) -C sdk/examples/hello_gui sdk_libs
	@touch $@

$(PXE_OUT_DIR)/hello.pxe: $(PXE_SDK_STAMP) sdk/tools/mkpxe.py \
                          sdk/examples/hello_gui/hello.c \
                          sdk/examples/hello_gui/hello.pxi \
                          sdk/examples/hello_gui/Makefile
	$(MAKE) -C sdk/examples/hello_gui \
		OBJ_DIR="$(abspath $(PXE_OUT_DIR)/obj_hello)" \
		OUT_DIR="$(abspath $(PXE_OUT_DIR))"

$(PXE_OUT_DIR)/mini_taskmon.pxe: $(PXE_SDK_STAMP) sdk/tools/mkpxe.py \
                                 sdk/examples/mini_taskmon/mini_taskmon.c \
                                 sdk/examples/mini_taskmon/mini_taskmon.pxi \
                                 sdk/examples/mini_taskmon/Makefile
	$(MAKE) -C sdk/examples/mini_taskmon \
		OBJ_DIR="$(abspath $(PXE_OUT_DIR)/obj_mini_taskmon)" \
		OUT_DIR="$(abspath $(PXE_OUT_DIR))"

version:
	@PIXEL_NEKO_BUILD_NUMBER=$(PIXEL_NEKO_BUILD_NUMBER) PIXEL_NEKO_BUILD_SUFFIX=$(PIXEL_NEKO_BUILD_SUFFIX) bash scripts/version_gen.sh

%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) -o $@ $<

drivers/audio/mp3_dec.o: drivers/audio/mp3_dec.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -msse -msse2 -fno-tree-vectorize -mstackrealign -c -o $@ $<

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

define BUILD_PDV_DRIVER
$$(PDV_OUT_DIR)/$(1).o: drivers/pdv/$(1)/$(1).c sdk/ddk/include/pdv/ddk.h sdk/ddk/include/pdv/format.h
	@mkdir -p $$(dir $$@)
	$$(CC) $$(PDV_CFLAGS) -c -o $$@ $$<

$$(PDV_OUT_DIR)/$(1).rel.o: $$(PDV_OUT_DIR)/$(1).o
	@mkdir -p $$(dir $$@)
	$$(LD) -m elf_x86_64 -r -o $$@ $$<

$$(PDV_OUT_DIR)/$(1).pdv: drivers/pdv/$(1)/$(1).pdi $$(PDV_OUT_DIR)/$(1).rel.o scripts/mkpdv.py
	@mkdir -p $$(dir $$@)
	$$(PYTHON3) scripts/mkpdv.py --manifest drivers/pdv/$(1)/$(1).pdi --object $$(PDV_OUT_DIR)/$(1).rel.o --output $$@
endef

$(foreach drv,$(PDV_DRIVERS),$(eval $(call BUILD_PDV_DRIVER,$(drv))))

LDFLAGS ?= -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 -T link64.ld

$(TARGET): $(OBJS) link64.ld
	@mkdir -p bin
	$(LD) $(LDFLAGS) -o $(TARGET) $(OBJS)
	@echo "Kernel successfully linked: $(TARGET)"

iso: all
	@bash scripts/make_iso.sh

clean:
	rm -rf bin build iso_root $(OBJS) *.iso *.log boot/limine/limine sdk/lib sdk/crt/*.o
