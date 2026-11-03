#ifndef _BOOT_FRAMEBUFFER_H
#define _BOOT_FRAMEBUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include "limine.h"

typedef enum {
    PIXEL_FORMAT_RGB888,
    PIXEL_FORMAT_BGR888,
    PIXEL_FORMAT_RGBA8888,
    PIXEL_FORMAT_BGRA8888,
    PIXEL_FORMAT_RGB565,
    PIXEL_FORMAT_UNKNOWN
} pixel_format_t;

typedef struct {
    uint64_t       framebuffer_phys; // Physical base address of framebuffer
    void*          framebuffer_virt; // Virtual base address (mapped)
    uint32_t       width;
    uint32_t       height;
    uint32_t       pitch;
    uint32_t       bpp;
    pixel_format_t format;
    uint8_t        red_mask_size;
    uint8_t        red_mask_shift;
    uint8_t        green_mask_size;
    uint8_t        green_mask_shift;
    uint8_t        blue_mask_size;
    uint8_t        blue_mask_shift;
} gfx_surface_t;

extern gfx_surface_t g_main_surface;

bool boot_framebuffer_init(struct limine_framebuffer* fb);
uint32_t boot_framebuffer_make_color(uint8_t r, uint8_t g, uint8_t b);
void boot_framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color);
void boot_framebuffer_clear(uint32_t color);
void boot_framebuffer_draw_checkerboard(void);
void boot_framebuffer_fill_rect(int x, int y, int w, int h, uint32_t color);
void boot_framebuffer_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void boot_framebuffer_draw_string(int x, int y, const char* str, uint32_t fg, uint32_t bg);
void boot_framebuffer_flush(void);

#endif /* _BOOT_FRAMEBUFFER_H */
