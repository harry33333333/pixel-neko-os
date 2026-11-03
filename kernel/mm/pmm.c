#include "pmm.h"
#include "limine.h"
#include "log.h"
#include "string.h"

extern volatile struct limine_memmap_request memmap_request;

static uint8_t* g_pmm_bitmap = NULL;
static uint64_t g_pmm_total_pages = 0;
static uint64_t g_pmm_free_pages = 0;
static uint64_t g_pmm_last_index = 0;

static inline void bitmap_set(uint64_t bit) {
    g_pmm_bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(uint64_t bit) {
    g_pmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline bool bitmap_test(uint64_t bit) {
    return (g_pmm_bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

uint64_t pmm_get_free_page_count(void) {
    return g_pmm_free_pages;
}

uint64_t pmm_get_total_page_count(void) {
    return g_pmm_total_pages;
}

void pmm_init(void) {
    if (!memmap_request.response) {
        log_printf("[PMM FATAL] Limine memmap response missing!\n");
        return;
    }

    uint64_t entry_count = memmap_request.response->entry_count;
    struct limine_memmap_entry** entries = memmap_request.response->entries;

    uint64_t max_phys = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        uint64_t type = entries[i]->type;
        if (type == LIMINE_MEMMAP_USABLE ||
            type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            type == LIMINE_MEMMAP_KERNEL_AND_MODULES ||
            type == LIMINE_MEMMAP_ACPI_RECLAIMABLE ||
            type == LIMINE_MEMMAP_ACPI_NVS) {
            uint64_t end = entries[i]->base + entries[i]->length;
            if (end > max_phys) max_phys = end;
        }
    }
    if (max_phys == 0) {
        for (uint64_t i = 0; i < entry_count; i++) {
            uint64_t end = entries[i]->base + entries[i]->length;
            if (end > max_phys) max_phys = end;
        }
    }

    g_pmm_total_pages = max_phys / PAGE_SIZE;
    uint64_t bitmap_size = (g_pmm_total_pages + 7) / 8;
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    // Find a USABLE memory chunk above 1MB to house the bitmap
    phys_addr_t bitmap_phys = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i]->type == LIMINE_MEMMAP_USABLE &&
            entries[i]->base >= 0x100000 &&
            entries[i]->length >= bitmap_pages * PAGE_SIZE) {
            bitmap_phys = entries[i]->base;
            break;
        }
    }

    if (!bitmap_phys) {
        log_printf("[PMM FATAL] Failed to locate contiguous RAM for page bitmap!\n");
        return;
    }

    g_pmm_bitmap = (uint8_t*)phys_to_virt(bitmap_phys);
    // 1. Mark ALL pages as USED initially
    memset(g_pmm_bitmap, 0xFF, bitmap_size);

    // 2. Mark USABLE regions as FREE in the bitmap
    g_pmm_free_pages = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i]->type == LIMINE_MEMMAP_USABLE) {
            uint64_t start_page = entries[i]->base / PAGE_SIZE;
            uint64_t page_cnt = entries[i]->length / PAGE_SIZE;

            for (uint64_t p = 0; p < page_cnt; p++) {
                uint64_t page_idx = start_page + p;
                if (page_idx < g_pmm_total_pages) {
                    bitmap_clear(page_idx);
                    g_pmm_free_pages++;
                }
            }
        }
    }

    // 3. Mark the lower 1MB (0..0x100000) as permanently USED for BIOS/BDA safety
    for (uint64_t p = 0; p < 256; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            if (g_pmm_free_pages > 0) g_pmm_free_pages--;
        }
    }

    // 4. Mark the bitmap's own allocated pages as USED
    uint64_t b_start = bitmap_phys / PAGE_SIZE;
    for (uint64_t p = 0; p < bitmap_pages; p++) {
        if (!bitmap_test(b_start + p)) {
            bitmap_set(b_start + p);
            if (g_pmm_free_pages > 0) g_pmm_free_pages--;
        }
    }

    log_printf("[PMM] 64-bit PMM Initialized: Total %llu MB (%llu pages), Free %llu MB (%llu pages)\n",
               (g_pmm_total_pages * PAGE_SIZE) / (1024 * 1024), g_pmm_total_pages,
               (g_pmm_free_pages * PAGE_SIZE) / (1024 * 1024), g_pmm_free_pages);
    log_printf("[PMM] Page Bitmap placed at Phys 0x%016llx (Size: %llu KB)\n",
               bitmap_phys, bitmap_size / 1024);
}

phys_addr_t pmm_alloc_page(void) {
    for (uint64_t i = 0; i < g_pmm_total_pages; i++) {
        uint64_t idx = (g_pmm_last_index + i) % g_pmm_total_pages;
        if (!bitmap_test(idx)) {
            bitmap_set(idx);
            g_pmm_last_index = (idx + 1) % g_pmm_total_pages;
            if (g_pmm_free_pages > 0) g_pmm_free_pages--;
            return (phys_addr_t)(idx * PAGE_SIZE);
        }
    }
    log_printf("[PMM ERROR] Physical memory exhausted (Out of Memory)!\n");
    return 0;
}

phys_addr_t pmm_alloc_pages(size_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_page();

    uint64_t consecutive = 0;
    uint64_t start_idx = 0;

    for (uint64_t i = 0; i < g_pmm_total_pages; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) start_idx = i;
            consecutive++;
            if (consecutive == count) {
                for (uint64_t j = 0; j < count; j++) {
                    bitmap_set(start_idx + j);
                }
                if (g_pmm_free_pages >= count) g_pmm_free_pages -= count;
                else g_pmm_free_pages = 0;
                return (phys_addr_t)(start_idx * PAGE_SIZE);
            }
        } else {
            consecutive = 0;
        }
    }
    log_printf("[PMM ERROR] Failed to allocate %llu contiguous physical pages!\n", (uint64_t)count);
    return 0;
}

void pmm_free_page(phys_addr_t phys) {
    if (phys % PAGE_SIZE != 0) return;
    uint64_t idx = phys / PAGE_SIZE;
    if (idx >= g_pmm_total_pages) return;

    if (bitmap_test(idx)) {
        bitmap_clear(idx);
        g_pmm_free_pages++;
    }
}

void pmm_free_pages(phys_addr_t phys, size_t count) {
    for (size_t i = 0; i < count; i++) {
        pmm_free_page(phys + i * PAGE_SIZE);
    }
}

phys_addr_t pmm_alloc_pages_zoned(size_t count, uint64_t max_addr, size_t alignment, bool no_64k_cross) {
    if (count == 0) return 0;
    if (alignment < PAGE_SIZE) alignment = PAGE_SIZE;
    uint64_t align_pages = alignment / PAGE_SIZE;

    uint64_t max_page = max_addr / PAGE_SIZE;
    if (max_page > g_pmm_total_pages) max_page = g_pmm_total_pages;

    for (uint64_t i = 0; i + count <= max_page; i += (align_pages ? align_pages : 1)) {
        phys_addr_t start_phys = (phys_addr_t)(i * PAGE_SIZE);
        phys_addr_t end_phys = start_phys + (count * PAGE_SIZE) - 1;

        if (end_phys > max_addr) break;

        // Check 64KB boundary restriction (for ISA DMA)
        if (no_64k_cross && ((start_phys >> 16) != (end_phys >> 16))) {
            continue;
        }

        // Verify if all requested pages are free
        bool all_free = true;
        for (uint64_t j = 0; j < count; j++) {
            if (bitmap_test(i + j)) {
                all_free = false;
                break;
            }
        }

        if (all_free) {
            for (uint64_t j = 0; j < count; j++) {
                bitmap_set(i + j);
            }
            if (g_pmm_free_pages >= count) g_pmm_free_pages -= count;
            else g_pmm_free_pages = 0;
            return start_phys;
        }
    }

    log_printf("[PMM WARN] Failed to allocate %llu zoned physical pages (limit: 0x%016llx, align: 0x%llx)\n",
               (uint64_t)count, max_addr, (uint64_t)alignment);
    return 0;
}
