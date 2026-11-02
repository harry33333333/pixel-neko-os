#ifndef _PMM_H
#define _PMM_H

#include "mm.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void pmm_init(void);
phys_addr_t pmm_alloc_page(void);
phys_addr_t pmm_alloc_pages(size_t count);
void pmm_free_page(phys_addr_t phys);
void pmm_free_pages(phys_addr_t phys, size_t count);
phys_addr_t pmm_alloc_pages_zoned(size_t count, uint64_t max_addr, size_t alignment, bool no_64k_cross);

uint64_t pmm_get_free_page_count(void);
uint64_t pmm_get_total_page_count(void);

#endif /* _PMM_H */
