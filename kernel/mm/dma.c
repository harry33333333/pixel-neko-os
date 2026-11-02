#include "dma.h"
#include "pmm.h"
#include "log.h"
#include "string.h"

void dma_init(void) {
    log_printf("[DMA] 64-bit Unified DMA Subsystem Initialized (ZONE_DMA <16M, ZONE_DMA32 <4G, ZONE_NORMAL >4G)\n");
}

int dma_alloc(size_t size, uint64_t dma_mask, size_t alignment, dma_buffer_t* out_buf) {
    if (!out_buf || size == 0) return -1;
    if (alignment < PAGE_SIZE) alignment = PAGE_SIZE;

    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    bool no_64k_cross = (dma_mask <= DMA_BIT_MASK(24));

    phys_addr_t phys = pmm_alloc_pages_zoned(pages, dma_mask, alignment, no_64k_cross);
    if (!phys) {
        log_printf("[DMA ERROR] Failed to allocate DMA buffer (size=%llu, mask=0x%016llx, align=0x%llx)\n",
                   (uint64_t)size, dma_mask, (uint64_t)alignment);
        return -1;
    }

    out_buf->phys_addr = phys;
    out_buf->virt_addr = (virt_addr_t)phys_to_virt(phys);
    out_buf->dma_addr = (dma_addr_t)phys; // 1:1 physical bus address
    out_buf->size = size;
    out_buf->dma_mask = dma_mask;
    out_buf->alignment = alignment;
    out_buf->is_coherent = 1;

    // Clear buffer memory
    memset((void*)out_buf->virt_addr, 0, size);

    log_printf("[DMA] Allocated DMA buffer: Virt %p, Phys 0x%016llx (Size: %llu KB)\n",
               (void*)out_buf->virt_addr, out_buf->phys_addr, (uint64_t)(size / 1024));

    return 0;
}

void dma_free(dma_buffer_t* buf) {
    if (!buf || !buf->phys_addr || buf->size == 0) return;
    size_t pages = (buf->size + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_free_pages(buf->phys_addr, pages);
    buf->virt_addr = 0;
    buf->phys_addr = 0;
    buf->dma_addr = 0;
    buf->size = 0;
}

void dma_sync_for_cpu(dma_buffer_t* buf) {
    (void)buf;
    // Memory fence to synchronize memory writes
    __asm__ volatile ("mfence" ::: "memory");
}

void dma_sync_for_device(dma_buffer_t* buf) {
    (void)buf;
    // Store fence to flush pending stores before hardware DMA trigger
    __asm__ volatile ("sfence" ::: "memory");
}
