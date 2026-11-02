#ifndef _ACPI_H
#define _ACPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct {
    acpi_sdt_header_t header;
    uint32_t lapic_address;
    uint32_t flags; // 1 = PC-AT 8259 PIC present
} __attribute__((packed)) acpi_madt_t;

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) acpi_madt_entry_t;

typedef struct {
    acpi_madt_entry_t header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags; // 1 = Enabled
} __attribute__((packed)) madt_lapic_entry_t;

typedef struct {
    acpi_madt_entry_t header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
} __attribute__((packed)) madt_ioapic_entry_t;

typedef struct {
    acpi_madt_entry_t header;
    uint8_t bus;        // 0 = ISA
    uint8_t source_irq; // Source ISA IRQ
    uint32_t gsi;       // Global System Interrupt
    uint16_t flags;     // Polarity and Trigger mode
} __attribute__((packed)) madt_iso_entry_t;

typedef struct {
    acpi_sdt_header_t header;
    uint64_t reserved;
} __attribute__((packed)) acpi_mcfg_t;

typedef struct {
    uint64_t base_address;
    uint16_t pci_segment;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed)) acpi_mcfg_entry_t;

#define MAX_ISO_ENTRIES 32

void acpi_init(void);
acpi_sdt_header_t* acpi_find_table(const char* signature);
uint64_t acpi_get_lapic_base(void);
uint64_t acpi_get_ioapic_base(void);
uint32_t acpi_get_ioapic_gsi_base(void);
uint32_t acpi_remap_irq(uint8_t irq);
uint16_t acpi_get_irq_flags(uint8_t irq);
uint64_t acpi_get_pcie_ecam_base(void);
bool acpi_has_pic(void);
uint32_t acpi_get_core_count(void);

#endif /* _ACPI_H */
