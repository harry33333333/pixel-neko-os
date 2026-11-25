#ifndef FB_BLIT_WC_H
#define FB_BLIT_WC_H

#include <stdint.h>
#include <stddef.h>

/*
 * High-speed SSE2 non-temporal (movntdq) streaming copy into PAT
 * Write-Combining framebuffer memory with a single trailing sfence.
 * Safe to call from -mgeneral-regs-only kernel code.
 */
void fb_blit_wc_rect(void* dst_base, uint32_t dst_pitch,
                     const void* src_base, uint32_t src_pitch,
                     uint32_t row_bytes, uint32_t height);

#endif /* FB_BLIT_WC_H */
