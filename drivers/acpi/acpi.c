#include "acpi.h"
#include "limine.h"
#include "mm.h"
#include "vmm.h"
#include "log.h"
#include "string.h"

extern volatile struct limine_rsdp_request rsdp_request;

static uint64_t g_lapic_base = 0xFEE00000;
static uint64_t g_ioapic_base = 0xFEC00000;
static uint32_t g_ioapic_gsi_base = 0;
static uint64_t g_pcie_ecam_base = 0;
static bool g_has_pic = true;
static uint32_t g_cpu_core_count = 0;

static madt_iso_entry_t g_iso_entries[MAX_ISO_ENTRIES];
static size_t g_iso_count = 0;

static acpi_rsdp_t* g_rsdp = NULL;
static acpi_sdt_header_t* g_rsdt_or_xsdt = NULL;
static bool g_is_xsdt = false;

static bool acpi_validate_checksum(const void* table, size_t length) {
    const uint8_t* bytes = (const uint8_t*)table;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

static inline void* acpi_resolve_ptr(uint64_t addr) {
    if (addr == 0) return NULL;
    uint64_t virt = addr;
    uint64_t phys = addr;
    if (addr < g_hhdm.hhdm_offset) {
        virt = g_hhdm.hhdm_offset + addr;
    } else {
        phys = addr - g_hhdm.hhdm_offset;
    }
    pagetable_t pml4 = vmm_get_kernel_pml4();
    if (pml4) {
        vmm_map_page(pml4, virt & ~0xFFFULL, phys & ~0xFFFULL, PTE_PRESENT | PTE_WRITABLE);
    }
    return (void*)virt;
}

static void acpi_ensure_table_mapped(void* table_ptr) {
    if (!table_ptr) return;
    pagetable_t pml4 = vmm_get_kernel_pml4();
    if (!pml4) return;
    acpi_sdt_header_t* header = (acpi_sdt_header_t*)table_ptr;
    uint64_t virt = (uint64_t)header;
    if (virt < g_hhdm.hhdm_offset) return;
    uint64_t phys = virt - g_hhdm.hhdm_offset;
    uint32_t len = header->length;
    if (len > 0 && len < 1024 * 1024) { // Valid table size check (< 1MB)
        uint64_t start_virt = virt & ~0xFFFULL;
        uint64_t end_virt = (virt + len + PAGE_SIZE - 1) & ~0xFFFULL;
        uint64_t cur_phys = phys & ~0xFFFULL;
        for (uint64_t v = start_virt; v < end_virt; v += PAGE_SIZE, cur_phys += PAGE_SIZE) {
            vmm_map_page(pml4, v, cur_phys, PTE_PRESENT | PTE_WRITABLE);
        }
    }
}

acpi_sdt_header_t* acpi_find_table(const char* signature) {
    if (!g_rsdt_or_xsdt) return NULL;

    size_t header_len = sizeof(acpi_sdt_header_t);
    if (g_rsdt_or_xsdt->length < header_len) return NULL;

    size_t entry_count;
    if (g_is_xsdt) {
        entry_count = (g_rsdt_or_xsdt->length - header_len) / sizeof(uint64_t);
        uint64_t* entries = (uint64_t*)((uint8_t*)g_rsdt_or_xsdt + header_len);
        for (size_t i = 0; i < entry_count; i++) {
            if (entries[i] == 0) continue;
            acpi_sdt_header_t* header = (acpi_sdt_header_t*)acpi_resolve_ptr(entries[i]);
            if (!header) continue;
            acpi_ensure_table_mapped(header);
            if (memcmp(header->signature, signature, 4) == 0) {
                return header;
            }
        }
    } else {
        entry_count = (g_rsdt_or_xsdt->length - header_len) / sizeof(uint32_t);
        uint32_t* entries = (uint32_t*)((uint8_t*)g_rsdt_or_xsdt + header_len);
        for (size_t i = 0; i < entry_count; i++) {
            if (entries[i] == 0) continue;
            acpi_sdt_header_t* header = (acpi_sdt_header_t*)acpi_resolve_ptr(entries[i]);
            if (!header) continue;
            acpi_ensure_table_mapped(header);
            if (memcmp(header->signature, signature, 4) == 0) {
                return header;
            }
        }
    }

    return NULL;
}

static void parse_madt(void) {
    acpi_madt_t* madt = (acpi_madt_t*)acpi_find_table("APIC");
    if (!madt) {
        log_printf("[ACPI WARN] MADT (APIC) table not found, using legacy default bases!\n");
        return;
    }

    if (!acpi_validate_checksum(madt, madt->header.length)) {
        log_printf("[ACPI WARN] MADT checksum validation failed!\n");
    }

    g_lapic_base = madt->lapic_address;
    g_has_pic = (madt->flags & 1) != 0;

    log_printf("[ACPI] MADT found: LAPIC default phys 0x%08llx, PC-AT Dual 8259 PIC: %s\n",
               g_lapic_base, g_has_pic ? "YES" : "NO");

    uint8_t* ptr = (uint8_t*)(madt + 1);
    uint8_t* end = (uint8_t*)madt + madt->header.length;

    while (ptr < end) {
        acpi_madt_entry_t* entry = (acpi_madt_entry_t*)ptr;
        if (entry->length < 2 || ptr + entry->length > end) break;

        switch (entry->type) {
            case 0: { // Processor Local APIC
                madt_lapic_entry_t* lapic = (madt_lapic_entry_t*)entry;
                if (lapic->flags & 1) {
                    g_cpu_core_count++;
                }
                break;
            }
            case 1: { // I/O APIC
                madt_ioapic_entry_t* ioapic = (madt_ioapic_entry_t*)entry;
                g_ioapic_base = ioapic->ioapic_address;
                g_ioapic_gsi_base = ioapic->gsi_base;
                log_printf("[ACPI] Found I/O APIC ID %u at Phys 0x%08x (GSI Base: %u)\n",
                           ioapic->ioapic_id, ioapic->ioapic_address, ioapic->gsi_base);
                break;
            }
            case 2: { // Interrupt Source Override (ISO)
                madt_iso_entry_t* iso = (madt_iso_entry_t*)entry;
                if (g_iso_count < MAX_ISO_ENTRIES) {
                    g_iso_entries[g_iso_count++] = *iso;
                    log_printf("[ACPI] ISO: ISA IRQ %u -> GSI %u (flags: 0x%04x)\n",
                               iso->source_irq, iso->gsi, iso->flags);
                }
                break;
            }
            case 5: { // 64-bit Local APIC Address Override
                struct {
                    acpi_madt_entry_t header;
                    uint16_t reserved;
                    uint64_t lapic_phys_address;
                } __attribute__((packed))* madt_ovr = (void*)entry;
                g_lapic_base = madt_ovr->lapic_phys_address;
                log_printf("[ACPI] 64-bit LAPIC Base Override: 0x%016llx\n", g_lapic_base);
                break;
            }
            default:
                break;
        }

        ptr += entry->length;
    }

    log_printf("[ACPI] Total detected online CPU cores: %u\n", g_cpu_core_count);
}

static void parse_mcfg(void) {
    acpi_mcfg_t* mcfg = (acpi_mcfg_t*)acpi_find_table("MCFG");
    if (!mcfg) {
        log_printf("[ACPI] MCFG table not found (PCIe ECAM unavailable, legacy PCI fallback)\n");
        return;
    }

    if (!acpi_validate_checksum(mcfg, mcfg->header.length)) {
        log_printf("[ACPI WARN] MCFG checksum validation failed!\n");
    }

    size_t entry_count = (mcfg->header.length - sizeof(acpi_mcfg_t)) / sizeof(acpi_mcfg_entry_t);
    acpi_mcfg_entry_t* entries = (acpi_mcfg_entry_t*)(mcfg + 1);

    if (entry_count > 0) {
        g_pcie_ecam_base = entries[0].base_address;
        log_printf("[ACPI] PCIe ECAM Base: 0x%016llx (Segment %u, Bus %u..%u)\n",
                   g_pcie_ecam_base, entries[0].pci_segment,
                   entries[0].start_bus, entries[0].end_bus);
    }
}

void acpi_init(void) {
    if (!rsdp_request.response || !rsdp_request.response->address) {
        log_printf("[ACPI WARN] Limine did not provide RSDP address!\n");
        return;
    }

    uint64_t rsdp_addr = (uint64_t)rsdp_request.response->address;
    g_rsdp = (acpi_rsdp_t*)acpi_resolve_ptr(rsdp_addr);
    if (!g_rsdp) {
        log_printf("[ACPI ERROR] Failed to resolve RSDP address (0x%016llx)!\n", rsdp_addr);
        return;
    }

    // Verify "RSD PTR " signature
    if (memcmp(g_rsdp->signature, "RSD PTR ", 8) != 0) {
        log_printf("[ACPI ERROR] Invalid RSDP signature!\n");
        return;
    }

    // Verify revision 1.0 checksum (first 20 bytes)
    if (!acpi_validate_checksum(g_rsdp, 20)) {
        log_printf("[ACPI ERROR] RSDP v1 checksum error!\n");
        return;
    }

    log_printf("[ACPI] RSDP found at %p (Phys: 0x%016llx, Rev: %u, OEM: %.6s)\n",
               g_rsdp, rsdp_addr, g_rsdp->revision, g_rsdp->oem_id);

    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_address != 0) {
        if (!acpi_validate_checksum(g_rsdp, g_rsdp->length)) {
            log_printf("[ACPI WARN] RSDP v2 extended checksum error, falling back to RSDT\n");
            g_is_xsdt = false;
            g_rsdt_or_xsdt = (acpi_sdt_header_t*)acpi_resolve_ptr(g_rsdp->rsdt_address);
        } else {
            g_is_xsdt = true;
            g_rsdt_or_xsdt = (acpi_sdt_header_t*)acpi_resolve_ptr(g_rsdp->xsdt_address);
            log_printf("[ACPI] Using 64-bit XSDT at 0x%016llx\n", g_rsdp->xsdt_address);
        }
    } else {
        g_is_xsdt = false;
        g_rsdt_or_xsdt = (acpi_sdt_header_t*)acpi_resolve_ptr(g_rsdp->rsdt_address);
        log_printf("[ACPI] Using 32-bit RSDT at 0x%08x\n", g_rsdp->rsdt_address);
    }

    if (g_rsdt_or_xsdt) {
        acpi_ensure_table_mapped(g_rsdt_or_xsdt);
    }

    if (g_rsdt_or_xsdt && !acpi_validate_checksum(g_rsdt_or_xsdt, g_rsdt_or_xsdt->length)) {
        log_printf("[ACPI WARN] Root SDT checksum validation failed!\n");
    }

    parse_madt();
    parse_mcfg();
}

uint64_t acpi_get_lapic_base(void) {
    return g_lapic_base;
}

uint64_t acpi_get_ioapic_base(void) {
    return g_ioapic_base;
}

uint32_t acpi_get_ioapic_gsi_base(void) {
    return g_ioapic_gsi_base;
}

uint32_t acpi_remap_irq(uint8_t irq) {
    for (size_t i = 0; i < g_iso_count; i++) {
        if (g_iso_entries[i].source_irq == irq) {
            return g_iso_entries[i].gsi;
        }
    }
    return irq;
}

uint16_t acpi_get_irq_flags(uint8_t irq) {
    for (size_t i = 0; i < g_iso_count; i++) {
        if (g_iso_entries[i].source_irq == irq) {
            return g_iso_entries[i].flags;
        }
    }
    return 0; // Default: edge triggered, active high
}

uint64_t acpi_get_pcie_ecam_base(void) {
    return g_pcie_ecam_base;
}

bool acpi_has_pic(void) {
    return g_has_pic;
}

uint32_t acpi_get_core_count(void) {
    return g_cpu_core_count > 0 ? g_cpu_core_count : 1;
}
