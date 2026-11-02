#include "vmm.h"
#include "pmm.h"
#include "log.h"
#include "string.h"
#include "boot_framebuffer.h"
#include "limine.h"

#define IA32_PAT_MSR 0x277

extern volatile struct limine_memmap_request memmap_request;

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

pagetable_t vmm_create_address_space(void) {
    if (!g_kernel_pml4) return NULL;

    phys_addr_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) return NULL;

    pagetable_t pml4 = (pagetable_t)phys_to_virt(pml4_phys);
    if (!pml4) {
        pmm_free_page(pml4_phys);
        return NULL;
    }

    memset(pml4, 0, PAGE_SIZE);

    /*
     * Copy only the kernel half. The low half deliberately starts empty so
     * Ring 3 never inherits the P2 identity maps.
     */
    for (uint32_t i = 256; i < 512; i++) {
        pml4[i] = g_kernel_pml4[i];
    }

    return pml4;
}

bool vmm_virt_to_phys(pagetable_t pml4, virt_addr_t virt, phys_addr_t* phys) {
    if (!pml4 || !phys) return false;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t pml4e = pml4[pml4_idx];
    if (!(pml4e & PTE_PRESENT)) return false;
    pagetable_t pdpt = (pagetable_t)phys_to_virt(pml4e & PTE_ADDR_MASK);
    if (!pdpt) return false;

    uint64_t pdpte = pdpt[pdpt_idx];
    if (!(pdpte & PTE_PRESENT)) return false;
    if (pdpte & PTE_LARGE_PAGE) {
        *phys = (pdpte & PTE_ADDR_MASK & ~((1ULL << 30) - 1)) |
                (virt & ((1ULL << 30) - 1));
        return true;
    }

    pagetable_t pd = (pagetable_t)phys_to_virt(pdpte & PTE_ADDR_MASK);
    if (!pd) return false;

    uint64_t pde = pd[pd_idx];
    if (!(pde & PTE_PRESENT)) return false;
    if (pde & PTE_LARGE_PAGE) {
        *phys = (pde & PTE_ADDR_MASK & ~((1ULL << 21) - 1)) |
                (virt & ((1ULL << 21) - 1));
        return true;
    }

    pagetable_t pt = (pagetable_t)phys_to_virt(pde & PTE_ADDR_MASK);
    if (!pt) return false;

    uint64_t pte = pt[pt_idx];
    if (!(pte & PTE_PRESENT)) return false;
    *phys = (pte & PTE_ADDR_MASK) | (virt & 0xFFFULL);
    return true;
}

bool vmm_user_range_ok(pagetable_t pml4, virt_addr_t virt, size_t size, bool write) {
    if (!pml4) return false;
    if (size == 0) return true;
    if (virt >= 0x0000800000000000ULL) return false;
    if (size > UINT64_MAX - virt) return false;
    if (virt + size - 1 >= 0x0000800000000000ULL) return false;

    uint64_t first = virt & PAGE_MASK;
    uint64_t last = (virt + size - 1) & PAGE_MASK;

    for (uint64_t page = first; ; page += PAGE_SIZE) {
        uint64_t pml4_idx = (page >> 39) & 0x1FF;
        uint64_t pdpt_idx = (page >> 30) & 0x1FF;
        uint64_t pd_idx   = (page >> 21) & 0x1FF;
        uint64_t pt_idx   = (page >> 12) & 0x1FF;

        uint64_t pml4e = pml4[pml4_idx];
        if (!(pml4e & PTE_PRESENT) || !(pml4e & PTE_USER)) return false;
        pagetable_t pdpt = (pagetable_t)phys_to_virt(pml4e & PAGE_MASK);
        if (!pdpt) return false;

        uint64_t pdpte = pdpt[pdpt_idx];
        if (!(pdpte & PTE_PRESENT) || !(pdpte & PTE_USER)) return false;
        if (pdpte & PTE_LARGE_PAGE) {
            if (write && !(pdpte & PTE_WRITABLE)) return false;
        } else {
            pagetable_t pd = (pagetable_t)phys_to_virt(pdpte & PAGE_MASK);
            if (!pd) return false;

            uint64_t pde = pd[pd_idx];
            if (!(pde & PTE_PRESENT) || !(pde & PTE_USER)) return false;
            if (pde & PTE_LARGE_PAGE) {
                if (write && !(pde & PTE_WRITABLE)) return false;
            } else {
                pagetable_t pt = (pagetable_t)phys_to_virt(pde & PAGE_MASK);
                if (!pt) return false;
                uint64_t pte = pt[pt_idx];
                if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) return false;
                if (write && !(pte & PTE_WRITABLE)) return false;
            }
        }

        if (page == last) break;
    }

    return true;
}

static pagetable_t get_next_level(pagetable_t table, uint64_t index, bool allocate, uint64_t flags) {
    uint64_t entry = table[index];
    if (entry & PTE_PRESENT) {
        if (entry & PTE_LARGE_PAGE) {
            return NULL; // Already mapped as large page (1GB / 2MB)
        }
        if (flags & PTE_WRITABLE) {
            table[index] |= PTE_WRITABLE;
        }
        if (flags & PTE_USER) {
            table[index] |= PTE_USER;
        }
        phys_addr_t phys = entry & ~0xFFFULL;
        return (pagetable_t)phys_to_virt(phys);
    }

    if (!allocate) return NULL;

    phys_addr_t new_table_phys = pmm_alloc_page();
    if (!new_table_phys) return NULL;

    pagetable_t new_table = (pagetable_t)phys_to_virt(new_table_phys);
    memset(new_table, 0, PAGE_SIZE);

    table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE |
                  (flags & (PTE_USER | PTE_WRITABLE));
    return new_table;
}

bool vmm_map_page(pagetable_t pml4, virt_addr_t virt, phys_addr_t phys, uint64_t flags) {
    if (!pml4) return false;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    pagetable_t pdpt = get_next_level(pml4, pml4_idx, true, flags);
    if (!pdpt) {
        if (pml4[pml4_idx] & PTE_LARGE_PAGE) return true;
        return false;
    }

    if (pdpt[pdpt_idx] & PTE_LARGE_PAGE) {
        return true; // Covered by 1GB large page
    }

    pagetable_t pd = get_next_level(pdpt, pdpt_idx, true, flags);
    if (!pd) return false;

    if (pd[pd_idx] & PTE_LARGE_PAGE) {
        return true; // Covered by 2MB large page
    }

    pagetable_t pt = get_next_level(pd, pd_idx, true, flags);
    if (!pt) return false;

    pt[pt_idx] = (phys & ~0xFFFULL) | flags | PTE_PRESENT;
    vmm_invlpg(virt);
    return true;
}

bool vmm_map_range(pagetable_t pml4, virt_addr_t virt, phys_addr_t phys, size_t size, uint64_t flags) {
    if (!pml4 || size == 0) return false;
    uint64_t page_offset = virt & 0xFFFULL;
    uint64_t start_virt = virt & ~0xFFFULL;
    uint64_t start_phys = phys & ~0xFFFULL;
    uint64_t total_size = size + page_offset;
    uint64_t page_count = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < page_count; i++) {
        if (!vmm_map_page(pml4, start_virt + i * PAGE_SIZE, start_phys + i * PAGE_SIZE, flags)) {
            return false;
        }
    }
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

    // 3. Explicitly map low 1MB BIOS / EBDA / ROM area (0x00001000 .. 0x00100000)
    // Page 0 (0x0..0x1000) is kept unmapped to trap NULL pointers!
    for (uint64_t addr = PAGE_SIZE; addr < 0x100000; addr += PAGE_SIZE) {
        vmm_map_page(g_kernel_pml4, g_hhdm.hhdm_offset + addr, addr, PTE_PRESENT | PTE_WRITABLE);
        vmm_map_page(g_kernel_pml4, addr, addr, PTE_PRESENT | PTE_WRITABLE);
    }
    log_printf("[VMM] Low 1MB (0x00001000 - 0x00100000, 255 pages) mapped to HHDM & Identity.\n");

    // 4. Explicitly map all ACPI_RECLAIMABLE, ACPI_NVS, and RESERVED ranges reported by Limine
    if (memmap_request.response) {
        uint64_t total_mapped_pages = 0;
        for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
            struct limine_memmap_entry* e = memmap_request.response->entries[i];
            if (e->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE ||
                e->type == LIMINE_MEMMAP_ACPI_NVS ||
                e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
                (e->type == LIMINE_MEMMAP_RESERVED && e->length <= 16 * 1024 * 1024)) {
                
                uint64_t start = e->base & ~0xFFFULL;
                uint64_t end = (e->base + e->length + PAGE_SIZE - 1) & ~0xFFFULL;
                for (uint64_t p = start; p < end; p += PAGE_SIZE) {
                    if (p == 0) continue; // Keep NULL trap
                    vmm_map_page(g_kernel_pml4, g_hhdm.hhdm_offset + p, p, PTE_PRESENT | PTE_WRITABLE);
                    total_mapped_pages++;
                }
            }
        }
        log_printf("[VMM] ACPI & System Reserved tables (%llu pages) mapped to HHDM.\n", total_mapped_pages);
    }

    // 5. Explicitly map Local APIC (0xFEE00000) and IOAPIC (0xFEC00000) MMIO pages
    vmm_map_page(g_kernel_pml4, g_hhdm.hhdm_offset + 0xFEE00000, 0xFEE00000, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    vmm_map_page(g_kernel_pml4, 0xFEE00000, 0xFEE00000, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);

    vmm_map_page(g_kernel_pml4, g_hhdm.hhdm_offset + 0xFEC00000, 0xFEC00000, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    vmm_map_page(g_kernel_pml4, 0xFEC00000, 0xFEC00000, PTE_PRESENT | PTE_WRITABLE | PTE_PCD);
    log_printf("[VMM] LAPIC (0xFEE00000) & IOAPIC (0xFEC00000) MMIO pages mapped (PCD enabled).\n");

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

