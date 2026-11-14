#include "gfx_accel.h"
#include "boot_framebuffer.h"
#include "wm.h"

void gfx_accel_init(void) {
}

int gfx_accel_is_active(void) {
    return g_main_surface.framebuffer_virt != 0;
}

int gfx_accel_has_valid_driver(void) {
    return 0;
}

const char* gfx_accel_get_name(void) {
    return "Limine Linear Framebuffer";
}

const char* gfx_accel_get_status(void) {
    return "Fallback LFB (max 1024x768, 8/16-bit)";
}

const char* gfx_accel_get_fallback_reason(void) {
    return "No valid display driver";
}

int gfx_accel_is_hardware_2d_active(void) {
    return 0;
}

int gfx_accel_supports_1280x1024(void) {
    return 0;
}

unsigned int gfx_accel_get_vram_size(void) {
    return (unsigned int)g_main_surface.pitch * g_main_surface.height;
}

int gfx_accel_get_max_width(void) {
    int hw = (int)g_main_surface.width;
    if (hw <= 0 || hw > GFX_FALLBACK_MAX_WIDTH) return GFX_FALLBACK_MAX_WIDTH;
    return hw;
}

int gfx_accel_get_max_height(void) {
    int hh = (int)g_main_surface.height;
    if (hh <= 0 || hh > GFX_FALLBACK_MAX_HEIGHT) return GFX_FALLBACK_MAX_HEIGHT;
    return hh;
}

int gfx_accel_get_supported_bpp_mask(void) {
    return 0x03; /* 8-bit | 16-bit */
}

int gfx_accel_get_mode_count(void) {
    int max_w = gfx_accel_get_max_width();
    int max_h = gfx_accel_get_max_height();
    int cnt = 1; /* 640x480 */
    if (max_w >= 800 && max_h >= 600) cnt++;
    if (max_w >= 1024 && max_h >= 768) cnt++;
    return cnt;
}

int gfx_accel_get_mode(int index, int* out_w, int* out_h,
                       unsigned char* out_bpp_mask,
                       unsigned char* out_preferred) {
    static const int kw[3] = { 640, 800, 1024 };
    static const int kh[3] = { 480, 600, 768 };
    int max_w = gfx_accel_get_max_width();
    int max_h = gfx_accel_get_max_height();
    int idx = 0;
    for (int i = 0; i < 3; i++) {
        if (kw[i] > max_w || kh[i] > max_h) continue;
        if (idx == index) {
            if (out_w) *out_w = kw[i];
            if (out_h) *out_h = kh[i];
            if (out_bpp_mask) *out_bpp_mask = 0x03;
            if (out_preferred) *out_preferred = (kw[i] == max_w && kh[i] == max_h) ? 1 : 0;
            return 1;
        }
        idx++;
    }
    return 0;
}

extern void set_resolution(int width, int height, int bpp);

int gfx_accel_can_change_mode(int width, int height, int bpp) {
    return gfx_accel_supports_bpp(width, height, bpp);
}

int gfx_accel_supports_bpp(int width, int height, int bpp) {
    if (width < GFX_MIN_WIDTH || height < GFX_MIN_HEIGHT) {
        return 0;
    }
    if (width > gfx_accel_get_max_width() || height > gfx_accel_get_max_height()) {
        return 0;
    }
    return (bpp == 8 || bpp == 16);
}

int gfx_accel_try_set_mode(int width, int height, int bpp) {
    if (!gfx_accel_supports_bpp(width, height, bpp)) return 0;
    return set_resolution_ex(width, height, bpp) ? 1 : 0;
}

void gfx_accel_set_mode(int width, int height, int bpp) {
    if (!gfx_accel_supports_bpp(width, height, bpp)) return;
    (void)set_resolution_ex(width, height, bpp);
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
