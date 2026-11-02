#include "pci.h"
#include "acpi.h"
#include "device.h"
#include "mm.h"
#include "vmm.h"
#include "port.h"
#include "log.h"
#include "string.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static pci_device_t g_pci_devices[PCI_MAX_DEVICES];
static uint32_t g_pci_device_count = 0;
static bool g_pci_scanned = false;
static bool g_ecam_active = false;
static uint64_t g_ecam_pages_mapped = 0;
static uint64_t g_last_ecam_page = UINT64_MAX;

static const acpi_ecam_range_t* ecam_find_range(uint8_t bus) {
    for (size_t i = 0; i < acpi_get_ecam_range_count(); i++) {
        const acpi_ecam_range_t* range = acpi_get_ecam_range(i);
        if (range && range->pci_segment == 0 &&
            bus >= range->start_bus && bus <= range->end_bus) {
            return range;
        }
    }
    return NULL;
}

static uint32_t pci_port_read(uint8_t bus, uint8_t slot,
                              uint8_t function, uint8_t offset) {
    uint32_t address = (1u << 31) |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) |
                       (offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_port_write(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset, uint32_t value) {
    uint32_t address = (1u << 31) |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)slot << 11) |
                       ((uint32_t)function << 8) |
                       (offset & 0xFCu);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

static bool ecam_address(uint8_t bus, uint8_t slot, uint8_t function,
                         uint8_t offset, uint64_t* phys_out) {
    const acpi_ecam_range_t* range = ecam_find_range(bus);
    if (!range || !phys_out || slot >= 32 || function >= 8) return false;

    uint64_t device_offset =
        ((uint64_t)(bus - range->start_bus) * 256ULL +
         (uint64_t)slot * 8ULL + function) * PAGE_SIZE;
    device_offset += offset & 0xFCu;
    if (device_offset > UINT64_MAX - range->base_address) return false;
    *phys_out = range->base_address + device_offset;
    return true;
}

static bool ecam_ensure_mapped(uint64_t phys) {
    if (g_hhdm.hhdm_offset == 0) return false;
    uint64_t page = phys & PAGE_MASK;
    if (page == g_last_ecam_page) return true;
    uint64_t virt = g_hhdm.hhdm_offset + page;
    if (!vmm_map_page(vmm_get_kernel_pml4(), virt, page,
                      PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_NX)) {
        return false;
    }
    g_ecam_pages_mapped++;
    g_last_ecam_page = page;
    return true;
}

static uint32_t pci_ecam_read(uint8_t bus, uint8_t slot,
                              uint8_t function, uint8_t offset) {
    uint64_t phys = 0;
    if (!ecam_address(bus, slot, function, offset, &phys) ||
        !ecam_ensure_mapped(phys)) {
        return pci_port_read(bus, slot, function, offset);
    }
    return *(volatile uint32_t*)phys_to_virt(phys);
}

static void pci_ecam_write(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset, uint32_t value) {
    uint64_t phys = 0;
    if (!ecam_address(bus, slot, function, offset, &phys) ||
        !ecam_ensure_mapped(phys)) {
        pci_port_write(bus, slot, function, offset, value);
        return;
    }
    *(volatile uint32_t*)phys_to_virt(phys) = value;
}

uint32_t pci_config_read(uint8_t bus, uint8_t slot,
                         uint8_t function, uint8_t offset) {
    return pci_ecam_read(bus, slot, function, offset);
}

void pci_config_write(uint8_t bus, uint8_t slot, uint8_t function,
                      uint8_t offset, uint32_t value) {
    pci_ecam_write(bus, slot, function, offset, value);
}

const char* pci_get_backend_name(void) {
    return g_ecam_active ? "PCIe ECAM" : "PCI I/O CF8/CFC";
}

bool pci_ecam_active(void) {
    return g_ecam_active;
}

static bool pci_probe_bar(uint8_t bus, uint8_t slot, uint8_t function,
                          uint8_t index, pci_bar_t* bar) {
    if (!bar) return false;
    memset(bar, 0, sizeof(*bar));
    bar->index = index;

    uint8_t offset = (uint8_t)(0x10 + index * 4);
    uint32_t original_low = pci_config_read(bus, slot, function, offset);
    if (original_low == 0 || original_low == UINT32_MAX) return false;

    bar->is_io = (original_low & 1u) != 0;
    uint32_t type = (original_low >> 1) & 0x3u;
    bar->is_64bit = !bar->is_io && type == 2u;
    bar->prefetchable = !bar->is_io && type == 1u;
    if (!bar->is_io && type == 3u) return false;
    if (bar->is_64bit && index + 1 >= PCI_BAR_COUNT) return false;

    uint32_t original_high = 0;
    uint32_t mask_high = UINT32_MAX;
    if (bar->is_64bit) {
        original_high = pci_config_read(bus, slot, function,
                                        (uint8_t)(offset + 4));
    }

    uint16_t command = (uint16_t)pci_config_read(bus, slot, function, 0x04);
    pci_config_write(bus, slot, function, 0x04,
                     (uint32_t)command & ~3u);

    if (bar->is_64bit) {
        pci_config_write(bus, slot, function, (uint8_t)(offset + 4),
                         UINT32_MAX);
        mask_high = pci_config_read(bus, slot, function,
                                    (uint8_t)(offset + 4));
        pci_config_write(bus, slot, function, (uint8_t)(offset + 4),
                         original_high);
    }

    pci_config_write(bus, slot, function, offset, UINT32_MAX);
    uint32_t mask_low = pci_config_read(bus, slot, function, offset);
    pci_config_write(bus, slot, function, offset, original_low);
    pci_config_write(bus, slot, function, 0x04, command);

    uint64_t size = 0;
    if (bar->is_io) {
        mask_low &= 0xFFFFu & ~3u;
        if (mask_low == 0) return false;
        size = (uint64_t)(~mask_low & 0xFFFFu) + 1u;
        bar->address = original_low & ~3u;
    } else if (bar->is_64bit) {
        uint64_t mask = ((uint64_t)mask_high << 32) |
                        (mask_low & ~0xFu);
        size = ~mask + 1u;
        bar->address = ((uint64_t)original_high << 32) |
                       (original_low & ~0xFu);
    } else {
        mask_low &= ~0xFu;
        if (mask_low == 0) return false;
        size = (uint64_t)(~mask_low) + 1u;
        bar->address = original_low & ~0xFu;
    }

    if (size == 0 || (size & (size - 1)) != 0) return false;
    bar->size = size;
    bar->valid = true;
    return true;
}

static void pci_name(char* output, size_t output_size, uint8_t bus,
                     uint8_t slot, uint8_t function) {
    static const char hex[] = "0123456789abcdef";
    if (output_size < 9) {
        if (output_size) output[0] = '\0';
        return;
    }
    output[0] = 'p';
    output[1] = 'c';
    output[2] = 'i';
    output[3] = hex[bus >> 4];
    output[4] = hex[bus & 0xF];
    output[5] = ':';
    output[6] = hex[slot >> 4];
    output[7] = hex[slot & 0xF];
    if (output_size >= 11) {
        output[8] = '.';
        output[9] = hex[function & 0xF];
        output[10] = '\0';
    } else {
        output[8] = '\0';
    }
}

static void pci_add_function(uint8_t bus, uint8_t slot, uint8_t function) {
    if (g_pci_device_count >= PCI_MAX_DEVICES) return;
    uint32_t id = pci_config_read(bus, slot, function, 0x00);
    if (id == 0 || id == UINT32_MAX) return;

    pci_device_t* device = &g_pci_devices[g_pci_device_count];
    memset(device, 0, sizeof(*device));
    device->segment = 0;
    device->bus = bus;
    device->slot = slot;
    device->function = function;
    device->vendor = (uint16_t)(id & 0xFFFFu);
    device->device = (uint16_t)(id >> 16);

    uint32_t class_revision = pci_config_read(bus, slot, function, 0x08);
    device->revision = (uint8_t)class_revision;
    device->prog_if = (uint8_t)(class_revision >> 8);
    device->subclass = (uint8_t)(class_revision >> 16);
    device->class_code = (uint8_t)(class_revision >> 24);

    uint32_t header = pci_config_read(bus, slot, function, 0x0C);
    device->header_type = (uint8_t)(header >> 16);
    device->command = (uint16_t)pci_config_read(bus, slot, function, 0x04);

    uint32_t irq = pci_config_read(bus, slot, function, 0x3C);
    device->irq_line = (uint8_t)irq;
    device->irq_pin = (uint8_t)(irq >> 8);

    if ((device->header_type & 0x7Fu) == 0) {
        uint32_t subsystem = pci_config_read(bus, slot, function, 0x2C);
        device->subsystem_vendor = (uint16_t)subsystem;
        device->subsystem_device = (uint16_t)(subsystem >> 16);
    }

    for (uint8_t i = 0; i < PCI_BAR_COUNT; i++) {
        pci_probe_bar(bus, slot, function, i, &device->bar[i]);
        if (device->bar[i].is_64bit) i++;
    }

    char name[16];
    pci_name(name, sizeof(name), bus, slot, function);
    bus_t* pci_bus = bus_find(BUS_TYPE_PCI);
    device_t* model = device_register(pci_bus, name,
                                      device->vendor, device->device,
                                      device->class_code, device->subclass,
                                      device);
    if (model) {
        model->bus_number = bus;
        model->slot = slot;
        model->function = function;
        model->subsystem_vendor_id = device->subsystem_vendor;
        model->subsystem_device_id = device->subsystem_device;
        model->prog_if = device->prog_if;
        model->revision = device->revision;
        model->irq = device->irq_line;

        if (device->irq_line != 0xFF && device->irq_pin != 0) {
            device_add_resource(model, DEVICE_RESOURCE_IRQ,
                                device->irq_line, 1, true);
        }
        for (uint8_t bar_index = 0; bar_index < PCI_BAR_COUNT; bar_index++) {
            pci_bar_t* bar = &device->bar[bar_index];
            if (!bar->valid || bar->address == 0) continue;
            bool reserved = device_add_resource(
                model,
                bar->is_io ? DEVICE_RESOURCE_IO : DEVICE_RESOURCE_MEMORY,
                bar->address, bar->size, false);
            bar->resource_conflict = !reserved;
        }
    }

    log_printf("[PCI ] %02x:%02x.%x %04x:%04x class=%02x:%02x rev=%02x irq=%u cmd=%04x (%s)\n",
               bus, slot, function, device->vendor, device->device,
               device->class_code, device->subclass, device->revision,
               device->irq_line, device->command,
               pci_class_name(device->class_code, device->subclass));

    for (uint8_t i = 0; i < PCI_BAR_COUNT; i++) {
        pci_bar_t* bar = &device->bar[i];
        if (!bar->valid) continue;
        log_printf("[PCI ]   BAR%u %s%s base=0x%016llx size=0x%llx%s\n",
                   i, bar->is_io ? "I/O" : "MEM",
                   bar->is_64bit ? "64" : "",
                   (unsigned long long)bar->address,
                   (unsigned long long)bar->size,
                   bar->resource_conflict ? " CONFLICT" : "");
    }

    g_pci_device_count++;
}

void pci_scan_all(void) {
    if (g_pci_scanned) return;

    g_pci_device_count = 0;
    g_ecam_active = acpi_get_ecam_range_count() > 0;
    g_ecam_pages_mapped = 0;
    g_last_ecam_page = UINT64_MAX;
    bus_t* pci_bus = bus_find(BUS_TYPE_PCI);
    if (!pci_bus) pci_bus = bus_register("pci", BUS_TYPE_PCI);
    if (!pci_bus) {
        log_printf("[PCI ] Failed to register PCI bus.\n");
        return;
    }

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_config_read((uint8_t)bus, (uint8_t)slot, 0, 0);
            if (id == 0 || id == UINT32_MAX) continue;

            uint32_t header = pci_config_read((uint8_t)bus,
                                              (uint8_t)slot, 0, 0x0C);
            uint32_t function_count =
                (header & (1u << 23)) ? 8u : 1u;
            for (uint32_t function = 0; function < function_count; function++) {
                pci_add_function((uint8_t)bus, (uint8_t)slot,
                                 (uint8_t)function);
            }
        }
    }

    g_pci_scanned = true;
    log_printf("[PCI ] Backend: %s, ECAM ranges=%llu, ECAM pages mapped=%llu\n",
               g_ecam_active ? "PCIe ECAM" : "legacy CF8/CFC",
               (unsigned long long)acpi_get_ecam_range_count(),
               (unsigned long long)g_ecam_pages_mapped);
    log_printf("[PCI ] Enumerated %u PCI/PCIe function(s).\n",
               g_pci_device_count);
}

bool pci_has_scanned(void) {
    return g_pci_scanned;
}

int pci_get_device_count(void) {
    if (!g_pci_scanned) pci_scan_all();
    return (int)g_pci_device_count;
}

const pci_device_t* pci_get_device(int index) {
    if (!g_pci_scanned) pci_scan_all();
    if (index < 0 || index >= (int)g_pci_device_count) return NULL;
    return &g_pci_devices[index];
}

const pci_device_t* pci_find_by_id(uint16_t vendor, uint16_t device) {
    if (!g_pci_scanned) pci_scan_all();
    for (uint32_t i = 0; i < g_pci_device_count; i++) {
        if (g_pci_devices[i].vendor == vendor &&
            g_pci_devices[i].device == device) {
            return &g_pci_devices[i];
        }
    }
    return NULL;
}

const pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass) {
    if (!g_pci_scanned) pci_scan_all();
    for (uint32_t i = 0; i < g_pci_device_count; i++) {
        if (g_pci_devices[i].class_code == class_code &&
            g_pci_devices[i].subclass == subclass) {
            return &g_pci_devices[i];
        }
    }
    return NULL;
}

static uint16_t pci_update_command(const pci_device_t* device,
                                   uint16_t set_bits) {
    if (!device) return 0;
    uint16_t command = (uint16_t)pci_config_read(
        device->bus, device->slot, device->function, 0x04);
    command |= set_bits;
    pci_config_write(device->bus, device->slot, device->function, 0x04,
                     command);
    return command;
}

uint16_t pci_enable_io(const pci_device_t* dev) {
    return pci_update_command(dev, 0x0001u);
}

uint16_t pci_enable_memory(const pci_device_t* dev) {
    return pci_update_command(dev, 0x0002u);
}

uint16_t pci_enable_bus_master(const pci_device_t* dev) {
    return pci_update_command(dev, 0x0004u);
}

uint64_t pci_get_bar_size(const pci_device_t* dev, int bar) {
    if (!dev || bar < 0 || bar >= PCI_BAR_COUNT || !dev->bar[bar].valid) {
        return 0;
    }
    return dev->bar[bar].size;
}

uint64_t pci_get_bar_address(const pci_device_t* dev, int bar) {
    if (!dev || bar < 0 || bar >= PCI_BAR_COUNT || !dev->bar[bar].valid) {
        return 0;
    }
    return dev->bar[bar].address;
}

uint8_t pci_find_capability(const pci_device_t* dev, uint8_t capability) {
    if (!dev) return 0;

    uint16_t status = (uint16_t)(pci_config_read(
        dev->bus, dev->slot, dev->function, 0x04) >> 16);
    if (!(status & (1u << 4))) return 0;

    uint32_t cap_word = pci_config_read(dev->bus, dev->slot,
                                        dev->function, 0x34);
    uint8_t pointer = (uint8_t)(cap_word & 0xFCu);
    for (int i = 0; i < 48 && pointer >= 0x40; i++) {
        uint32_t header = pci_config_read(dev->bus, dev->slot,
                                          dev->function, pointer);
        uint8_t id = (uint8_t)(header >> ((pointer & 3) * 8));
        if (id == capability) return pointer;
        uint8_t next_offset = (uint8_t)((pointer & 3) + 1);
        uint8_t next = (uint8_t)(header >> (next_offset * 8));
        pointer = next & 0xFCu;
        if (!pointer) break;
    }
    return 0;
}

const char* pci_class_name(uint8_t class_code, uint8_t subclass) {
    if (class_code == 0x00) return "Unclassified";
    if (class_code == 0x01 && subclass == 0x01) return "IDE Controller";
    if (class_code == 0x01 && subclass == 0x06) return "SATA/AHCI";
    if (class_code == 0x02) return "Network Controller";
    if (class_code == 0x03) return "Display Controller";
    if (class_code == 0x04 && subclass == 0x03) return "HD Audio";
    if (class_code == 0x04) return "Multimedia";
    if (class_code == 0x06) return "Bridge";
    if (class_code == 0x0C && subclass == 0x03) return "USB Controller";
    return "Other";
}
