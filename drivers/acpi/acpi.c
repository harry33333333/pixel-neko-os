#include "acpi.h"
#include "limine.h"
#include "mm.h"
#include "vmm.h"
#include "log.h"
#include "port.h"
#include "string.h"

extern volatile struct limine_rsdp_request rsdp_request;

static uint64_t g_lapic_base = 0xFEE00000;
static uint64_t g_ioapic_base = 0xFEC00000;
static uint32_t g_ioapic_gsi_base = 0;
static uint64_t g_pcie_ecam_base = 0;
static acpi_ecam_range_t g_ecam_ranges[MAX_ECAM_RANGES];
static size_t g_ecam_range_count = 0;
static bool g_has_pic = true;
static uint32_t g_cpu_core_count = 0;

static madt_iso_entry_t g_iso_entries[MAX_ISO_ENTRIES];
static size_t g_iso_count = 0;

static acpi_rsdp_t* g_rsdp = NULL;
static acpi_sdt_header_t* g_rsdt_or_xsdt = NULL;
static bool g_is_xsdt = false;

static uint32_t g_fadt_smi_cmd = 0;
static uint8_t  g_fadt_acpi_enable = 0;
static uint32_t g_fadt_pm1a_cnt = 0;
static uint32_t g_fadt_pm1b_cnt = 0;
static uint16_t g_fadt_reset_port = 0;
static uint8_t  g_fadt_reset_val = 0;
static uint16_t g_slp_typa = 0;
static uint16_t g_slp_typb = 0;
static bool     g_s5_valid = false;

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
        vmm_map_page(pml4, (virt + sizeof(acpi_sdt_header_t)) & ~0xFFFULL,
                     (phys + sizeof(acpi_sdt_header_t)) & ~0xFFFULL,
                     PTE_PRESENT | PTE_WRITABLE);
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

    if (mcfg->header.length < sizeof(acpi_mcfg_t)) {
        log_printf("[ACPI WARN] MCFG table is truncated.\n");
        return;
    }

    size_t entry_count =
        (mcfg->header.length - sizeof(acpi_mcfg_t)) /
        sizeof(acpi_mcfg_entry_t);
    acpi_mcfg_entry_t* entries = (acpi_mcfg_entry_t*)(mcfg + 1);

    for (size_t i = 0; i < entry_count && i < MAX_ECAM_RANGES; i++) {
        if (entries[i].end_bus < entries[i].start_bus ||
            entries[i].base_address == 0) {
            log_printf("[ACPI WARN] Ignoring invalid MCFG range %llu.\n",
                       (unsigned long long)i);
            continue;
        }
        g_ecam_ranges[g_ecam_range_count].base_address =
            entries[i].base_address;
        g_ecam_ranges[g_ecam_range_count].pci_segment =
            entries[i].pci_segment;
        g_ecam_ranges[g_ecam_range_count].start_bus =
            entries[i].start_bus;
        g_ecam_ranges[g_ecam_range_count].end_bus =
            entries[i].end_bus;
        if (g_ecam_range_count == 0) {
            g_pcie_ecam_base = entries[i].base_address;
        }
        log_printf("[ACPI] PCIe ECAM[%llu] Base: 0x%016llx (Segment %u, Bus %u..%u)\n",
                   (unsigned long long)g_ecam_range_count,
                   entries[i].base_address, entries[i].pci_segment,
                   entries[i].start_bus, entries[i].end_bus);
        g_ecam_range_count++;
    }
}

static void parse_fadt(void);

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
    parse_fadt();
}

static bool parse_s5_from_aml(const uint8_t* aml, uint32_t aml_len) {
    if (!aml || aml_len < 10) return false;
    for (uint32_t i = 0; i + 8 < aml_len; i++) {
        if (aml[i] == '_' && aml[i + 1] == 'S' &&
            aml[i + 2] == '5' && aml[i + 3] == '_') {
            bool valid_name = false;
            if (i > 0 && aml[i - 1] == 0x08) valid_name = true;
            else if (i > 1 && aml[i - 2] == 0x08 && aml[i - 1] == '\\') valid_name = true;
            else if (aml[i + 4] == 0x12) valid_name = true;
            if (!valid_name || aml[i + 4] != 0x12) continue;

            const uint8_t* p = &aml[i + 5];
            const uint8_t* end = aml + aml_len;
            uint8_t pkg_len_bytes = (uint8_t)(((p[0] & 0xC0u) >> 6) + 2u);
            p += pkg_len_bytes;
            if (p >= end) break;

            uint8_t slp_typa = 0;
            uint8_t slp_typb = 0;
            if (*p == 0x0Au) {
                p++; if (p >= end) break;
                slp_typa = *p++;
            } else if (*p == 0x0Bu) {
                p++; if (p + 1 >= end) break;
                slp_typa = *p; p += 2;
            } else if (*p == 0x0Cu) {
                p++; if (p + 3 >= end) break;
                slp_typa = *p; p += 4;
            } else {
                slp_typa = *p++;
            }

            if (p < end) {
                if (*p == 0x0Au) {
                    p++; if (p < end) slp_typb = *p;
                } else if (*p == 0x0Bu) {
                    p++; if (p + 1 < end) slp_typb = *p;
                } else if (*p == 0x0Cu) {
                    p++; if (p + 3 < end) slp_typb = *p;
                } else {
                    slp_typb = *p;
                }
            }
            g_slp_typa = (uint16_t)((slp_typa & 0x07u) << 10);
            g_slp_typb = (uint16_t)((slp_typb & 0x07u) << 10);
            g_s5_valid = true;
            log_printf("[ACPI] Found _S5_ package: SLP_TYPa=%u (0x%04x) SLP_TYPb=%u (0x%04x)\n",
                       slp_typa & 0x07u, g_slp_typa, slp_typb & 0x07u, g_slp_typb);
            return true;
        }
    }
    return false;
}

static void parse_fadt(void) {
    acpi_sdt_header_t* facp = acpi_find_table("FACP");
    if (!facp || facp->length < 76) {
        log_printf("[ACPI WARN] FACP (FADT) table not found or truncated.\n");
        return;
    }
    const uint8_t* raw = (const uint8_t*)facp;
    uint32_t dsdt32 = *(const uint32_t*)(raw + 40);
    g_fadt_smi_cmd = *(const uint32_t*)(raw + 48);
    g_fadt_acpi_enable = raw[52];
    g_fadt_pm1a_cnt = *(const uint32_t*)(raw + 64);
    g_fadt_pm1b_cnt = *(const uint32_t*)(raw + 68);

    if (facp->length >= 129) {
        uint8_t space_id = raw[116];
        uint64_t reset_addr = *(const uint64_t*)(raw + 120);
        uint8_t reset_val = raw[128];
        if (space_id == 1 && reset_addr > 0 && reset_addr <= 0xFFFFu) {
            g_fadt_reset_port = (uint16_t)reset_addr;
            g_fadt_reset_val = reset_val;
        }
    }

    uint64_t dsdt_phys = dsdt32;
    if (facp->length >= 148) {
        uint64_t x_dsdt = *(const uint64_t*)(raw + 140);
        if (x_dsdt != 0) dsdt_phys = x_dsdt;
    }
    if (g_fadt_pm1a_cnt == 0 && facp->length >= 184) {
        uint8_t space_id = raw[172];
        uint64_t x_pm1a = *(const uint64_t*)(raw + 176);
        if (space_id == 1 && x_pm1a <= 0xFFFFu) {
            g_fadt_pm1a_cnt = (uint32_t)x_pm1a;
        }
    }
    if (g_fadt_pm1b_cnt == 0 && facp->length >= 196) {
        uint8_t space_id = raw[184];
        uint64_t x_pm1b = *(const uint64_t*)(raw + 188);
        if (space_id == 1 && x_pm1b <= 0xFFFFu) {
            g_fadt_pm1b_cnt = (uint32_t)x_pm1b;
        }
    }

    log_printf("[ACPI] FADT: PM1a_CNT=0x%04x PM1b_CNT=0x%04x SMI_CMD=0x%04x DSDT=0x%llx\n",
               g_fadt_pm1a_cnt, g_fadt_pm1b_cnt, g_fadt_smi_cmd,
               (unsigned long long)dsdt_phys);

    if (dsdt_phys != 0) {
        acpi_sdt_header_t* dsdt = (acpi_sdt_header_t*)acpi_resolve_ptr(dsdt_phys);
        if (dsdt) {
            acpi_ensure_table_mapped(dsdt);
            if (dsdt->length > sizeof(acpi_sdt_header_t) &&
                dsdt->length < 1024 * 1024) {
                parse_s5_from_aml((const uint8_t*)dsdt + sizeof(acpi_sdt_header_t),
                                  dsdt->length - (uint32_t)sizeof(acpi_sdt_header_t));
            }
        }
    }

    /* Also scan SSDTs if _S5_ was not in DSDT */
    if (!g_s5_valid && g_rsdt_or_xsdt) {
        size_t header_len = sizeof(acpi_sdt_header_t);
        size_t count = g_is_xsdt
            ? (g_rsdt_or_xsdt->length - header_len) / sizeof(uint64_t)
            : (g_rsdt_or_xsdt->length - header_len) / sizeof(uint32_t);
        for (size_t i = 0; i < count && !g_s5_valid; i++) {
            uint64_t addr = g_is_xsdt
                ? ((const uint64_t*)((const uint8_t*)g_rsdt_or_xsdt + header_len))[i]
                : ((const uint32_t*)((const uint8_t*)g_rsdt_or_xsdt + header_len))[i];
            if (!addr) continue;
            acpi_sdt_header_t* hdr = (acpi_sdt_header_t*)acpi_resolve_ptr(addr);
            if (!hdr) continue;
            acpi_ensure_table_mapped(hdr);
            if (memcmp(hdr->signature, "SSDT", 4) == 0 &&
                hdr->length > sizeof(acpi_sdt_header_t) &&
                hdr->length < 1024 * 1024) {
                parse_s5_from_aml((const uint8_t*)hdr + sizeof(acpi_sdt_header_t),
                                  hdr->length - (uint32_t)sizeof(acpi_sdt_header_t));
            }
        }
    }
}

void acpi_shutdown(void) {
    uint16_t slp_en = (uint16_t)(1u << 13);

    if (g_fadt_pm1a_cnt != 0) {
        /* Enable ACPI mode if SCI_EN (bit 0) is not yet set */
        if ((inw((uint16_t)g_fadt_pm1a_cnt) & 1u) == 0 &&
            g_fadt_smi_cmd != 0 && g_fadt_acpi_enable != 0) {
            outb((uint16_t)g_fadt_smi_cmd, g_fadt_acpi_enable);
            for (int i = 0; i < 300000; i++) {
                if (inw((uint16_t)g_fadt_pm1a_cnt) & 1u) break;
                inb(0x80);
            }
        }

        if (g_s5_valid) {
            log_printf("[ACPI] Entering S5 soft-off: PM1a=0x%04x val=0x%04x\n",
                       g_fadt_pm1a_cnt, (uint16_t)(g_slp_typa | slp_en));
            outw((uint16_t)g_fadt_pm1a_cnt, (uint16_t)(g_slp_typa | slp_en));
            if (g_fadt_pm1b_cnt != 0) {
                outw((uint16_t)g_fadt_pm1b_cnt, (uint16_t)(g_slp_typb | slp_en));
            }
            for (int i = 0; i < 100000; i++) inb(0x80);
        }

        /* Fallback common S5 SLP_TYP values (Intel ICH/PCH=7 or 5, PIIX4=0) */
        static const uint8_t fallback_typs[] = { 7, 5, 0 };
        for (int k = 0; k < 3; k++) {
            uint16_t val = (uint16_t)(((uint16_t)fallback_typs[k] << 10) | slp_en);
            outw((uint16_t)g_fadt_pm1a_cnt, val);
            if (g_fadt_pm1b_cnt != 0) {
                outw((uint16_t)g_fadt_pm1b_cnt, val);
            }
            for (int i = 0; i < 50000; i++) inb(0x80);
        }
    }

    /* Emulator / Virtual Machine ACPI power-off ports (QEMU / Bochs / VirtualBox) */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
}

void acpi_reboot(void) {
    if (g_fadt_reset_port != 0) {
        outb(g_fadt_reset_port, g_fadt_reset_val);
        for (int i = 0; i < 50000; i++) inb(0x80);
    }
    /* PCI Reset Control Register (0xCF9) supported on Intel H55 & modern chipsets */
    outb(0xCF9, 0x02);
    for (int i = 0; i < 10000; i++) inb(0x80);
    outb(0xCF9, 0x06);
    for (int i = 0; i < 50000; i++) inb(0x80);
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

size_t acpi_get_ecam_range_count(void) {
    return g_ecam_range_count;
}

const acpi_ecam_range_t* acpi_get_ecam_range(size_t index) {
    return index < g_ecam_range_count ? &g_ecam_ranges[index] : NULL;
}

bool acpi_has_pic(void) {
    return g_has_pic;
}

uint32_t acpi_get_core_count(void) {
    return g_cpu_core_count > 0 ? g_cpu_core_count : 1;
}
