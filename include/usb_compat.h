#ifndef _USB_COMPAT_H
#define _USB_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "dma.h"
#include "pci.h"

typedef struct {
    int depth;
    uint64_t irq_flags;
} usb_recursive_lock_t;

void usb_recursive_lock(usb_recursive_lock_t* lock);
void usb_recursive_unlock(usb_recursive_lock_t* lock);

void* usb_map_mmio(uint64_t physical, size_t size);
int usb_legacy_handoff(const pci_device_t* device);
uint64_t usb_phys32(const void* virtual_address);

int usb_dma_alloc_zero(size_t size, uint64_t mask, size_t alignment,
                       dma_buffer_t* buffer);

#endif
