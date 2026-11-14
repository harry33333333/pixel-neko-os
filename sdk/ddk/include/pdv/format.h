#ifndef PDV_FORMAT_H
#define PDV_FORMAT_H

#include <stdint.h>

#define PDV_MAGIC_0 'P'
#define PDV_MAGIC_1 'D'
#define PDV_MAGIC_2 'V'
#define PDV_MAGIC_3 '1'

#define PDV_FORMAT_VERSION_1_0  0x0100U
#define PDV_DDK_VERSION_1_0     0x0100U

#define PDV_CLASS_UNKNOWN       0U
#define PDV_CLASS_DISPLAY       1U
#define PDV_CLASS_AUDIO         2U
#define PDV_CLASS_INPUT         3U
#define PDV_CLASS_STORAGE       4U
#define PDV_CLASS_NETWORK       5U
#define PDV_CLASS_SYSTEM        6U

#define PDV_FLAG_BOOT_START     (1U << 0)
#define PDV_FLAG_DEMAND_START   (1U << 1)
#define PDV_FLAG_SAFE_MODE_OK   (1U << 2)

#define PDV_MATCH_ANY           0U
#define PDV_MATCH_PCI_ID        1U
#define PDV_MATCH_PCI_SUBSYS    2U
#define PDV_MATCH_PCI_CLASS     3U
#define PDV_MATCH_PCI_VEN_CLASS 4U
#define PDV_MATCH_ISA_PNP       5U

#define PDV_WILDCARD_U8         0xFFU
#define PDV_WILDCARD_U16        0xFFFFU

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];          /* "PDV1" */
    uint16_t format_version;    /* 0x0100 (v1.0) */
    uint16_t min_ddk_version;   /* Minimum kernel DDK ABI version (0x0100) */
    uint16_t driver_class;      /* PDV_CLASS_* */
    uint16_t flags;             /* PDV_FLAG_* */
    uint32_t match_count;       /* Number of pdv_match_rule_t entries */
    uint32_t match_table_off;   /* Byte offset to match table (128) */
    uint32_t elf_payload_off;   /* Page-aligned (4096) offset to ELF64 ET_REL */
    uint32_t elf_payload_size;  /* Size in bytes of embedded ELF64 object */
    uint32_t file_crc32;        /* IEEE 802.3 CRC32 (computed with field = 0) */
    char     driver_name[32];   /* Null-terminated internal driver identifier */
    char     provider[32];      /* Vendor / author string */
    char     version_str[16];   /* Human-readable driver version */
    uint8_t  reserved[16];      /* Zero-padded reserved bytes */
} pdv_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  match_type;        /* PDV_MATCH_* */
    uint8_t  pci_class;         /* PCI base class */
    uint8_t  pci_subclass;      /* PCI subclass */
    uint8_t  pci_progif;        /* PCI prog-if (0xFF = wildcard) */
    uint16_t vendor_id;         /* PCI vendor ID (0xFFFF = wildcard) */
    uint16_t device_id;         /* PCI device ID (0xFFFF = wildcard) */
    uint16_t subsys_vendor;     /* PCI subsystem vendor ID (0xFFFF = wildcard) */
    uint16_t subsys_device;     /* PCI subsystem device ID (0xFFFF = wildcard) */
    uint16_t isa_base_port;     /* Legacy ISA base I/O port (e.g. 0x03F0) */
    uint8_t  isa_irq;           /* Legacy ISA IRQ (0xFF = unused) */
    uint8_t  isa_dma;           /* Legacy ISA DMA channel (0xFF = unused) */
} pdv_match_rule_t;

_Static_assert(sizeof(pdv_header_t) == 128, "pdv_header_t must be 128 bytes");
_Static_assert(sizeof(pdv_match_rule_t) == 16, "pdv_match_rule_t must be 16 bytes");

#endif /* PDV_FORMAT_H */
