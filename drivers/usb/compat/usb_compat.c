#include "usb_compat.h"
#include "mm.h"
#include "vmm.h"
#include "log.h"
#include "usb_core.h"

static inline uint64_t usb_read_flags(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags) : : "memory");
    return flags;
}

void usb_recursive_lock(usb_recursive_lock_t* lock) {
    if (!lock) return;
    if (lock->depth++ == 0) {
        lock->irq_flags = usb_read_flags();
        __asm__ volatile ("cli" : : : "memory");
    }
}

void usb_recursive_unlock(usb_recursive_lock_t* lock) {
    if (!lock || lock->depth <= 0) return;
    if (--lock->depth == 0 && (lock->irq_flags & (1ULL << 9))) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

void* usb_map_mmio(uint64_t physical, size_t size) {
    if (!physical || !size || g_hhdm.hhdm_offset == 0) return NULL;
    uint64_t virtual_base = g_hhdm.hhdm_offset + (physical & PAGE_MASK);
    uint64_t end = (physical + size + PAGE_SIZE - 1) & PAGE_MASK;
    uint64_t page = physical & PAGE_MASK;
    while (page < end) {
        vmm_map_page(vmm_get_kernel_pml4(), g_hhdm.hhdm_offset + page, page,
                     PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_NX);
        page += PAGE_SIZE;
    }
    return (void*)virtual_base;
}

uint64_t usb_phys32(const void* virtual_address) {
    uint64_t physical = 0;
    if (!virt_to_phys_checked(virtual_address, &physical) ||
        physical > UINT32_MAX) {
        return UINT64_MAX;
    }
    return physical;
}

int usb_legacy_handoff(const pci_device_t* device) {
    if (!device) return -1;

    /*
     * EHCI (prog_if == 0x20): FreeBSD ehci_pci_take_controller()
     * Read EECP (bits 15:8 of HCCPARAMS at BAR0 + 0x08) and walk extended
     * capabilities in PCI config space looking for USBLEGSUP (ID == 0x01).
     */
    if (device->class_code == 0x0C && device->subclass == 0x03 &&
        device->prog_if == 0x20 && device->bar[0].valid &&
        !device->bar[0].is_io && device->bar[0].address) {
        pci_enable_memory(device);
        pci_enable_bus_master(device);
        volatile uint8_t* mmio = (volatile uint8_t*)usb_map_mmio(
            device->bar[0].address, 0x100);
        if (mmio) {
            uint32_t hccparams = *(volatile uint32_t*)(mmio + 0x08);
            uint8_t eecp = (uint8_t)((hccparams >> 8) & 0xFFu);
            for (int i = 0; i < 48 && eecp >= 0x40; i++) {
                uint32_t legsup = pci_config_read(
                    device->bus, device->slot, device->function, eecp);
                uint8_t cap_id = (uint8_t)(legsup & 0xFFu);
                uint8_t next = (uint8_t)((legsup >> 8) & 0xFFu);
                if (cap_id == 0x01u) {
                    /* Always assert OS Owned semaphore (bit 24) */
                    pci_config_write(device->bus, device->slot,
                                     device->function, eecp,
                                     legsup | (1u << 24));
                    if (legsup & (1u << 16)) {
                        for (int t = 0; t < 1000; t++) {
                            legsup = pci_config_read(
                                device->bus, device->slot,
                                device->function, eecp);
                            if (!(legsup & (1u << 16))) break;
                            usb_delay_ms(1);
                        }
                        if (legsup & (1u << 16)) {
                            /* Force-clear BIOS Owned if SMM did not respond */
                            pci_config_write(device->bus, device->slot,
                                             device->function, eecp,
                                             (legsup & ~(1u << 16)) | (1u << 24));
                        }
                    }
                    /* Clear SMI enables (15:0) and W1C SMI status bits (31:29) in USBLEGCTLSTS */
                    pci_config_write(device->bus, device->slot,
                                     device->function, (uint8_t)(eecp + 4),
                                     0xE0000000u);
                    log_printf("[USB ] EHCI BIOS ownership hand-off complete at EECP 0x%02x\n",
                               eecp);
                    return 0;
                }
                eecp = next;
            }
        }
    }

    uint32_t command_status = pci_config_read(
        device->bus, device->slot, device->function, 0x04);
    if (!(command_status & (1u << 20))) return 0;

    uint8_t pointer = (uint8_t)(pci_config_read(
        device->bus, device->slot, device->function, 0x34) & 0xFCu);
    for (int i = 0; i < 48 && pointer >= 0x40; i++) {
        uint32_t header = pci_config_read(
            device->bus, device->slot, device->function, pointer);
        uint8_t id = (uint8_t)header;
        uint8_t next = (uint8_t)(header >> 8);
        if (id == 0x0A) {
            uint32_t support = pci_config_read(
                device->bus, device->slot, device->function,
                (uint8_t)(pointer + 4));
            if (support & (1u << 16)) {
                pci_config_write(device->bus, device->slot, device->function,
                                 (uint8_t)(pointer + 4),
                                 support | (1u << 24));
                for (int timeout = 0; timeout < 500; timeout++) {
                    support = pci_config_read(
                        device->bus, device->slot, device->function,
                        (uint8_t)(pointer + 4));
                    if (!(support & (1u << 16))) break;
                    usb_delay_ms(1);
                }
                if (support & (1u << 16)) {
                    log_printf("[USB ] BIOS ownership hand-off timed out at cap 0x%02x\n",
                               pointer);
                    return -1;
                }
            }
            log_printf("[USB ] Legacy USB ownership transferred at cap 0x%02x\n",
                       pointer);
            return 0;
        }
        pointer = (uint8_t)(next & 0xFCu);
        if (!pointer || pointer == 0x34) break;
    }
    return 0;
}

int usb_dma_alloc_zero(size_t size, uint64_t mask, size_t alignment,
                       dma_buffer_t* buffer) {
    if (!buffer || dma_alloc(size, mask, alignment, buffer) != 0) return -1;
    __asm__ volatile ("mfence" : : : "memory");
    return 0;
}
