#ifndef UI_MEMORY_H
#define UI_MEMORY_H

#include "mm.h"
#include "pmm.h"

static inline void* ui_alloc_pages(size_t pages) {
    phys_addr_t physical = pmm_alloc_pages(pages);
    return physical ? phys_to_virt(physical) : NULL;
}

static inline void ui_free_pages(void* pointer, size_t pages) {
    phys_addr_t physical = 0;
    if (virt_to_phys_checked(pointer, &physical)) {
        pmm_free_pages(physical, pages);
    }
}

static inline unsigned int ui_get_free_pages(void) {
    return (unsigned int)pmm_get_free_page_count();
}

#define pmm_alloc_pages(pages) ui_alloc_pages(pages)
#define pmm_free_pages(pointer, pages) ui_free_pages((pointer), (pages))
#define pmm_get_free_pages() ui_get_free_pages()

#endif
