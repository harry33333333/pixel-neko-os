#ifndef _VMM_H
#define _VMM_H

#include "mm.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITABLE  (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)  // Page Write-Through (selects PAT Entry 1 when PCD=0, PAT=0)
#define PTE_PCD       (1ULL << 4)  // Page Cache-Disable
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_LARGE_PAGE (1ULL << 7)  // Page Size (PS) bit in PDPTE / PDE (1GB / 2MB)
#define PTE_PAT_4KB    (1ULL << 7)  // Page Attribute Table bit in 4KB PTE
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_NX         (1ULL << 63) // No-Execute bit
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

// Memory caching flags
#define VMM_CACHE_DEFAULT 0
#define VMM_CACHE_WC      (PTE_PWT) // Selects Write-Combining via PAT Entry 1

typedef uint64_t* pagetable_t;

void vmm_init(void);
pagetable_t vmm_get_kernel_pml4(void);
pagetable_t vmm_create_address_space(void);
bool vmm_map_page(pagetable_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags);
bool vmm_map_range(pagetable_t pml4, virt_addr_t virt, phys_addr_t phys, size_t size, uint64_t flags);
bool vmm_unmap_page(pagetable_t pml4, virt_addr_t virt);
bool vmm_virt_to_phys(pagetable_t pml4, virt_addr_t virt, phys_addr_t* phys);
bool vmm_user_range_ok(pagetable_t pml4, virt_addr_t virt, size_t size, bool write);
void vmm_switch_pml4(pagetable_t pml4);
void vmm_invlpg(virt_addr_t virt);
bool vmm_remap_framebuffer_wc(void);

#endif /* _VMM_H */
