#include "boot_framebuffer.h"
#include "log.h"
#include "mm.h"
#include <stddef.h>

gfx_surface_t g_main_surface = {0};

// Built-in 8x16 monochrome bitmap font table (ASCII 0..127)
#include "font_new.h"

bool boot_framebuffer_init(struct limine_framebuffer* fb) {
    if (!fb || !fb->address) {
        log_printf("[BOOT FB ERROR] Limine framebuffer response is NULL or invalid!\n");
        return false;
    }

    uint64_t addr = (uint64_t)fb->address;
    if (addr >= g_hhdm.hhdm_offset && g_hhdm.hhdm_offset > 0) {
        g_main_surface.framebuffer_virt = (void*)addr;
        g_main_surface.framebuffer_phys = addr - g_hhdm.hhdm_offset;
    } else {
        g_main_surface.framebuffer_phys = addr;
        g_main_surface.framebuffer_virt = phys_to_virt(addr);
    }
    g_main_surface.width = (uint32_t)fb->width;
    g_main_surface.height = (uint32_t)fb->height;
    g_main_surface.pitch = (uint32_t)fb->pitch;
    g_main_surface.bpp = fb->bpp;

    // Extract color masks and shifts
    g_main_surface.red_mask_size = fb->red_mask_size;
    g_main_surface.red_mask_shift = fb->red_mask_shift;
    g_main_surface.green_mask_size = fb->green_mask_size;
    g_main_surface.green_mask_shift = fb->green_mask_shift;
    g_main_surface.blue_mask_size = fb->blue_mask_size;
    g_main_surface.blue_mask_shift = fb->blue_mask_shift;

    // Safe fallbacks if firmware provides 0 masks
    if (g_main_surface.red_mask_size == 0) {
        if (fb->bpp == 32 || fb->bpp == 24) {
            g_main_surface.red_mask_size = 8;
            g_main_surface.red_mask_shift = 16;
            g_main_surface.green_mask_size = 8;
            g_main_surface.green_mask_shift = 8;
            g_main_surface.blue_mask_size = 8;
            g_main_surface.blue_mask_shift = 0;
            g_main_surface.format = PIXEL_FORMAT_RGB888;
        } else if (fb->bpp == 16) {
            g_main_surface.red_mask_size = 5;
            g_main_surface.red_mask_shift = 11;
            g_main_surface.green_mask_size = 6;
            g_main_surface.green_mask_shift = 5;
            g_main_surface.blue_mask_size = 5;
            g_main_surface.blue_mask_shift = 0;
            g_main_surface.format = PIXEL_FORMAT_RGB565;
        }
    } else {
        if (fb->bpp == 32) {
            g_main_surface.format = (fb->red_mask_shift == 16) ? PIXEL_FORMAT_RGBA8888 : PIXEL_FORMAT_BGRA8888;
        } else if (fb->bpp == 24) {
            g_main_surface.format = (fb->red_mask_shift == 16) ? PIXEL_FORMAT_RGB888 : PIXEL_FORMAT_BGR888;
        } else if (fb->bpp == 16) {
            g_main_surface.format = PIXEL_FORMAT_RGB565;
        } else {
            g_main_surface.format = PIXEL_FORMAT_UNKNOWN;
        }
    }

    log_printf("[BOOT FB] Initialized: %ux%u @ %ubpp, pitch=%u, addr=%p\n",
               g_main_surface.width, g_main_surface.height, g_main_surface.bpp,
               g_main_surface.pitch, g_main_surface.framebuffer_virt);
    log_printf("[BOOT FB] Color layout: R(%u, <<%u) G(%u, <<%u) B(%u, <<%u)\n",
               g_main_surface.red_mask_size, g_main_surface.red_mask_shift,
               g_main_surface.green_mask_size, g_main_surface.green_mask_shift,
               g_main_surface.blue_mask_size, g_main_surface.blue_mask_shift);

    return true;
}

uint32_t boot_framebuffer_make_color(uint8_t r, uint8_t g, uint8_t b) {
    if (g_main_surface.bpp <= 8) {
        if (r == 0 && g == 0 && b == 0) return 0;
        if (r == 255 && g == 255 && b == 255) return 15;
        uint32_t r6 = ((uint32_t)r * 5U + 127U) / 255U;
        uint32_t g6 = ((uint32_t)g * 5U + 127U) / 255U;
        uint32_t b6 = ((uint32_t)b * 5U + 127U) / 255U;
        return 32U + r6 + g6 * 6U + b6 * 36U;
    }
    uint32_t r_shift = (g_main_surface.red_mask_size > 0 && g_main_surface.red_mask_size <= 8)
                       ? (8U - g_main_surface.red_mask_size) : 0U;
    uint32_t g_shift = (g_main_surface.green_mask_size > 0 && g_main_surface.green_mask_size <= 8)
                       ? (8U - g_main_surface.green_mask_size) : 0U;
    uint32_t b_shift = (g_main_surface.blue_mask_size > 0 && g_main_surface.blue_mask_size <= 8)
                       ? (8U - g_main_surface.blue_mask_size) : 0U;
    uint32_t r_part = ((uint32_t)(r >> r_shift)) << g_main_surface.red_mask_shift;
    uint32_t g_part = ((uint32_t)(g >> g_shift)) << g_main_surface.green_mask_shift;
    uint32_t b_part = ((uint32_t)(b >> b_shift)) << g_main_surface.blue_mask_shift;
    return r_part | g_part | b_part;
}

void boot_framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_main_surface.framebuffer_virt || x >= g_main_surface.width || y >= g_main_surface.height) {
        return;
    }

    uint8_t* row = ((uint8_t*)g_main_surface.framebuffer_virt) + (y * g_main_surface.pitch);
    if (g_main_surface.bpp == 32) {
        ((uint32_t*)row)[x] = color;
    } else if (g_main_surface.bpp == 24) {
        uint32_t offset = x * 3;
        row[offset + 0] = (uint8_t)(color & 0xFF);
        row[offset + 1] = (uint8_t)((color >> 8) & 0xFF);
        row[offset + 2] = (uint8_t)((color >> 16) & 0xFF);
    } else if (g_main_surface.bpp == 16) {
        ((uint16_t*)row)[x] = (uint16_t)color;
    } else if (g_main_surface.bpp == 8) {
        row[x] = (uint8_t)color;
    }
}

void boot_framebuffer_clear(uint32_t color) {
    if (!g_main_surface.framebuffer_virt) return;
    for (uint32_t y = 0; y < g_main_surface.height; y++) {
        uint8_t* row = ((uint8_t*)g_main_surface.framebuffer_virt) + (y * g_main_surface.pitch);
        if (g_main_surface.bpp == 32) {
            uint32_t* p = (uint32_t*)row;
            for (uint32_t x = 0; x < g_main_surface.width; x++) {
                p[x] = color;
            }
        } else if (g_main_surface.bpp == 16) {
            uint16_t* p = (uint16_t*)row;
            for (uint32_t x = 0; x < g_main_surface.width; x++) {
                p[x] = (uint16_t)color;
            }
        } else {
            for (uint32_t x = 0; x < g_main_surface.width; x++) {
                boot_framebuffer_putpixel(x, y, color);
            }
        }
    }
}

void boot_framebuffer_draw_checkerboard(void) {
    if (!g_main_surface.framebuffer_virt) return;

    // Classic 50% dither checkerboard pattern (Mac OS classic desk background)
    uint32_t color_white = boot_framebuffer_make_color(255, 255, 255);
    uint32_t color_gray  = boot_framebuffer_make_color(170, 170, 170);

    for (uint32_t y = 0; y < g_main_surface.height; y++) {
        uint8_t* row = ((uint8_t*)g_main_surface.framebuffer_virt) + (y * g_main_surface.pitch);
        if (g_main_surface.bpp == 32) {
            uint32_t* p = (uint32_t*)row;
            for (uint32_t x = 0; x < g_main_surface.width; x++) {
                p[x] = ((x ^ y) & 1) ? color_white : color_gray;
            }
        } else if (g_main_surface.bpp == 16) {
            uint16_t* p = (uint16_t*)row;
            for (uint32_t x = 0; x < g_main_surface.width; x++) {
                p[x] = ((x ^ y) & 1) ? (uint16_t)color_white : (uint16_t)color_gray;
            }
        } else {
            for (uint32_t x = 0; x < g_main_surface.width; x++) {
                boot_framebuffer_putpixel(x, y, ((x ^ y) & 1) ? color_white : color_gray);
            }
        }
    }
}

void boot_framebuffer_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_main_surface.framebuffer_virt) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_main_surface.width) w = (int)g_main_surface.width - x;
    if (y + h > (int)g_main_surface.height) h = (int)g_main_surface.height - y;
    if (w <= 0 || h <= 0) return;

    for (int cy = y; cy < y + h; cy++) {
        uint8_t* row = ((uint8_t*)g_main_surface.framebuffer_virt) + (cy * g_main_surface.pitch);
        if (g_main_surface.bpp == 32) {
            uint32_t* p = (uint32_t*)row;
            for (int cx = x; cx < x + w; cx++) {
                p[cx] = color;
            }
        } else {
            for (int cx = x; cx < x + w; cx++) {
                boot_framebuffer_putpixel(cx, cy, color);
            }
        }
    }
}

void boot_framebuffer_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    if ((unsigned char)c >= 128) c = ' ';
    const unsigned char* glyph = font_8x16[(unsigned char)c];

    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                boot_framebuffer_putpixel(x + col, y + row, fg);
            } else if (bg != 0xFFFFFFFF) {
                boot_framebuffer_putpixel(x + col, y + row, bg);
            }
        }
    }
}

void boot_framebuffer_draw_string(int x, int y, const char* str, uint32_t fg, uint32_t bg) {
    if (!str) return;
    int cur_x = x;
    int cur_y = y;

    while (*str) {
        if (*str == '\n') {
            cur_x = x;
            cur_y += 16;
        } else {
            boot_framebuffer_draw_char(cur_x, cur_y, *str, fg, bg);
            cur_x += 8;
        }
        str++;
    }
}

void boot_framebuffer_flush(void) {
    if (!g_main_surface.framebuffer_virt) return;
    __asm__ volatile ("sfence" : : : "memory");
}
