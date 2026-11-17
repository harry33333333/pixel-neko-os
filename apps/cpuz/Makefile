APP_NAME := cpuz
APP_SRCS := cpuz.c compat/libc_shim.c \
            libcpuid/cpuid_main.c libcpuid/libcpuid_util.c \
            libcpuid/asm-bits.c libcpuid/recog_intel.c \
            libcpuid/recog_amd.c libcpuid/recog_centaur.c
APP_PXI  := cpuz.pxi
PXE_RING := 0
SDK_DIR  ?= ../../sdk

EXTRA_CFLAGS := -Icompat -Ilibcpuid \
                -DLIBCPUID_PIXEL_NEKO \
                -DLIBCPUID_DISABLE_DEPRECATED

include $(SDK_DIR)/pxe_app.mk
