#ifndef _PXE_FORMAT_H
#define _PXE_FORMAT_H

#include <stdint.h>

/*
 * Pixel Neko OS Native Executable Format (*.pxe) v1.0
 * Supports dual privilege rings:
 *   - PXE_RING_3 (3): Isolated Ring 3 userland process (default)
 *   - PXE_RING_0 (0): Privileged Ring 0 kernel-space process
 */

#define PXE_MAGIC              0x01455850u /* 'P','X','E',0x01 */
#define PXE_ABI_VERSION_1_0    0x0100u
#define PXE_ARCH_X86_64        0x8664u

/* Privilege ring (exec_ring) */
#define PXE_RING_0             0u  /* Kernel-mode process (sysinfo.pxe, install.pxe, taskmgr.pxe) */
#define PXE_RING_3             3u  /* User-mode isolated process (default) */

/* Subsystem type (subsystem) */
#define PXE_SUBSYS_GUI         1u  /* Windowed desktop application */
#define PXE_SUBSYS_BACKGROUND  2u  /* Headless / background utility */

/* Behavioral flags (flags) */
#define PXE_FLAG_SINGLE_INST   (1u << 0) /* Focus existing instance instead of spawning duplicate */
#define PXE_FLAG_RESIZABLE     (1u << 1) /* Allow bottom-right window resize grip */
#define PXE_FLAG_NO_CLOSE      (1u << 2) /* Hide title-bar close/shade buttons */
#define PXE_FLAG_SYS_SETTINGS  (1u << 3) /* Grant Ring 3 access to display/desktop settings syscalls */

typedef struct __attribute__((packed)) {
    uint32_t magic;            /* 0x00: PXE_MAGIC */
    uint16_t abi_version;      /* 0x04: PXE_ABI_VERSION_1_0 */
    uint16_t arch;             /* 0x06: PXE_ARCH_X86_64 */
    uint8_t  exec_ring;        /* 0x08: PXE_RING_3 (3) or PXE_RING_0 (0) */
    uint8_t  subsystem;        /* 0x09: PXE_SUBSYS_* */
    uint16_t flags;            /* 0x0A: PXE_FLAG_* */
    uint32_t header_size;      /* 0x0C: 128 */
    uint32_t rsrc_offset;      /* 0x10: File offset of pxe_resource_t */
    uint32_t rsrc_size;        /* 0x14: Byte length of resource section */
    uint32_t elf_offset;       /* 0x18: Page-aligned file offset of ELF64 PIE payload */
    uint32_t elf_size;         /* 0x1C: Byte length of ELF64 PIE payload */
    uint32_t stack_size;       /* 0x20: Stack size in bytes (default 64 KB) */
    uint32_t heap_init_size;   /* 0x24: Initial heap size in bytes (default 128 KB) */
    uint16_t def_win_w;        /* 0x28: Default window width */
    uint16_t def_win_h;        /* 0x2A: Default window height */
    uint16_t min_win_w;        /* 0x2C: Minimum window width */
    uint16_t min_win_h;        /* 0x2E: Minimum window height */
    uint32_t checksum_crc32;   /* 0x30: Whole-file IEEE 802.3 CRC32 (bytes 0x30..0x33 zeroed) */
    char     app_name[32];     /* 0x34: Short application name */
    char     app_version[16];  /* 0x54: Version string */
    uint8_t  reserved[28];     /* 0x64..0x7F: Reserved (0) */
} pxe_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  icon_16x16[256];  /* 16x16 8-bit classic palette icon (255 = transparent) */
    char     title[48];        /* Default window / desktop title */
    char     author[32];       /* Developer / vendor name */
    char     ext_assoc[32];    /* Comma-separated file extensions (e.g. "txt,log,md") */
    uint8_t  reserved[16];     /* Reserved (0) */
} pxe_resource_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(pxe_header_t) == 128, "pxe_header_t must be 128 bytes");
_Static_assert(sizeof(pxe_resource_t) == 384, "pxe_resource_t must be 384 bytes");
#endif

#endif /* _PXE_FORMAT_H */

