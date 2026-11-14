# Pixel Neko OS Native Application SDK (v1.0)
# Usage:
#   APP_NAME  := myapp
#   APP_SRCS  := main.c
#   APP_PXI   := myapp.pxi
#   PXE_RING  := 3        # 3 (Userland, default) or 0 (Ring 0 Privileged)
#   SDK_DIR   := /path/to/pixel-neko-os/sdk
#   include $(SDK_DIR)/pxe_app.mk

CC      ?= gcc
LD      ?= ld
AR      ?= ar
NASM    ?= nasm
PYTHON3 ?= python3

SDK_DIR  := $(or $(SDK_DIR),$(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST)))))
MKPXE    ?= $(SDK_DIR)/tools/mkpxe.py

PXE_RING ?= 3
APP_PXI  ?= $(APP_NAME).pxi
OUT_DIR  ?= build
OBJ_DIR  ?= $(OUT_DIR)/obj_$(APP_NAME)
APP_ASM_SRCS ?=
EXTRA_CFLAGS ?=

PXE_COMMON_CFLAGS := -m64 -march=core2 -mno-red-zone \
                     -fno-asynchronous-unwind-tables \
                     -fPIE -fno-plt -fvisibility=hidden \
                     -ffreestanding -nostdlib -nostartfiles \
                     -fno-builtin -fno-stack-protector \
                     -std=gnu11 -O2 -Wall -Wextra -I$(SDK_DIR)/include

PXE_R3_CFLAGS := $(PXE_COMMON_CFLAGS) -msse2
PXE_R0_CFLAGS := $(PXE_COMMON_CFLAGS) -mgeneral-regs-only

ifeq ($(PXE_RING),0)
PXE_TARGET_CFLAGS := $(PXE_R0_CFLAGS)
PXE_ENTRY         := _start_r0
PXE_CRT0          := $(SDK_DIR)/crt/crt0_r0.o
PXE_LIB           := $(SDK_DIR)/lib/libpxe_r0.a
else
PXE_TARGET_CFLAGS := $(PXE_R3_CFLAGS)
PXE_ENTRY         := _start
PXE_CRT0          := $(SDK_DIR)/crt/crt0_r3.o
PXE_LIB           := $(SDK_DIR)/lib/libpxe_r3.a
endif

PXE_LDFLAGS := -m elf_x86_64 -nostdlib -pie -Bsymbolic --no-dynamic-linker \
               --build-id=none -z max-page-size=0x1000 -z norelro -z noexecstack \
               -T $(SDK_DIR)/crt/pxe_pie.ld -e $(PXE_ENTRY)

PXE_HEADERS := $(SDK_DIR)/include/pxe/abi.h \
               $(SDK_DIR)/include/pxe/audio.h \
               $(SDK_DIR)/include/pxe/format.h \
               $(SDK_DIR)/include/pxe/fs.h \
               $(SDK_DIR)/include/pxe/gfx.h \
               $(SDK_DIR)/include/pxe/gui.h \
               $(SDK_DIR)/include/pxe/mem.h \
               $(SDK_DIR)/include/pxe/ring0.h \
               $(SDK_DIR)/include/pxe/str.h \
               $(SDK_DIR)/include/pxe/sys.h

PXE_R3_OBJS := $(SDK_DIR)/lib/obj_r3/runtime_r3.o \
               $(SDK_DIR)/lib/obj_r3/gfx.o \
               $(SDK_DIR)/lib/obj_r3/str.o

PXE_R0_OBJS := $(SDK_DIR)/lib/obj_r0/runtime_r0.o \
               $(SDK_DIR)/lib/obj_r0/gfx.o \
               $(SDK_DIR)/lib/obj_r0/str.o

APP_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(APP_SRCS)) \
            $(patsubst %.asm,$(OBJ_DIR)/%.o,$(APP_ASM_SRCS))

APP_ELF := $(OBJ_DIR)/$(APP_NAME).elf
APP_OUT ?= $(OUT_DIR)/$(APP_NAME).pxe

.PHONY: all clean sdk_libs

all: $(APP_OUT)

sdk_libs: $(SDK_DIR)/crt/crt0_r3.o $(SDK_DIR)/crt/crt0_r0.o \
          $(SDK_DIR)/lib/libpxe_r3.a $(SDK_DIR)/lib/libpxe_r0.a

$(SDK_DIR)/crt/crt0_r3.o: $(SDK_DIR)/crt/crt0_r3.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

$(SDK_DIR)/crt/crt0_r0.o: $(SDK_DIR)/crt/crt0_r0.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

$(SDK_DIR)/lib/obj_r3/%.o: $(SDK_DIR)/libpxe/%.c $(PXE_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(PXE_R3_CFLAGS) -c $< -o $@

$(SDK_DIR)/lib/obj_r0/%.o: $(SDK_DIR)/libpxe/%.c $(PXE_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(PXE_R0_CFLAGS) -c $< -o $@

$(SDK_DIR)/lib/libpxe_r3.a: $(PXE_R3_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(SDK_DIR)/lib/libpxe_r0.a: $(PXE_R0_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(OBJ_DIR)/%.o: %.c $(PXE_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(PXE_TARGET_CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 -o $@ $<

$(APP_ELF): $(PXE_CRT0) $(APP_OBJS) $(PXE_LIB) $(SDK_DIR)/crt/pxe_pie.ld
	@mkdir -p $(dir $@)
	$(LD) $(PXE_LDFLAGS) -o $@ $(PXE_CRT0) $(APP_OBJS) $(PXE_LIB)

$(APP_OUT): $(APP_PXI) $(APP_ELF) $(MKPXE)
	@mkdir -p $(dir $@)
	$(PYTHON3) $(MKPXE) --manifest $(APP_PXI) --elf $(APP_ELF) --output $@

clean:
	rm -rf $(OBJ_DIR) $(APP_OUT)
