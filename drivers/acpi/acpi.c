#include "acpi.h"
#include "limine.h"
#include "mm.h"
#include "mmio.h"
#include "pmm.h"
#include "vmm.h"
#include "log.h"
#include "port.h"
#include "string.h"

#include <uacpi/sleep.h>
#include <uacpi/uacpi.h>

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

typedef struct {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_width;
    uint64_t address;
} acpi_gas_t;

static acpi_gas_t g_pm1a_cnt_gas;
static acpi_gas_t g_pm1b_cnt_gas;
static bool g_uacpi_enabled = true;
static bool g_uacpi_namespace_ready = false;
static bool g_uacpi_sleep_ready = false;
static bool g_s5_from_uacpi = false;

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

static bool acpi_gas_valid(const acpi_gas_t* gas) {
    if (!gas || !gas->address || gas->bit_width == 0 ||
        gas->bit_width > 32 || gas->bit_offset >= 32 ||
        gas->bit_offset + gas->bit_width > 32) {
        return false;
    }
    if (gas->address_space != 0 && gas->address_space != 1) return false;
    if (gas->address_space == 1 && gas->address > 0xFFFF) return false;
    return gas->access_width == 0 || gas->access_width <= 4;
}

static uint32_t acpi_gas_access_bytes(const acpi_gas_t* gas) {
    if (gas->access_width) {
        return 1u << (gas->access_width - 1u);
    }
    uint32_t bits = gas->bit_offset + gas->bit_width;
    if (bits <= 8) return 1;
    if (bits <= 16) return 2;
    if (bits <= 32) return 4;
    return 0;
}

static bool acpi_gas_read_register(const acpi_gas_t* gas, uint32_t bytes,
                                   uint32_t* out_value) {
    if (!out_value) return false;
    if (gas->address_space == 1) {
        uint16_t port = (uint16_t)gas->address;
        if (bytes == 1) *out_value = inb(port);
        else if (bytes == 2) *out_value = inw(port);
        else *out_value = inl(port);
        return true;
    }
    volatile void* base = mmio_map_phys(gas->address, bytes);
    if (!base) return false;
    if (bytes == 1) *out_value = mmio_read8(base);
    else if (bytes == 2) *out_value = mmio_read16(base);
    else *out_value = mmio_read32(base);
    return true;
}

static bool acpi_gas_write_register(const acpi_gas_t* gas, uint32_t bytes,
                                    uint32_t value) {
    if (gas->address_space == 1) {
        uint16_t port = (uint16_t)gas->address;
        if (bytes == 1) outb(port, (uint8_t)value);
        else if (bytes == 2) outw(port, (uint16_t)value);
        else outl(port, value);
        return true;
    }
    volatile void* base = mmio_map_phys(gas->address, bytes);
    if (!base) return false;
    if (bytes == 1) mmio_write8(base, (uint8_t)value);
    else if (bytes == 2) mmio_write16(base, (uint16_t)value);
    else mmio_write32(base, value);
    return true;
}

static bool acpi_gas_read(const acpi_gas_t* gas, uint32_t* value) {
    if (!value || !acpi_gas_valid(gas)) return false;
    uint32_t bytes = acpi_gas_access_bytes(gas);
    if (!bytes || gas->bit_offset + gas->bit_width > bytes * 8u) {
        return false;
    }
    uint32_t raw;
    if (!acpi_gas_read_register(gas, bytes, &raw)) return false;
    uint32_t mask = gas->bit_width == 32
                        ? 0xFFFFFFFFu
                        : ((1u << gas->bit_width) - 1u);
    *value = (raw >> gas->bit_offset) & mask;
    return true;
}

static bool acpi_gas_write(const acpi_gas_t* gas, uint32_t value) {
    if (!acpi_gas_valid(gas)) return false;
    uint32_t bytes = acpi_gas_access_bytes(gas);
    if (!bytes || gas->bit_offset + gas->bit_width > bytes * 8u) {
        return false;
    }
    uint32_t mask = gas->bit_width == 32
                        ? 0xFFFFFFFFu
                        : ((1u << gas->bit_width) - 1u);
    uint32_t raw;
    if (!acpi_gas_read_register(gas, bytes, &raw)) return false;
    raw &= ~(mask << gas->bit_offset);
    raw |= (value & mask) << gas->bit_offset;
    return acpi_gas_write_register(gas, bytes, raw);
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
static void initialize_uacpi(void);

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
    initialize_uacpi();
}

static bool aml_package_length(const uint8_t* start, const uint8_t* end,
                               uint32_t* package_length,
                               uint32_t* length_bytes) {
    if (!start || start >= end || !package_length || !length_bytes) {
        return false;
    }
    uint8_t lead = *start;
    uint32_t extra = (lead >> 6) & 3u;
    if (start + 1 + extra > end) return false;
    uint32_t length = lead & (extra ? 0x0Fu : 0x3Fu);
    for (uint32_t index = 0; index < extra; index++) {
        length |= (uint32_t)start[1 + index] << (4u + index * 8u);
    }
    if (length < 1u + extra) return false;
    *package_length = length;
    *length_bytes = 1u + extra;
    return true;
}

static bool aml_read_integer(const uint8_t** cursor, const uint8_t* end,
                             uint64_t* value) {
    if (!cursor || !*cursor || !value || *cursor >= end) return false;
    const uint8_t* p = *cursor;
    uint8_t opcode = *p++;
    switch (opcode) {
        case 0x00:
            *value = 0;
            break;
        case 0x01:
            *value = 1;
            break;
        case 0xFF:
            *value = UINT64_MAX;
            break;
        case 0x0A:
            if (p + 1 > end) return false;
            *value = *p;
            p++;
            break;
        case 0x0B:
            if (p + 2 > end) return false;
            *value = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
            p += 2;
            break;
        case 0x0C:
            if (p + 4 > end) return false;
            *value = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            p += 4;
            break;
        case 0x0E:
            if (p + 8 > end) return false;
            *value = 0;
            for (int index = 0; index < 8; index++) {
                *value |= (uint64_t)p[index] << (index * 8);
            }
            p += 8;
            break;
        default:
            return false;
    }
    *cursor = p;
    return true;
}

static bool parse_s5_from_aml(const uint8_t* aml, uint32_t aml_len) {
    if (!aml || aml_len < 10) return false;
    const uint8_t* end = aml + aml_len;
    for (uint32_t i = 0; i + 9 < aml_len; i++) {
        if (aml[i] != '_' || aml[i + 1] != 'S' ||
            aml[i + 2] != '5' || aml[i + 3] != '_' ||
            aml[i + 4] != 0x12) {
            continue;
        }
        bool name_op = i > 0 && aml[i - 1] == 0x08;
        bool root_name = i > 1 && aml[i - 2] == 0x08 &&
                         aml[i - 1] == '\\';
        if (!name_op && !root_name) continue;

        uint32_t package_length;
        uint32_t length_bytes;
        const uint8_t* length_start = &aml[i + 5];
        if (!aml_package_length(length_start, end, &package_length,
                                &length_bytes)) {
            continue;
        }
        const uint8_t* package_end = length_start + package_length;
        if (package_end > end) continue;
        const uint8_t* cursor = length_start + length_bytes;
        if (cursor >= package_end) continue;
        uint8_t element_count = *cursor++;
        if (element_count == 0 || element_count > 4) continue;

        uint64_t slp_typa;
        uint64_t slp_typb;
        if (!aml_read_integer(&cursor, package_end, &slp_typa)) continue;
        if (element_count > 1) {
            if (!aml_read_integer(&cursor, package_end, &slp_typb)) continue;
        } else {
            slp_typb = slp_typa >> 8;
        }

        g_slp_typa = (uint16_t)((slp_typa & 0x07u) << 10);
        g_slp_typb = (uint16_t)((slp_typb & 0x07u) << 10);
        g_s5_valid = true;
        g_s5_from_uacpi = false;
        log_printf("[ACPI] AML fallback found _S5_: SLP_TYPa=%u (0x%04x) "
                   "SLP_TYPb=%u (0x%04x)\n",
                   (unsigned int)(slp_typa & 0x07u), g_slp_typa,
                   (unsigned int)(slp_typb & 0x07u), g_slp_typb);
        return true;
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
    memset(&g_pm1a_cnt_gas, 0, sizeof(g_pm1a_cnt_gas));
    memset(&g_pm1b_cnt_gas, 0, sizeof(g_pm1b_cnt_gas));
    if (g_fadt_pm1a_cnt) {
        g_pm1a_cnt_gas.address_space = 1;
        g_pm1a_cnt_gas.bit_width = 16;
        g_pm1a_cnt_gas.access_width = 2;
        g_pm1a_cnt_gas.address = g_fadt_pm1a_cnt;
    }
    if (g_fadt_pm1b_cnt) {
        g_pm1b_cnt_gas.address_space = 1;
        g_pm1b_cnt_gas.bit_width = 16;
        g_pm1b_cnt_gas.access_width = 2;
        g_pm1b_cnt_gas.address = g_fadt_pm1b_cnt;
    }

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
    if (facp->length >= 184) {
        acpi_gas_t gas;
        memset(&gas, 0, sizeof(gas));
        gas.address_space = raw[172];
        gas.bit_width = raw[173];
        gas.bit_offset = raw[174];
        gas.access_width = raw[175];
        gas.address = *(const uint64_t*)(raw + 176);
        if (acpi_gas_valid(&gas)) {
            g_pm1a_cnt_gas = gas;
            if (gas.address_space == 1) {
                g_fadt_pm1a_cnt = (uint32_t)gas.address;
            }
        }
    }
    if (facp->length >= 196) {
        acpi_gas_t gas;
        memset(&gas, 0, sizeof(gas));
        gas.address_space = raw[184];
        gas.bit_width = raw[185];
        gas.bit_offset = raw[186];
        gas.access_width = raw[187];
        gas.address = *(const uint64_t*)(raw + 188);
        if (acpi_gas_valid(&gas)) {
            g_pm1b_cnt_gas = gas;
            if (gas.address_space == 1) {
                g_fadt_pm1b_cnt = (uint32_t)gas.address;
            }
        }
    }

    log_printf("[ACPI] FADT: PM1a_CNT=0x%04x PM1b_CNT=0x%04x SMI_CMD=0x%04x DSDT=0x%llx\n",
               g_fadt_pm1a_cnt, g_fadt_pm1b_cnt, g_fadt_smi_cmd,
               (unsigned long long)dsdt_phys);
    if (g_pm1a_cnt_gas.address) {
        log_printf("[ACPI] PM1a_CNT GAS: space=%u width=%u offset=%u "
                   "access=%u address=0x%llx\n",
                   g_pm1a_cnt_gas.address_space,
                   g_pm1a_cnt_gas.bit_width,
                   g_pm1a_cnt_gas.bit_offset,
                   g_pm1a_cnt_gas.access_width,
                   (unsigned long long)g_pm1a_cnt_gas.address);
    }

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

static bool evaluate_s5_with_uacpi(void) {
    uacpi_object* object = NULL;
    uacpi_status status = uacpi_eval_simple_package(
        uacpi_namespace_root(), "_S5_", &object);
    if (status != UACPI_STATUS_OK) {
        log_printf("[uACPI] _S5_ evaluation failed: %s\n",
                   uacpi_status_to_string(status));
        return false;
    }

    uacpi_object_array package;
    memset(&package, 0, sizeof(package));
    status = uacpi_object_get_package(object, &package);
    if (status != UACPI_STATUS_OK || package.count == 0 ||
        package.count > 4) {
        uacpi_object_unref(object);
        log_printf("[uACPI] _S5_ is not a valid package\n");
        return false;
    }

    uacpi_u64 slp_typa;
    uacpi_u64 slp_typb;
    if (uacpi_object_get_integer(package.objects[0], &slp_typa) !=
        UACPI_STATUS_OK) {
        uacpi_object_unref(object);
        log_printf("[uACPI] _S5_[0] is not an integer\n");
        return false;
    }
    if (package.count > 1) {
        if (uacpi_object_get_integer(package.objects[1], &slp_typb) !=
            UACPI_STATUS_OK) {
            uacpi_object_unref(object);
            log_printf("[uACPI] _S5_[1] is not an integer\n");
            return false;
        }
    } else {
        slp_typb = slp_typa >> 8;
    }

    g_slp_typa = (uint16_t)((slp_typa & 0x07u) << 10);
    g_slp_typb = (uint16_t)((slp_typb & 0x07u) << 10);
    g_s5_valid = true;
    g_s5_from_uacpi = true;
    log_printf("[uACPI] _S5_ namespace package: SLP_TYPa=%u (0x%04x) "
               "SLP_TYPb=%u (0x%04x)\n",
               (unsigned int)(slp_typa & 0x07u), g_slp_typa,
               (unsigned int)(slp_typb & 0x07u), g_slp_typb);
    uacpi_object_unref(object);
    return true;
}

static void initialize_uacpi(void) {
    if (!g_uacpi_enabled) {
        log_printf("[uACPI] Disabled by boot option; AML fallback only.\n");
        return;
    }

    uacpi_status status = uacpi_initialize(0);
    if (status != UACPI_STATUS_OK) {
        log_printf("[uACPI] Initialization failed: %s\n",
                   uacpi_status_to_string(status));
        return;
    }
    log_printf("[uACPI] Core initialized (v%u.%u.%u), ACPI mode active\n",
               UACPI_MAJOR, UACPI_MINOR, UACPI_PATCH);

    status = uacpi_namespace_load();
    if (status != UACPI_STATUS_OK) {
        log_printf("[uACPI] DSDT/SSDT namespace load failed: %s\n",
                   uacpi_status_to_string(status));
        return;
    }
    g_uacpi_namespace_ready = true;
    log_printf("[uACPI] DSDT/SSDT namespace loaded\n");
    (void)evaluate_s5_with_uacpi();

    status = uacpi_namespace_initialize();
    if (status != UACPI_STATUS_OK) {
        log_printf("[uACPI] Namespace _INI initialization failed: %s\n",
                   uacpi_status_to_string(status));
        return;
    }
    g_uacpi_sleep_ready = true;
    log_printf("[uACPI] Namespace initialized; S5 sleep backend ready\n");
}

void acpi_set_uacpi_enabled(bool enabled) {
    g_uacpi_enabled = enabled;
}

bool acpi_uacpi_available(void) {
    return g_uacpi_namespace_ready;
}

static bool run_gas_register_self_test(void) {
    phys_addr_t physical = pmm_alloc_page();
    if (!physical) return false;
    volatile uint32_t* memory =
        (volatile uint32_t*)phys_to_virt(physical);
    *memory = 0;

    acpi_gas_t gas;
    memset(&gas, 0, sizeof(gas));
    gas.address_space = 0;
    gas.bit_width = 16;
    gas.access_width = 2;
    gas.address = physical;
    bool okay = acpi_gas_write(&gas, 0xA5C3);
    uint32_t readback = 0;
    okay = okay && acpi_gas_read(&gas, &readback) &&
           readback == 0xA5C3;

    gas.bit_width = 4;
    gas.bit_offset = 4;
    okay = okay && acpi_gas_write(&gas, 0x9);
    readback = 0;
    okay = okay && acpi_gas_read(&gas, &readback) && readback == 0x9;
    okay = okay && ((*memory & 0x00F0u) == 0x0090u);
    pmm_free_page(physical);
    return okay;
}

bool acpi_run_gas_self_test(void) {
    bool okay = run_gas_register_self_test();
    log_printf("[ACPI] FADT GAS I/O/MMIO helper self-test: %s\n",
               okay ? "PASS" : "FAIL");
    return okay;
}

void acpi_shutdown(void) {
    uint16_t slp_en = (uint16_t)(1u << 13);

    if (g_uacpi_enabled && g_uacpi_sleep_ready) {
        log_printf("[ACPI] Requesting S5 through uACPI namespace/FADT path\n");
        uacpi_status status =
            uacpi_enter_sleep_state_simple(UACPI_SLEEP_STATE_S5);
        log_printf("[ACPI] uACPI S5 returned: %s\n",
                   uacpi_status_to_string(status));
    }

    if (!g_s5_valid) {
        log_printf("[ACPI] Refusing PM1 writes: no validated _S5 package\n");
        return;
    }
    if (!acpi_gas_valid(&g_pm1a_cnt_gas)) {
        log_printf("[ACPI] Refusing PM1 writes: PM1a control GAS unavailable\n");
        return;
    }

    uint32_t control = 0;
    if (acpi_gas_read(&g_pm1a_cnt_gas, &control) &&
        (control & 1u) == 0 &&
        g_fadt_smi_cmd != 0 && g_fadt_acpi_enable != 0) {
        outb((uint16_t)g_fadt_smi_cmd, g_fadt_acpi_enable);
        for (int index = 0; index < 300000; index++) {
            if (acpi_gas_read(&g_pm1a_cnt_gas, &control) &&
                (control & 1u)) {
                break;
            }
            inb(0x80);
        }
    }

    log_printf("[ACPI] Fallback S5: %s PM1a GAS=0x%llx "
               "SLP_TYPa=0x%04x SLP_TYPb=0x%04x source=%s\n",
               g_pm1a_cnt_gas.address_space == 1 ? "I/O" : "MMIO",
               (unsigned long long)g_pm1a_cnt_gas.address,
               g_slp_typa, g_slp_typb,
               g_s5_from_uacpi ? "uACPI-evaluated" : "AML-pattern");
    if (!acpi_gas_write(&g_pm1a_cnt_gas, g_slp_typa)) {
        log_printf("[ACPI] Failed to write PM1a SLP_TYP\n");
        return;
    }
    if (acpi_gas_valid(&g_pm1b_cnt_gas)) {
        acpi_gas_write(&g_pm1b_cnt_gas, g_slp_typb);
    }
    acpi_gas_write(&g_pm1a_cnt_gas,
                   (uint16_t)(g_slp_typa | slp_en));
    if (acpi_gas_valid(&g_pm1b_cnt_gas)) {
        acpi_gas_write(&g_pm1b_cnt_gas,
                       (uint16_t)(g_slp_typb | slp_en));
    }
    for (int index = 0; index < 100000; index++) inb(0x80);
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
