APP_NAME := vplayer
APP_PXI  := vplayer.pxi
PXE_RING := 3
SDK_DIR  ?= ../../sdk

OPENH264_DIR := third_party/openh264
FAAD2_DIR    := third_party/faad2

APP_SRCS := vplayer.c \
            vplayer_mp4.c \
            vplayer_audio.c \
            vplayer_yuv.c \
            compat/libc_shim.c \
            $(FAAD2_DIR)/libfaad/bits.c \
            $(FAAD2_DIR)/libfaad/cfft.c \
            $(FAAD2_DIR)/libfaad/common.c \
            $(FAAD2_DIR)/libfaad/decoder.c \
            $(FAAD2_DIR)/libfaad/drc.c \
            $(FAAD2_DIR)/libfaad/error.c \
            $(FAAD2_DIR)/libfaad/filtbank.c \
            $(FAAD2_DIR)/libfaad/hcr.c \
            $(FAAD2_DIR)/libfaad/huffman.c \
            $(FAAD2_DIR)/libfaad/ic_predict.c \
            $(FAAD2_DIR)/libfaad/is.c \
            $(FAAD2_DIR)/libfaad/lt_predict.c \
            $(FAAD2_DIR)/libfaad/mdct.c \
            $(FAAD2_DIR)/libfaad/mp4.c \
            $(FAAD2_DIR)/libfaad/ms.c \
            $(FAAD2_DIR)/libfaad/output.c \
            $(FAAD2_DIR)/libfaad/pns.c \
            $(FAAD2_DIR)/libfaad/pulse.c \
            $(FAAD2_DIR)/libfaad/rvlc.c \
            $(FAAD2_DIR)/libfaad/specrec.c \
            $(FAAD2_DIR)/libfaad/syntax.c \
            $(FAAD2_DIR)/libfaad/tns.c

APP_CXX_SRCS := compat/cxx_shim.cpp \
                vplayer_h264.cpp \
                $(OPENH264_DIR)/codec/common/src/common_tables.cpp \
                $(OPENH264_DIR)/codec/common/src/copy_mb.cpp \
                $(OPENH264_DIR)/codec/common/src/cpu.cpp \
                $(OPENH264_DIR)/codec/common/src/crt_util_safe_x.cpp \
                $(OPENH264_DIR)/codec/common/src/deblocking_common.cpp \
                $(OPENH264_DIR)/codec/common/src/expand_pic.cpp \
                $(OPENH264_DIR)/codec/common/src/intra_pred_common.cpp \
                $(OPENH264_DIR)/codec/common/src/mc.cpp \
                $(OPENH264_DIR)/codec/common/src/memory_align.cpp \
                $(OPENH264_DIR)/codec/common/src/sad_common.cpp \
                $(OPENH264_DIR)/codec/common/src/utils.cpp \
                $(OPENH264_DIR)/codec/common/src/welsCodecTrace.cpp \
                $(OPENH264_DIR)/codec/common/src/WelsTaskThread.cpp \
                $(OPENH264_DIR)/codec/common/src/WelsThread.cpp \
                $(OPENH264_DIR)/codec/common/src/WelsThreadLib.cpp \
                $(OPENH264_DIR)/codec/common/src/WelsThreadPool.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/au_parser.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/bit_stream.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/cabac_decoder.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/deblocking.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/decode_mb_aux.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/decode_slice.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/decoder.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/decoder_core.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/decoder_data_tables.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/error_concealment.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/fmo.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/get_intra_predictor.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/manage_dec_ref.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/memmgr_nal_unit.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/mv_pred.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/parse_mb_syn_cabac.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/parse_mb_syn_cavlc.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/pic_queue.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/rec_mb.cpp \
                $(OPENH264_DIR)/codec/decoder/core/src/wels_decoder_thread.cpp \
                $(OPENH264_DIR)/codec/decoder/plus/src/welsDecoderExt.cpp

APP_ASM_SRCS := $(OPENH264_DIR)/codec/common/x86/cpuid.asm \
                $(OPENH264_DIR)/codec/common/x86/dct.asm \
                $(OPENH264_DIR)/codec/common/x86/deblock.asm \
                $(OPENH264_DIR)/codec/common/x86/expand_picture.asm \
                $(OPENH264_DIR)/codec/common/x86/intra_pred_com.asm \
                $(OPENH264_DIR)/codec/common/x86/mb_copy.asm \
                $(OPENH264_DIR)/codec/common/x86/mc_chroma.asm \
                $(OPENH264_DIR)/codec/common/x86/mc_luma.asm \
                $(OPENH264_DIR)/codec/common/x86/satd_sad.asm \
                $(OPENH264_DIR)/codec/common/x86/vaa.asm \
                $(OPENH264_DIR)/codec/decoder/core/x86/dct.asm \
                $(OPENH264_DIR)/codec/decoder/core/x86/intra_pred.asm

EXTRA_CFLAGS := -isystem compat \
                -I$(FAAD2_DIR)/include -I$(FAAD2_DIR)/libfaad \
                -Ithird_party/minimp4 \
                -DFIXED_POINT -DLC_ONLY_DECODER -DDISABLE_SBR \
                -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1 \
                -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 \
                -DPACKAGE_VERSION=\"2.11.1\" \
                -Wno-unused-parameter -Wno-sign-compare -Wno-unused-function \
                -Wno-unused-variable -Wno-implicit-fallthrough -Wno-pointer-sign

EXTRA_CXXFLAGS := -std=gnu++11 -w \
                  -isystem compat \
                  -I$(OPENH264_DIR)/codec/api/wels \
                  -I$(OPENH264_DIR)/codec/common/inc \
                  -I$(OPENH264_DIR)/codec/decoder/core/inc \
                  -I$(OPENH264_DIR)/codec/decoder/plus/inc \
                  -D__linux__ -DX86_ASM -DHAVE_AVX2 -DUNIX64

EXTRA_ASMFLAGS := -DUNIX64 -DHAVE_AVX2 -I$(OPENH264_DIR)/codec/common/x86/

include $(SDK_DIR)/pxe_app.mk
