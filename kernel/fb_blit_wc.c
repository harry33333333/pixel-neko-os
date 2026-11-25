#include "fb_blit_wc.h"
#include "string.h"

static inline uint64_t wc_irq_save(void) {
    uint64_t flags;
    __asm__ __volatile__("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void wc_irq_restore(uint64_t flags) {
    __asm__ __volatile__("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static void fb_blit_scalar_row(uint8_t* dst, const uint8_t* src, uint32_t n) {
    while (n >= 8U) {
        *(volatile uint64_t*)dst = *(const uint64_t*)src;
        dst += 8;
        src += 8;
        n -= 8U;
    }
    while (n >= 4U) {
        *(volatile uint32_t*)dst = *(const uint32_t*)src;
        dst += 4;
        src += 4;
        n -= 4U;
    }
    while (n > 0U) {
        *(volatile uint8_t*)dst = *src;
        dst++;
        src++;
        n--;
    }
}

static inline int wc_sse2_ready(void) {
    uint64_t cr0, cr4;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
    /* Require CR4.OSFXSR (bit 9) = 1, CR0.EM (bit 2) = 0, CR0.TS (bit 3) = 0 */
    return ((cr4 & (1ULL << 9)) != 0ULL) &&
           ((cr0 & ((1ULL << 2) | (1ULL << 3))) == 0ULL);
}

static void fb_blit_sse2_chunk(uint8_t* dst_row, uint32_t dst_pitch,
                               const uint8_t* src_row, uint32_t src_pitch,
                               uint32_t row_bytes, uint32_t rows) {
    uint8_t xmm_save[64] __attribute__((aligned(16)));
    uint64_t flags = wc_irq_save();

    __asm__ __volatile__(
        "movdqu %%xmm0, 0(%0)\n\t"
        "movdqu %%xmm1, 16(%0)\n\t"
        "movdqu %%xmm2, 32(%0)\n\t"
        "movdqu %%xmm3, 48(%0)\n\t"
        :
        : "r"(xmm_save)
        : "memory");

    for (uint32_t r = 0; r < rows; r++) {
        uint8_t* d = dst_row + (size_t)r * (size_t)dst_pitch;
        const uint8_t* s = src_row + (size_t)r * (size_t)src_pitch;
        uint32_t rem = row_bytes;

        /* Align destination to 16 bytes for movntdq */
        uintptr_t mis = (uintptr_t)d & 15U;
        if (mis != 0U) {
            uint32_t lead = (uint32_t)(16U - mis);
            if (lead > rem) lead = rem;
            fb_blit_scalar_row(d, s, lead);
            d += lead;
            s += lead;
            rem -= lead;
        }

        /* 64-byte unrolled non-temporal streaming stores into WC VRAM */
        while (rem >= 64U) {
            __asm__ __volatile__(
                "movdqu   0(%1), %%xmm0\n\t"
                "movdqu  16(%1), %%xmm1\n\t"
                "movdqu  32(%1), %%xmm2\n\t"
                "movdqu  48(%1), %%xmm3\n\t"
                "movntdq %%xmm0,  0(%0)\n\t"
                "movntdq %%xmm1, 16(%0)\n\t"
                "movntdq %%xmm2, 32(%0)\n\t"
                "movntdq %%xmm3, 48(%0)\n\t"
                :
                : "r"(d), "r"(s)
                : "memory");
            d += 64;
            s += 64;
            rem -= 64U;
        }

        while (rem >= 16U) {
            __asm__ __volatile__(
                "movdqu  (%1), %%xmm0\n\t"
                "movntdq %%xmm0, (%0)\n\t"
                :
                : "r"(d), "r"(s)
                : "memory");
            d += 16;
            s += 16;
            rem -= 16U;
        }

        if (rem > 0U) {
            fb_blit_scalar_row(d, s, rem);
        }
    }

    __asm__ __volatile__(
        "sfence\n\t"
        "movdqu 0(%0), %%xmm0\n\t"
        "movdqu 16(%0), %%xmm1\n\t"
        "movdqu 32(%0), %%xmm2\n\t"
        "movdqu 48(%0), %%xmm3\n\t"
        :
        : "r"(xmm_save)
        : "memory");

    wc_irq_restore(flags);
}

void fb_blit_wc_rect(void* dst_base, uint32_t dst_pitch,
                     const void* src_base, uint32_t src_pitch,
                     uint32_t row_bytes, uint32_t height) {
    if (!dst_base || !src_base || row_bytes == 0U || height == 0U) return;

    uint8_t* d = (uint8_t*)dst_base;
    const uint8_t* s = (const uint8_t*)src_base;

    if (row_bytes < 64U || !wc_sse2_ready()) {
        for (uint32_t r = 0; r < height; r++) {
            fb_blit_scalar_row(d + (size_t)r * (size_t)dst_pitch,
                               s + (size_t)r * (size_t)src_pitch,
                               row_bytes);
        }
        __asm__ __volatile__("sfence" : : : "memory");
        return;
    }

    /* Process in 16-row chunks to keep IRQ latency < 15 us even at 4K */
    const uint32_t k_chunk_rows = 16U;
    uint32_t done = 0;
    while (done < height) {
        uint32_t batch = height - done;
        if (batch > k_chunk_rows) batch = k_chunk_rows;
        fb_blit_sse2_chunk(d + (size_t)done * (size_t)dst_pitch, dst_pitch,
                           s + (size_t)done * (size_t)src_pitch, src_pitch,
                           row_bytes, batch);
        done += batch;
    }
}

