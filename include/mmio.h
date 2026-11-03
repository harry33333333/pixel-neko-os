#ifndef MMIO_H
#define MMIO_H

#include <stdint.h>
#include <stddef.h>
#include "mm.h"
#include "vmm.h"

static inline void mmio_barrier(void) {
    __asm__ __volatile__("" : : : "memory");
}

static inline uint8_t mmio_read8(volatile const void* base, uintptr_t offset) {
    const volatile uint8_t* reg =
        (const volatile uint8_t*)((uintptr_t)base + offset);
    uint8_t value = *reg;
    mmio_barrier();
    return value;
}

static inline uint16_t mmio_read16(volatile const void* base, uintptr_t offset) {
    const volatile uint16_t* reg =
        (const volatile uint16_t*)((uintptr_t)base + offset);
    uint16_t value = *reg;
    mmio_barrier();
    return value;
}

static inline uint32_t mmio_read32(volatile const void* base, uintptr_t offset) {
    const volatile uint32_t* reg =
        (const volatile uint32_t*)((uintptr_t)base + offset);
    uint32_t value = *reg;
    mmio_barrier();
    return value;
}

static inline void mmio_write8(volatile void* base, uintptr_t offset,
                               uint8_t value) {
    volatile uint8_t* reg = (volatile uint8_t*)((uintptr_t)base + offset);
    mmio_barrier();
    *reg = value;
    mmio_barrier();
}

static inline void mmio_write16(volatile void* base, uintptr_t offset,
                                uint16_t value) {
    volatile uint16_t* reg = (volatile uint16_t*)((uintptr_t)base + offset);
    mmio_barrier();
    *reg = value;
    mmio_barrier();
}

static inline void mmio_write32(volatile void* base, uintptr_t offset,
                                uint32_t value) {
    volatile uint32_t* reg = (volatile uint32_t*)((uintptr_t)base + offset);
    mmio_barrier();
    *reg = value;
    mmio_barrier();
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
