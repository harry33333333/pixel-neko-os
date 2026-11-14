# Pixel Neko OS Native Driver Development Kit (DDK v1.0)
# Usage:
#   DRIVER_NAME := mydriver
#   DRIVER_SRCS := mydriver.c
#   DRIVER_PDI  := mydriver.pdi
#   PDV_DDK_DIR := /path/to/pixel-neko-os/sdk/ddk
#   include $(PDV_DDK_DIR)/pdv_driver.mk

CC      ?= gcc
LD      ?= ld
PYTHON3 ?= python3

PDV_DDK_DIR ?= $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
MKPDV       ?= $(PDV_DDK_DIR)/../../scripts/mkpdv.py
BUILD_DIR   ?= build

PDV_CFLAGS := -m64 -march=core2 -mcmodel=kernel -mno-red-zone \
              -mgeneral-regs-only -fno-asynchronous-unwind-tables \
              -ffreestanding -nostdlib -nostartfiles \
              -fno-builtin -fno-stack-protector -fno-pie -fno-pic \
              -std=gnu11 -O2 -Wall -Wextra -I$(PDV_DDK_DIR)/include

DRIVER_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(DRIVER_SRCS))
DRIVER_OUT  ?= $(BUILD_DIR)/$(DRIVER_NAME).pdv

.PHONY: all clean

all: $(DRIVER_OUT)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(PDV_CFLAGS) $(EXTRA_CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(DRIVER_NAME).rel.o: $(DRIVER_OBJS)
	$(LD) -m elf_x86_64 -r $(DRIVER_OBJS) -o $@

$(DRIVER_OUT): $(DRIVER_PDI) $(BUILD_DIR)/$(DRIVER_NAME).rel.o
	@mkdir -p $(dir $@)
	$(PYTHON3) $(MKPDV) --manifest $(DRIVER_PDI) --object $(BUILD_DIR)/$(DRIVER_NAME).rel.o --output $@

clean:
	rm -rf $(BUILD_DIR)
