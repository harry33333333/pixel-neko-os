#ifndef _DMA_H
#define _DMA_H

#include "mm.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DMA_BIT_MASK(n) (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))

#define ZONE_DMA_LIMIT    0x01000000ULL   // 16 MB (ISA DMA)
#define ZONE_DMA32_LIMIT  0x100000000ULL  // 4 GB (32-bit PCI DMA)

typedef struct {
    union {
        virt_addr_t virt_addr;      // CPU kernel virtual address
        void*       virt;
    };
    union {
        phys_addr_t phys_addr;      // Host physical page frame address
        uintptr_t   phys;
    };
    dma_addr_t  dma_addr;           // Device bus accessible DMA address
    size_t      size;               // Buffer size in bytes
    uint64_t    dma_mask;           // Device addressing capability mask
    size_t      alignment;          // Alignment requirement (e.g. 4KB, 64KB)
    int         is_coherent;        // Cache coherency flag
} dma_buffer_t;

void dma_init(void);
int  dma_alloc(size_t size, uint64_t dma_mask, size_t alignment, dma_buffer_t* out_buf);
void dma_free(dma_buffer_t* buf);
void dma_sync_for_cpu(dma_buffer_t* buf);
void dma_sync_for_device(dma_buffer_t* buf);

static inline dma_buffer_t dma_alloc_aligned(size_t size, size_t alignment) {
    dma_buffer_t buf;
    size_t align = alignment < 4096 ? 4096 : alignment;
    if (dma_alloc(size, DMA_BIT_MASK(32), align, &buf) != 0) {
        buf.virt_addr = 0;
        buf.phys_addr = 0;
        buf.dma_addr = 0;
        buf.size = 0;
        buf.dma_mask = 0;
        buf.alignment = 0;
        buf.is_coherent = 0;
    }
    return buf;
}

static inline int dma_is_valid(const dma_buffer_t* buf) {
    return buf && buf->virt_addr != 0 && buf->phys_addr != 0 && buf->size != 0;
}

static inline void dma_flush(const dma_buffer_t* buf) {
    if (buf) dma_sync_for_device((dma_buffer_t*)buf);
}

#endif /* _DMA_H */
