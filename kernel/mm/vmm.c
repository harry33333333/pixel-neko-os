#include "vmm.h"
#include "pmm.h"
#include "log.h"
#include "string.h"
#include "boot_framebuffer.h"

#define IA32_PAT_MSR 0x277

static pagetable_t g_kernel_pml4 = NULL;

static inline void rdmsr(uint32_t msr, uint32_t* lo, uint32_t* hi) {
    __asm__ volatile ("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(msr));
}

static inline void wrmsr(uint32_t msr, uint32_t lo, uint32_t hi) {
    __asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

void vmm_invlpg(virt_addr_t virt) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_switch_pml4(pagetable_t pml4) {
    phys_addr_t phys = 0;
    if (virt_to_phys_checked(pml4, &phys)) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(phys) : "memory");
    }
}

pagetable_t vmm_get_kernel_pml4(void) {
    return g_kernel_pml4;
}

static pagetable_t get_next_level(pagetable_t table, uint64_t index, bool allocate, uint64_t flags) {
    uint64_t entry = table[index];
    if (entry & PTE_PRESENT) {
        if (entry & PTE_LARGE_PAGE) {
            return NULL; // Already mapped as large page (1GB / 2MB)
        }
        phys_addr_t phys = entry & ~0xFFFULL;
        return (pagetable_t)phys_to_virt(phys);
    }

    if (!allocate) return NULL;

    phys_addr_t new_table_phys = pmm_alloc_page();
    if (!new_table_phys) return NULL;

    pagetable_t new_table = (pagetable_t)phys_to_virt(new_table_phys);
    memset(new_table, 0, PAGE_SIZE);

    table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | (flags & PTE_USER);
    return new_table;
}

bool vmm_map_page(pagetable_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags) {
    if (!pml4) return false;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    pagetable_t pdpt = get_next_level(pml4, pml4_idx, true, flags);
    if (!pdpt) return false;

    pagetable_t pd = get_next_level(pdpt, pdpt_idx, true, flags);
    if (!pd) return false;

    pagetable_t pt = get_next_level(pd, pd_idx, true, flags);
    if (!pt) return false;

    pt[pt_idx] = (phys & ~0xFFFULL) | flags | PTE_PRESENT;
    vmm_invlpg(virt);
    return true;
}

bool vmm_unmap_page(pagetable_t pml4, virt_addr_t virt) {
    if (!pml4) return false;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    pagetable_t pdpt = get_next_level(pml4, pml4_idx, false, 0);
    if (!pdpt) return false;

    pagetable_t pd = get_next_level(pdpt, pdpt_idx, false, 0);
    if (!pd) return false;

    pagetable_t pt = get_next_level(pd, pd_idx, false, 0);
    if (!pt) return false;

    pt[pt_idx] = 0;
    vmm_invlpg(virt);
    return true;
}

void vmm_init(void) {
    // 1. Get current PML4 physical base from CR3
    uint64_t cr3 = 0;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    phys_addr_t pml4_phys = cr3 & ~0xFFFULL;
    g_kernel_pml4 = (pagetable_t)phys_to_virt(pml4_phys);

    log_printf("[VMM] 4-Level Paging initialized. Kernel PML4 @ Phys 0x%016llx, Virt 0x%016llx\n",
               pml4_phys, (uint64_t)g_kernel_pml4);

    // 2. Configure IA32_PAT MSR (0x277): Set Entry 1 (bits 8..15) to Write-Combining (WC = 0x01)
    uint32_t pat_lo = 0, pat_hi = 0;
    rdmsr(IA32_PAT_MSR, &pat_lo, &pat_hi);
    
    // Clear Entry 1 (bits 8..15) and set to 0x01 (WC)
    pat_lo &= ~(0xFF << 8);
    pat_lo |= (0x01 << 8);
    wrmsr(IA32_PAT_MSR, pat_lo, pat_hi);

    log_printf("[VMM] IA32_PAT MSR configured: PAT Entry 1 = 0x01 (Write-Combining WC enabled).\n");

    vmm_remap_framebuffer_wc();
}

void vmm_remap_framebuffer_wc(void) {
    if (!g_kernel_pml4) return;
    if (!g_main_surface.framebuffer_virt || !g_main_surface.framebuffer_phys) return;

    uint64_t fb_size = (uint64_t)g_main_surface.pitch * g_main_surface.height;
    if (fb_size == 0) return;
    uint64_t fb_pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;

    virt_addr_t fb_virt = (virt_addr_t)g_main_surface.framebuffer_virt;
    phys_addr_t fb_phys = (phys_addr_t)g_main_surface.framebuffer_phys;
    if (fb_phys >= g_hhdm.hhdm_offset && g_hhdm.hhdm_offset > 0) {
        fb_phys -= g_hhdm.hhdm_offset;
    }

    // Apply WC flag (PTE_PWT selects PAT entry 1)
    uint64_t wc_flags = PTE_PRESENT | PTE_WRITABLE | PTE_PWT;

    for (uint64_t p = 0; p < fb_pages; p++) {
        vmm_map_page(g_kernel_pml4, fb_virt + p * PAGE_SIZE, fb_phys + p * PAGE_SIZE, wc_flags);
    }

    log_printf("[VMM] Boot Framebuffer (%llu KB, %llu pages) accelerated with PAT Write-Combining (WC)!\n",
               fb_size / 1024, fb_pages);
}

