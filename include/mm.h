#ifndef _MM_H
#define _MM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint64_t phys_addr_t;
typedef uint64_t virt_addr_t;
typedef uint64_t dma_addr_t;

#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~(PAGE_SIZE - 1))

typedef struct {
    uint64_t hhdm_offset;
    uint64_t physical_memory_limit; // Maximum addressable physical boundary
    uint64_t total_usable_bytes;
    uint64_t total_memory_bytes;
} hhdm_info_t;

extern hhdm_info_t g_hhdm;

// Physical address to HHDM virtual address with safety checks
static inline void* phys_to_virt(phys_addr_t phys) {
    if (g_hhdm.hhdm_offset == 0) return NULL;
    if (phys > UINT64_MAX - g_hhdm.hhdm_offset) return NULL;
    return (void*)(phys + g_hhdm.hhdm_offset);
}

// Safety-checked virtual address to physical address conversion
static inline bool virt_to_phys_checked(const void* virt, phys_addr_t* out_phys) {
    if (!virt || !out_phys || g_hhdm.hhdm_offset == 0) return false;
    uint64_t v = (uint64_t)virt;
    if (v < g_hhdm.hhdm_offset) return false;
    uint64_t delta = v - g_hhdm.hhdm_offset;
    *out_phys = (phys_addr_t)delta;
    return true;
}

#endif /* _MM_H */
