#include "gfx_accel.h"
#include "boot_framebuffer.h"

void gfx_accel_init(void) {
}

int gfx_accel_is_active(void) {
    return g_main_surface.framebuffer_virt != 0;
}

const char* gfx_accel_get_name(void) {
    return "Limine Linear Framebuffer";
}

const char* gfx_accel_get_status(void) {
    return "WC framebuffer active";
}

const char* gfx_accel_get_fallback_reason(void) {
    return "Linear framebuffer backend";
}

int gfx_accel_is_hardware_2d_active(void) {
    return 0;
}

int gfx_accel_supports_1280x1024(void) {
    return 1;
}

unsigned int gfx_accel_get_vram_size(void) {
    return (unsigned int)g_main_surface.pitch * g_main_surface.height;
}

extern void set_resolution(int width, int height, int bpp);

int gfx_accel_supports_bpp(int width, int height, int bpp) {
    if (width < 320 || width > 1280 || height < 200 || height > 1024) {
        return 0;
    }
    return (bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32);
}

int gfx_accel_try_set_mode(int width, int height, int bpp) {
    if (!gfx_accel_supports_bpp(width, height, bpp)) return 0;
    set_resolution(width, height, bpp);
    return 1;
}

void gfx_accel_set_mode(int width, int height, int bpp) {
    if (!gfx_accel_supports_bpp(width, height, bpp)) return;
    set_resolution(width, height, bpp);
}

void gfx_accel_fill_rect(int x, int y, int width, int height,
                         unsigned int color) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
}

void gfx_accel_screen_blt(int source_x, int source_y, int destination_x,
                          int destination_y, int width, int height) {
    (void)source_x;
    (void)source_y;
    (void)destination_x;
    (void)destination_y;
    (void)width;
    (void)height;
}

void gfx_accel_present_rect(int x, int y, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void gfx_accel_sync(void) {
    boot_framebuffer_flush();
}

int gfx_accel_has_hardware_cursor(void) {
    return 0;
}

int gfx_accel_set_cursor(int x, int y, int visible) {
    (void)x;
    (void)y;
    (void)visible;
    return 0;
}
