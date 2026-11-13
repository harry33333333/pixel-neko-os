#include "ui_compat.h"
#include "apic.h"
#include "audio.h"
#include "boot_framebuffer.h"
#include "gfx_accel.h"
#include "input.h"
#include "keyboard.h"
#include "mm.h"
#include "pmm.h"
#include "port.h"
#include "scheduler.h"
#include "string.h"
#include "timer.h"
#include "vfs.h"
#include "wm.h"

#define UI_MIN_WIDTH 640
#define UI_MIN_HEIGHT 480
#define UI_BOOT_FALLBACK_BYTES (1024U * 768U * 2U)

#define CURSOR_W 12
#define CURSOR_H 17

int screen_w;
int screen_h;
int screen_bpp;
int screen_pitch;
static unsigned char g_boot_backbuffer[UI_BOOT_FALLBACK_BYTES];
unsigned char* backbuffer = g_boot_backbuffer;
static phys_addr_t g_backbuffer_phys = 0;
static size_t g_backbuffer_pages = 0;
unsigned char* lfb;
unsigned short palette16[256];
unsigned int palette32[256];
int total_ram_mb;
int usable_ram_mb;

static int g_mouse_x;
static int g_mouse_y;
static uint32_t g_mouse_buttons;
static bool g_started;
static bool g_install_mode;

static uint32_t g_cursor_saved[CURSOR_H][CURSOR_W];
static int g_cursor_saved_x = -1;
static int g_cursor_saved_y = -1;
static bool g_cursor_saved_valid = false;
static bool g_cursor_invalidated = false;

static bool g_drag_box_saved = false;
static int g_drag_bx = 0;
static int g_drag_by = 0;
static int g_drag_bw = 0;
static int g_drag_bh = 0;

static const uint8_t g_vga_palette[16][3] = {
    {0, 0, 0}, {0, 0, 128}, {0, 128, 0}, {0, 128, 128},
    {128, 0, 0}, {128, 0, 128}, {128, 64, 0}, {192, 192, 192},
    {128, 128, 128}, {0, 0, 255}, {0, 255, 0}, {0, 255, 255},
    {255, 0, 0}, {255, 0, 255}, {255, 255, 0}, {255, 255, 255}
};

static void ui_build_palette(void) {
    memset(palette16, 0, sizeof(palette16));
    memset(palette32, 0, sizeof(palette32));
    for (int index = 0; index < 256; index++) {
        int red;
        int green;
        int blue;
        if (index < 16) {
            red = g_vga_palette[index][0];
            green = g_vga_palette[index][1];
            blue = g_vga_palette[index][2];
        } else if (index < 32) {
            red = green = blue = (index - 16) * 17;
        } else {
            int value = index - 32;
            red = (value % 6) * 51;
            green = ((value / 6) % 6) * 51;
            blue = ((value / 36) % 6) * 51;
        }
        palette32[index] = ((uint32_t)red << 16) |
                           ((uint32_t)green << 8) | (uint32_t)blue;
        uint16_t r5 = (uint16_t)(red >> 3);
        uint16_t g6 = (uint16_t)(green >> 2);
        uint16_t b5 = (uint16_t)(blue >> 3);
        palette16[index] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
}

static size_t ui_calc_backbuffer_pages(int width, int height, int bpp,
                                       uint64_t* out_bytes) {
    if (width < UI_MIN_WIDTH || height < UI_MIN_HEIGHT) return 0;
    if (width > 16384 || height > 16384) return 0;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    uint64_t bpp_bytes = (uint64_t)(unsigned int)(bpp / 8);
    uint64_t bytes = (uint64_t)(unsigned int)width *
                     (uint64_t)(unsigned int)height * bpp_bytes;
    if (out_bytes) *out_bytes = bytes;
    return (size_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
}

bool ui_can_allocate_resolution(int width, int height, int bpp) {
    uint64_t bytes = 0;
    size_t need_pages = ui_calc_backbuffer_pages(width, height, bpp, &bytes);
    if (need_pages == 0) return false;
    if (bytes <= UI_BOOT_FALLBACK_BYTES) return true;
    if (g_backbuffer_phys != 0 && need_pages <= g_backbuffer_pages) return true;
    return (uint64_t)need_pages + 512ULL <= pmm_get_free_page_count();
}

void ui_compat_init(void) {
    screen_w = (int)g_main_surface.width;
    screen_h = (int)g_main_surface.height;
    if (screen_w < UI_MIN_WIDTH) screen_w = UI_MIN_WIDTH;
    if (screen_h < UI_MIN_HEIGHT) screen_h = UI_MIN_HEIGHT;

    int bpp = (int)g_main_surface.bpp;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        bpp = 16;
    }
    screen_bpp = bpp;
    int bytes_per_pixel = screen_bpp / 8;
    screen_pitch = screen_w * bytes_per_pixel;
    lfb = (unsigned char*)g_main_surface.framebuffer_virt;
    ui_build_palette();

    uint64_t need_bytes = (uint64_t)(unsigned int)screen_pitch *
                          (uint64_t)(unsigned int)screen_h;
    size_t pages = (size_t)((need_bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    if (need_bytes <= UI_BOOT_FALLBACK_BYTES) {
        backbuffer = g_boot_backbuffer;
        g_backbuffer_phys = 0;
        g_backbuffer_pages = 0;
    } else {
        phys_addr_t phys = (pages > 0) ? pmm_alloc_pages(pages) : 0;
        if (phys != 0) {
            backbuffer = (unsigned char*)phys_to_virt(phys);
            g_backbuffer_phys = phys;
            g_backbuffer_pages = pages;
        } else {
            screen_w = 1024;
            screen_h = 768;
            screen_bpp = 16;
            screen_pitch = screen_w * 2;
            need_bytes = (uint64_t)(unsigned int)screen_pitch *
                         (uint64_t)(unsigned int)screen_h;
            backbuffer = g_boot_backbuffer;
            g_backbuffer_phys = 0;
            g_backbuffer_pages = 0;
        }
    }
    memset(backbuffer, 0, (size_t)need_bytes);

    if (g_hhdm.total_memory_bytes > 0) {
        total_ram_mb = (int)((g_hhdm.total_memory_bytes + 0x7FFFFULL) / (1024 * 1024));
    } else {
        total_ram_mb = (int)((pmm_get_total_page_count() * PAGE_SIZE) / (1024 * 1024));
    }
    if (g_hhdm.total_usable_bytes > 0) {
        usable_ram_mb = (int)(g_hhdm.total_usable_bytes / (1024 * 1024));
    } else {
        usable_ram_mb = total_ram_mb;
    }

    g_mouse_x = screen_w / 2;
    g_mouse_y = screen_h / 2;
    g_cursor_saved_valid = false;
    g_cursor_invalidated = false;
    g_started = false;
}

static inline uint32_t surface_pixel_to_rgb32(const unsigned char* source,
                                              int sx, int sy) {
    if (screen_bpp <= 8) {
        uint8_t idx = source[(size_t)sy * (size_t)screen_pitch + (size_t)sx];
        return palette32[idx];
    }
    if (screen_bpp <= 16) {
        uint16_t c16 = *(const uint16_t*)(source + (size_t)sy * (size_t)screen_pitch +
                                          (size_t)sx * 2);
        uint32_t r5 = (c16 >> 11) & 0x1Fu;
        uint32_t g6 = (c16 >> 5) & 0x3Fu;
        uint32_t b5 = c16 & 0x1Fu;
        uint32_t r8 = (r5 << 3) | (r5 >> 2);
        uint32_t g8 = (g6 << 2) | (g6 >> 4);
        uint32_t b8 = (b5 << 3) | (b5 >> 2);
        return (r8 << 16) | (g8 << 8) | b8;
    }
    if (screen_bpp == 24) {
        const uint8_t* p = source + (size_t)sy * (size_t)screen_pitch +
                           (size_t)sx * 3;
        return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
    }
    return *(const uint32_t*)(source + (size_t)sy * (size_t)screen_pitch +
                              (size_t)sx * 4);
}

static inline uint32_t surface_read_raw(const unsigned char* surface,
                                        int px, int py) {
    if (screen_bpp <= 8) {
        return surface[(size_t)py * (size_t)screen_pitch + (size_t)px];
    }
    if (screen_bpp <= 16) {
        return *(const uint16_t*)(surface + (size_t)py * (size_t)screen_pitch +
                                  (size_t)px * 2);
    }
    if (screen_bpp == 24) {
        const uint8_t* p = surface + (size_t)py * (size_t)screen_pitch +
                           (size_t)px * 3;
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    }
    return *(const uint32_t*)(surface + (size_t)py * (size_t)screen_pitch +
                              (size_t)px * 4);
}

static inline void surface_write_raw(unsigned char* surface,
                                     int px, int py, uint32_t raw) {
    if (screen_bpp <= 8) {
        surface[(size_t)py * (size_t)screen_pitch + (size_t)px] = (uint8_t)raw;
        return;
    }
    if (screen_bpp <= 16) {
        *(uint16_t*)(surface + (size_t)py * (size_t)screen_pitch +
                     (size_t)px * 2) = (uint16_t)raw;
        return;
    }
    if (screen_bpp == 24) {
        uint8_t* p = surface + (size_t)py * (size_t)screen_pitch +
                     (size_t)px * 3;
        p[0] = (uint8_t)(raw & 0xFFu);
        p[1] = (uint8_t)((raw >> 8) & 0xFFu);
        p[2] = (uint8_t)((raw >> 16) & 0xFFu);
        return;
    }
    *(uint32_t*)(surface + (size_t)py * (size_t)screen_pitch +
                 (size_t)px * 4) = raw;
}

static inline void lfb_put_rgb32(int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 ||
        x >= (int)g_main_surface.width || y >= (int)g_main_surface.height) {
        return;
    }
    uint32_t color = boot_framebuffer_make_color(
        (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
    boot_framebuffer_putpixel((uint32_t)x, (uint32_t)y, color);
}

void copy_rect_to_lfb(const unsigned char* source, int x, int y,
                      int width, int height) {
    if (!source || !lfb) return;
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > screen_w) width = screen_w - x;
    if (y + height > screen_h) height = screen_h - y;
    if (width <= 0 || height <= 0) return;

    int hw_w = (int)g_main_surface.width;
    int hw_h = (int)g_main_surface.height;
    if (hw_w <= 0 || hw_h <= 0) return;

    lfb = (unsigned char*)g_main_surface.framebuffer_virt;
    if (screen_w == hw_w && screen_h == hw_h) {
        for (int row = 0; row < height; row++) {
            int sy = y + row;
            if (screen_bpp == 8 && g_main_surface.bpp == 8) {
                volatile uint8_t* dst =
                    (volatile uint8_t*)(lfb +
                                        (size_t)sy * g_main_surface.pitch +
                                        (size_t)x);
                const uint8_t* src =
                    (const uint8_t*)(source + (size_t)sy * (size_t)screen_pitch +
                                     (size_t)x);
                for (int column = 0; column < width; column++) {
                    dst[column] = src[column];
                }
            } else if (screen_bpp == 16 && g_main_surface.bpp == 16 &&
                       g_main_surface.red_mask_size == 5 &&
                       g_main_surface.green_mask_size == 6 &&
                       g_main_surface.blue_mask_size == 5 &&
                       g_main_surface.red_mask_shift == 11) {
                volatile uint16_t* dst =
                    (volatile uint16_t*)(lfb +
                                         (size_t)sy * g_main_surface.pitch +
                                         (size_t)x * 2);
                const uint16_t* src =
                    (const uint16_t*)(source + (size_t)sy * (size_t)screen_pitch +
                                      (size_t)x * 2);
                for (int column = 0; column < width; column++) {
                    dst[column] = src[column];
                }
            } else if (screen_bpp == 32 && g_main_surface.bpp == 32 &&
                       ((g_main_surface.red_mask_shift == 16 &&
                         g_main_surface.green_mask_shift == 8 &&
                         g_main_surface.blue_mask_shift == 0) ||
                        g_main_surface.format == PIXEL_FORMAT_RGB888)) {
                volatile uint32_t* dst =
                    (volatile uint32_t*)(lfb +
                                         (size_t)sy * g_main_surface.pitch +
                                         (size_t)x * 4);
                const uint32_t* src =
                    (const uint32_t*)(source + (size_t)sy * (size_t)screen_pitch +
                                      (size_t)x * 4);
                for (int column = 0; column < width; column++) {
                    dst[column] = src[column];
                }
            } else {
                for (int column = 0; column < width; column++) {
                    lfb_put_rgb32(x + column, sy,
                                  surface_pixel_to_rgb32(source,
                                                         x + column, sy));
                }
            }
        }
    } else {
        int dst_x0 = (x * hw_w) / screen_w;
        int dst_y0 = (y * hw_h) / screen_h;
        int dst_x1 = ((x + width) * hw_w + screen_w - 1) / screen_w;
        int dst_y1 = ((y + height) * hw_h + screen_h - 1) / screen_h;
        if (dst_x0 < 0) dst_x0 = 0;
        if (dst_y0 < 0) dst_y0 = 0;
        if (dst_x1 > hw_w) dst_x1 = hw_w;
        if (dst_y1 > hw_h) dst_y1 = hw_h;

        for (int dy = dst_y0; dy < dst_y1; dy++) {
            int sy = (dy * screen_h) / hw_h;
            if (sy >= screen_h) sy = screen_h - 1;
            for (int dx = dst_x0; dx < dst_x1; dx++) {
                int sx = (dx * screen_w) / hw_w;
                if (sx >= screen_w) sx = screen_w - 1;
                lfb_put_rgb32(dx, dy,
                              surface_pixel_to_rgb32(source, sx, sy));
            }
        }
        gfx_accel_present_rect(dst_x0, dst_y0, dst_x1 - dst_x0, dst_y1 - dst_y0);
        boot_framebuffer_flush();
        return;
    }
    gfx_accel_present_rect(x, y, width, height);
    boot_framebuffer_flush();
}

void copy_fullscreen_to_lfb(const unsigned char* source) {
    copy_rect_to_lfb(source, 0, 0, screen_w, screen_h);
}

void mouse_invalidate(void) {
    g_cursor_saved_valid = false;
    g_cursor_invalidated = true;
}

void mouse_reset(unsigned char* surface) {
    if (!surface || !g_cursor_saved_valid) return;
    for (int row = 0; row < CURSOR_H; row++) {
        int py = g_cursor_saved_y + row;
        if (py < 0 || py >= screen_h) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int px = g_cursor_saved_x + col;
            if (px >= 0 && px < screen_w) {
                surface_write_raw(surface, px, py, g_cursor_saved[row][col]);
            }
        }
    }
    g_cursor_saved_valid = false;
}

static void ui_xor_pixel(unsigned char* surface, int px, int py) {
    if (px < 0 || px >= screen_w || py < 0 || py >= screen_h) return;
    if (((px + py) & 1) != 0) return;
    uint32_t mask = (screen_bpp <= 8) ? 0x0Fu :
                    ((screen_bpp <= 16) ? 0xFFFFu : 0x00FFFFFFu);
    uint32_t raw = surface_read_raw(surface, px, py);
    surface_write_raw(surface, px, py, raw ^ mask);
}

static void ui_xor_drag_box(unsigned char* surface,
                            int bx, int by, int bw, int bh) {
    if (!surface || bw <= 0 || bh <= 0) return;
    for (int i = 0; i < bw; i++) {
        ui_xor_pixel(surface, bx + i, by);
        if (bh > 1) {
            ui_xor_pixel(surface, bx + i, by + bh - 1);
        }
    }
    for (int i = 1; i < bh - 1; i++) {
        ui_xor_pixel(surface, bx, by + i);
        if (bw > 1) {
            ui_xor_pixel(surface, bx + bw - 1, by + i);
        }
    }
}

static void ui_sync_drag_box_to_lfb(const unsigned char* surface,
                                    int bx, int by, int bw, int bh) {
    if (!surface || bw <= 0 || bh <= 0) return;
    copy_rect_to_lfb(surface, bx, by, bw, 1);
    if (bh > 1) {
        copy_rect_to_lfb(surface, bx, by + bh - 1, bw, 1);
    }
    if (bh > 2) {
        copy_rect_to_lfb(surface, bx, by + 1, 1, bh - 2);
        if (bw > 1) {
            copy_rect_to_lfb(surface, bx + bw - 1, by + 1, 1, bh - 2);
        }
    }
}

bool set_resolution_ex(int width, int height, int bpp) {
    if (width < UI_MIN_WIDTH) width = UI_MIN_WIDTH;
    if (height < UI_MIN_HEIGHT) height = UI_MIN_HEIGHT;
    if (width > 16384 || height > 16384) return false;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) bpp = 16;

    uint64_t need_bytes = 0;
    size_t need_pages = ui_calc_backbuffer_pages(width, height, bpp, &need_bytes);
    if (need_pages == 0) return false;

    unsigned char* new_buf = NULL;
    phys_addr_t new_phys = 0;
    size_t new_pages = 0;
    bool reuse_existing = false;

    if (need_bytes <= UI_BOOT_FALLBACK_BYTES) {
        new_buf = g_boot_backbuffer;
    } else if (g_backbuffer_phys != 0 && need_pages <= g_backbuffer_pages &&
               (g_backbuffer_pages - need_pages) < 128) {
        reuse_existing = true;
        new_buf = backbuffer;
        new_phys = g_backbuffer_phys;
        new_pages = g_backbuffer_pages;
    } else {
        new_phys = pmm_alloc_pages(need_pages);
        if (new_phys == 0) return false;
        new_buf = (unsigned char*)phys_to_virt(new_phys);
        new_pages = need_pages;
    }

    if (g_cursor_saved_valid && backbuffer) {
        mouse_reset(backbuffer);
    }
    if (g_drag_box_saved && backbuffer) {
        ui_xor_drag_box(backbuffer, g_drag_bx, g_drag_by, g_drag_bw, g_drag_bh);
        g_drag_box_saved = false;
    }

    phys_addr_t old_phys = g_backbuffer_phys;
    size_t old_pages = g_backbuffer_pages;

    backbuffer = new_buf;
    g_backbuffer_phys = new_phys;
    g_backbuffer_pages = new_pages;

    screen_w = width;
    screen_h = height;
    screen_bpp = bpp;
    int bytes_per_pixel = screen_bpp / 8;
    screen_pitch = screen_w * bytes_per_pixel;
    lfb = (unsigned char*)g_main_surface.framebuffer_virt;

    if (!reuse_existing && old_phys != 0 && old_phys != new_phys) {
        pmm_free_pages(old_phys, old_pages);
    }

    if (g_mouse_x >= screen_w) g_mouse_x = screen_w - 1;
    if (g_mouse_y >= screen_h) g_mouse_y = screen_h - 1;
    if (g_mouse_x < 0) g_mouse_x = 0;
    if (g_mouse_y < 0) g_mouse_y = 0;

    mouse_invalidate();
    memset(backbuffer, 0, (size_t)need_bytes);
    wm_on_resolution_change(screen_w, screen_h);
    return true;
}

void set_resolution(int width, int height, int bpp) {
    (void)set_resolution_ex(width, height, bpp);
}

static void ui_draw_cursor(unsigned char* surface) {
    static const char* cursor[CURSOR_H] = {
        "X",
        "XX",
        "X.X",
        "X..X",
        "X...X",
        "X....X",
        "X.....X",
        "X......X",
        "X.......X",
        "X........X",
        "X.....XXXXX",
        "X..X..X",
        "X.X X..X",
        "XX  X..X",
        "X    X..X",
        "     X..X",
        "      XX"
    };
    if (!surface) return;
    if (g_cursor_saved_valid) {
        mouse_reset(surface);
    }
    for (int row = 0; row < CURSOR_H; row++) {
        int py = g_mouse_y + row;
        if (py < 0 || py >= screen_h) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int px = g_mouse_x + col;
            if (px >= 0 && px < screen_w) {
                g_cursor_saved[row][col] = surface_read_raw(surface, px, py);
            }
        }
    }
    g_cursor_saved_x = g_mouse_x;
    g_cursor_saved_y = g_mouse_y;
    g_cursor_saved_valid = true;

    for (int row = 0; row < CURSOR_H; row++) {
        int py = g_mouse_y + row;
        if (py < 0 || py >= screen_h) continue;
        for (int column = 0; cursor[row][column]; column++) {
            int px = g_mouse_x + column;
            if (px < 0 || px >= screen_w) continue;
            if (cursor[row][column] == 'X') {
                PUT_PIXEL(surface, px, py, 0);
            } else if (cursor[row][column] == '.') {
                PUT_PIXEL(surface, px, py, 15);
            }
        }
    }
}

void mouse_draw(unsigned char* surface) {
    ui_draw_cursor(surface);
}

void ui_desktop_start(void) {
    if (g_started) return;
    gfx_accel_init();
    audio_init();
    wm_init();
    wm_draw(backbuffer);
    ui_draw_cursor(backbuffer);
    copy_fullscreen_to_lfb(backbuffer);
    g_started = true;
}

static void ui_update_pointer(input_event_t* event) {
    if (event->type == INPUT_MOUSE_MOVE) {
        if (event->is_absolute) {
            g_mouse_x = event->abs_x;
            g_mouse_y = event->abs_y;
        } else {
            g_mouse_x += event->dx;
            g_mouse_y += event->dy;
        }
        if (g_mouse_x < 0) g_mouse_x = 0;
        if (g_mouse_y < 0) g_mouse_y = 0;
        if (g_mouse_x >= screen_w) g_mouse_x = screen_w - 1;
        if (g_mouse_y >= screen_h) g_mouse_y = screen_h - 1;
        g_mouse_buttons = event->buttons;
    }
}

void ui_desktop_poll(void) {
    if (!g_started) return;
    timer_update();
    if (!g_install_mode && vfs_poll_media()) wm_notify_media_changed();

    int prev_mx = g_cursor_saved_x;
    int prev_my = g_cursor_saved_y;
    bool had_cursor = g_cursor_saved_valid;
    g_cursor_invalidated = false;

    bool had_drag_box = g_drag_box_saved;
    int prev_bx = g_drag_bx;
    int prev_by = g_drag_by;
    int prev_bw = g_drag_bw;
    int prev_bh = g_drag_bh;

    /* Remove cursor and drag box from backbuffer before WM updates or redraws */
    mouse_reset(backbuffer);
    if (g_drag_box_saved) {
        ui_xor_drag_box(backbuffer, g_drag_bx, g_drag_by, g_drag_bw, g_drag_bh);
        g_drag_box_saved = false;
    }

    input_event_t event;
    while (input_poll_event(&event)) {
        if (event.type == INPUT_KEY_DOWN) {
            keyboard_push_virtual((int)event.code);
        } else if (event.type == INPUT_MOUSE_MOVE) {
            int pressed = 0;
            int released = 0;
            uint32_t old_buttons = g_mouse_buttons;
            ui_update_pointer(&event);
            pressed = (int)(g_mouse_buttons & ~old_buttons);
            released = (int)(old_buttons & ~g_mouse_buttons);
            wm_set_input_event_tick((int)event.timestamp);
            wm_update(backbuffer, g_mouse_x, g_mouse_y,
                      (int)g_mouse_buttons, pressed, released);
        } else if (event.type == INPUT_MOUSE_BUTTON) {
            uint32_t old_buttons = g_mouse_buttons;
            g_mouse_buttons = event.buttons;
            wm_set_input_event_tick((int)event.timestamp);
            wm_update(backbuffer, g_mouse_x, g_mouse_y,
                      (int)g_mouse_buttons,
                      (int)(g_mouse_buttons & ~old_buttons),
                      (int)(old_buttons & ~g_mouse_buttons));
        }
    }
    wm_set_input_event_tick((int)apic_get_ticks());
    wm_update(backbuffer, g_mouse_x, g_mouse_y, (int)g_mouse_buttons, 0, 0);
    wm_draw(backbuffer);

    int bx = 0, by = 0, bw = 0, bh = 0;
    bool has_drag_box = (wm_get_drag_box(&bx, &by, &bw, &bh) != 0);
    bool box_changed = (had_drag_box != has_drag_box) ||
                       (has_drag_box && (bx != prev_bx || by != prev_by ||
                                         bw != prev_bw || bh != prev_bh));

    bool full_sync = g_cursor_invalidated || (wm_need_mouse_reset() != 0);
    bool moved = (!had_cursor) || (g_mouse_x != prev_mx) || (g_mouse_y != prev_my);

    if (!full_sync) {
        if (had_drag_box && (box_changed || moved)) {
            ui_sync_drag_box_to_lfb(backbuffer, prev_bx, prev_by, prev_bw, prev_bh);
        }
        if ((moved || box_changed) && had_cursor) {
            copy_rect_to_lfb(backbuffer, prev_mx, prev_my, CURSOR_W, CURSOR_H);
        }
    }

    if (has_drag_box) {
        ui_xor_drag_box(backbuffer, bx, by, bw, bh);
        g_drag_box_saved = true;
        g_drag_bx = bx;
        g_drag_by = by;
        g_drag_bw = bw;
        g_drag_bh = bh;
    }

    ui_draw_cursor(backbuffer);

    if (full_sync) {
        copy_fullscreen_to_lfb(backbuffer);
    } else {
        if (has_drag_box && (box_changed || moved)) {
            ui_sync_drag_box_to_lfb(backbuffer, bx, by, bw, bh);
        }
        if (moved || box_changed) {
            copy_rect_to_lfb(backbuffer, g_mouse_x, g_mouse_y, CURSOR_W, CURSOR_H);
        }
    }
}

bool ui_install_mode_requested(void) {
    return g_install_mode;
}

void ui_compat_set_install_mode(bool enabled) {
    g_install_mode = enabled;
}
