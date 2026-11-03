#ifndef MMIO_H
#define MMIO_H

#include <stdint.h>
#include <stddef.h>
#include "mm.h"
#include "vmm.h"

static inline void mmio_barrier(void) {
    __asm__ __volatile__("" : : : "memory");
}

static inline uint8_t mmio_read8(volatile const void* address) {
    mmio_barrier();
    uint8_t value = *(volatile const uint8_t*)address;
    mmio_barrier();
    return value;
}

static inline uint16_t mmio_read16(volatile const void* address) {
    mmio_barrier();
    uint16_t value = *(volatile const uint16_t*)address;
    mmio_barrier();
    return value;
}

static inline uint32_t mmio_read32(volatile const void* address) {
    mmio_barrier();
    uint32_t value = *(volatile const uint32_t*)address;
    mmio_barrier();
    return value;
}

static inline void mmio_write8(volatile void* address, uint8_t value) {
    mmio_barrier();
    *(volatile uint8_t*)address = value;
    mmio_barrier();
}

static inline void mmio_write16(volatile void* address, uint16_t value) {
    mmio_barrier();
    *(volatile uint16_t*)address = value;
    mmio_barrier();
}

static inline void mmio_write32(volatile void* address, uint32_t value) {
    mmio_barrier();
    *(volatile uint32_t*)address = value;
    mmio_barrier();
}

static inline uint8_t mmio_read8_offset(volatile const void* base, uintptr_t offset) {
    return mmio_read8((volatile const uint8_t*)base + offset);
}

static inline uint16_t mmio_read16_offset(volatile const void* base, uintptr_t offset) {
    return mmio_read16((volatile const uint8_t*)base + offset);
}

static inline uint32_t mmio_read32_offset(volatile const void* base, uintptr_t offset) {
    return mmio_read32((volatile const uint8_t*)base + offset);
}

static inline void mmio_write8_offset(volatile void* base, uintptr_t offset, uint8_t value) {
    mmio_write8((volatile uint8_t*)base + offset, value);
}

static inline void mmio_write16_offset(volatile void* base, uintptr_t offset, uint16_t value) {
    mmio_write16((volatile uint8_t*)base + offset, value);
}

static inline void mmio_write32_offset(volatile void* base, uintptr_t offset, uint32_t value) {
    mmio_write32((volatile uint8_t*)base + offset, value);
}

static inline volatile void* mmio_map_phys(uint64_t phys, size_t size) {
    if (!phys || !size || g_hhdm.hhdm_offset == 0) return NULL;
    uint64_t virt_base = g_hhdm.hhdm_offset + phys;
    uint64_t start_page = phys & PAGE_MASK;
    uint64_t end_page = (phys + size + PAGE_SIZE - 1) & PAGE_MASK;
    pagetable_t pml4 = vmm_get_kernel_pml4();
    if (pml4) {
        for (uint64_t p = start_page; p < end_page; p += PAGE_SIZE) {
            vmm_map_page(pml4, g_hhdm.hhdm_offset + p, p,
                         PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_NX);
        }
    }
    return (volatile void*)(uintptr_t)virt_base;
}

static inline volatile void* mmio_map_wc(uint64_t phys, size_t size) {
    if (!phys || !size || g_hhdm.hhdm_offset == 0) return NULL;
    uint64_t virt_base = g_hhdm.hhdm_offset + phys;
    uint64_t start_page = phys & PAGE_MASK;
    uint64_t end_page = (phys + size + PAGE_SIZE - 1) & PAGE_MASK;
    pagetable_t pml4 = vmm_get_kernel_pml4();
    if (pml4) {
        for (uint64_t p = start_page; p < end_page; p += PAGE_SIZE) {
            vmm_map_page(pml4, g_hhdm.hhdm_offset + p, p,
                         PTE_PRESENT | PTE_WRITABLE | PTE_PWT | PTE_NX);
        }
    }
    return (volatile void*)(uintptr_t)virt_base;
}

#endif
