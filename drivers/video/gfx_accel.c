#include "gfx_accel.h"
#include "boot_framebuffer.h"
#include "driver.h"
#include "log.h"
#include "pci.h"
#include "pdv.h"
#include "string.h"

extern int screen_w;
extern int screen_h;
extern int screen_bpp;
extern int screen_pitch;
extern unsigned char* lfb;
extern void set_resolution(int width, int height, int bpp);

#define MAX_PDV_DISPLAY_DRIVERS 8

static pdv_display_driver_t g_pdv_display_drivers[MAX_PDV_DISPLAY_DRIVERS];
static driver_descriptor_t  g_pdv_display_descs[MAX_PDV_DISPLAY_DRIVERS];
static int g_pdv_display_count = 0;

static int g_initialized = 0;
static const pdv_display_driver_t* g_active_drv = NULL;
static const char* g_fallback_reason = "WC linear framebuffer active";
static const char* g_generic_vga_name = "Limine Linear Framebuffer";
static unsigned int g_generic_vram_bytes = 0;
static int g_ati_detected = 0;

int gfx_accel_register_pdv_driver(const pdv_display_driver_t* drv) {
    if (!drv || !drv->name || !drv->probe) return -1;
    if (g_pdv_display_count >= MAX_PDV_DISPLAY_DRIVERS) return -1;

    int slot = g_pdv_display_count++;
    g_pdv_display_drivers[slot] = *drv;
    g_pdv_display_descs[slot].name     = g_pdv_display_drivers[slot].name;
    g_pdv_display_descs[slot].category = DRIVER_CATEGORY_DISPLAY;
    g_pdv_display_descs[slot].priority = g_pdv_display_drivers[slot].priority;
    g_pdv_display_descs[slot].probe    = g_pdv_display_drivers[slot].probe;
    g_pdv_display_descs[slot].start    = g_pdv_display_drivers[slot].start;
    g_pdv_display_descs[slot].stop     = g_pdv_display_drivers[slot].stop;

    return driver_register(&g_pdv_display_descs[slot]);
}

void gfx_accel_notify_ati_cap(void) {
    g_ati_detected = 1;
    int width = screen_w > 0 ? screen_w : 1024;
    int height = screen_h > 0 ? screen_h : 768;
    int bpp = screen_bpp > 0 ? screen_bpp : 16;
    if (width > 1024 || height > 768) {
        width = 1024;
        height = 768;
    }
    if (bpp > 16) bpp = 16;
    if (width != screen_w || height != screen_h || bpp != screen_bpp) {
        set_resolution(width, height, bpp);
    }
    klog_info("[GFX] ATI detected; acceleration and mode switching disabled, "
              "maximum 1024x768x16");
}

static void detect_generic_vga_info(void) {
    pci_scan_all();
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev || dev->class_code != 0x03) continue;
        for (int b = 0; b < 6; b++) {
            if (dev->bar[b].valid && !dev->bar[b].is_io &&
                dev->bar[b].size >= 4ULL * 1024ULL * 1024ULL &&
                dev->bar[b].size > g_generic_vram_bytes) {
                g_generic_vram_bytes = (unsigned int)dev->bar[b].size;
            }
        }
        if (dev->vendor == 0x80EE && dev->device == 0xBEEF) {
            g_generic_vga_name = "VirtualBox Graphics Adapter (VBoxVGA)";
            return;
        }
        if (dev->vendor == 0x1234 && dev->device == 0x1111) {
            g_generic_vga_name = "QEMU/Bochs Standard VGA";
            return;
        }
        if (dev->vendor == 0x8086) {
            if (dev->device == 0x0042 || dev->device == 0x0046) {
                g_generic_vga_name = "Intel HD Graphics (Ironlake/Clarkdale)";
            } else {
                g_generic_vga_name = "Intel Integrated Graphics";
            }
            return;
        }
    }
}

void gfx_accel_init(void) {
    if (g_initialized) return;

    detect_generic_vga_info();
    g_active_drv = NULL;

    if (driver_probe_category(DRIVER_CATEGORY_DISPLAY)) {
        for (int i = 0; i < g_pdv_display_count; i++) {
            if (driver_is_active(DRIVER_CATEGORY_DISPLAY,
                                 g_pdv_display_drivers[i].name)) {
                g_active_drv = &g_pdv_display_drivers[i];
                g_fallback_reason = "None";
                break;
            }
        }
    }

    g_initialized = 1;
    if (!g_active_drv) {
        g_fallback_reason = "WC framebuffer active";
        klog_info("[GFX ] using linear WC framebuffer backend (%s)",
                  g_generic_vga_name);
    } else {
        klog_info("[GFX ] active backend: %s, status: %s",
                  gfx_accel_get_name(), gfx_accel_get_status());
    }
}

int gfx_accel_is_active(void) {
    if (!g_initialized) gfx_accel_init();
    if (g_active_drv) return 1;
    return g_main_surface.framebuffer_virt != NULL;
}

const char* gfx_accel_get_name(void) {
    if (!g_initialized) gfx_accel_init();
    if (g_active_drv && g_active_drv->get_name) {
        return g_active_drv->get_name();
    }
    if (g_active_drv) {
        return g_active_drv->name;
    }
    return g_generic_vga_name;
}

const char* gfx_accel_get_status(void) {
    if (!g_initialized) gfx_accel_init();
    if (g_active_drv && g_active_drv->get_status) {
        return g_active_drv->get_status();
    }
    if (g_active_drv) {
        return "GUI accelerator active";
    }
    return g_fallback_reason;
}

const char* gfx_accel_get_fallback_reason(void) {
    return g_fallback_reason;
}

unsigned int gfx_accel_get_vram_size(void) {
    if (!g_initialized) gfx_accel_init();
    if (g_active_drv && g_active_drv->get_vram_size) {
        unsigned int v = g_active_drv->get_vram_size();
        if (v > 0) return v;
    }
    if (g_generic_vram_bytes > 0) {
        return g_generic_vram_bytes;
    }
    unsigned int fb_bytes =
        (unsigned int)g_main_surface.pitch * (unsigned int)g_main_surface.height;
    if (fb_bytes < 16U * 1024U * 1024U) fb_bytes = 16U * 1024U * 1024U;
    return fb_bytes;
}

int gfx_accel_is_hardware_2d_active(void) {
    if (!g_initialized) gfx_accel_init();
    if (g_active_drv && g_active_drv->is_2d_active) {
        return g_active_drv->is_2d_active();
    }
    return 0;
}

int gfx_accel_supports_1280x1024(void) {
    return gfx_accel_can_change_mode(1280, 1024, 32);
}

int gfx_accel_can_change_mode(int w, int h, int bpp) {
    if (!g_initialized) gfx_accel_init();
    if (w < 320 || w > 1280 || h < 200 || h > 1024) return 0;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    if (g_ati_detected) {
        return w <= 1024 && h <= 768 && (bpp == 8 || bpp == 16) &&
               g_main_surface.framebuffer_virt != NULL;
    }

    if (g_active_drv) {
        if (g_active_drv->can_set_mode) {
            return g_active_drv->can_set_mode(w, h, bpp);
        }
        if (w > (int)g_main_surface.width || h > (int)g_main_surface.height) return 0;
        if (g_active_drv->is_active) {
            return g_active_drv->is_active();
        }
        return 1;
    }

    if (w > (int)g_main_surface.width || h > (int)g_main_surface.height) return 0;
    return g_main_surface.framebuffer_virt != NULL;
}

int gfx_accel_supports_bpp(int w, int h, int bpp) {
    if (g_active_drv && g_active_drv->supports_bpp) {
        return g_active_drv->supports_bpp(w, h, bpp);
    }
    return gfx_accel_can_change_mode(w, h, bpp);
}

int gfx_accel_try_set_mode(int w, int h, int bpp) {
    if (!gfx_accel_can_change_mode(w, h, bpp)) return 0;
    if (g_active_drv && g_active_drv->requires_hw_mode_set) {
        if (!g_active_drv->set_mode || !g_active_drv->set_mode(w, h, bpp)) return 0;
        set_resolution(w, h, bpp);
    } else {
        set_resolution(w, h, bpp);
        gfx_accel_set_mode(w, h, bpp);
    }
    return 1;
}

void gfx_accel_set_mode(int w, int h, int bpp) {
    if (!gfx_accel_can_change_mode(w, h, bpp)) return;
    if (screen_w != w || screen_h != h || screen_bpp != bpp) {
        set_resolution(w, h, bpp);
    }
    if (g_active_drv && g_active_drv->set_mode) {
        (void)g_active_drv->set_mode(w, h, bpp);
    }
}

void gfx_accel_fill_rect(int x, int y, int w, int h, unsigned int color) {
    if (g_active_drv && g_active_drv->fill_rect) {
        g_active_drv->fill_rect(x, y, w, h, color);
    }
}

void gfx_accel_screen_blt(int sx, int sy, int dx, int dy, int w, int h) {
    if (g_active_drv && g_active_drv->screen_blt) {
        g_active_drv->screen_blt(sx, sy, dx, dy, w, h);
    }
}

void gfx_accel_present_rect(int x, int y, int w, int h) {
    if (g_active_drv && g_active_drv->present_rect) {
        g_active_drv->present_rect(x, y, w, h);
    }
}

void gfx_accel_sync(void) {
    if (g_active_drv && g_active_drv->sync) {
        g_active_drv->sync();
    } else {
        boot_framebuffer_flush();
    }
}

int gfx_accel_has_hardware_cursor(void) {
    if (g_active_drv && g_active_drv->has_hw_cursor) {
        return g_active_drv->has_hw_cursor();
    }
    return 0;
}

int gfx_accel_set_cursor(int x, int y, int visible) {
    if (g_active_drv && g_active_drv->set_cursor) {
        return g_active_drv->set_cursor(x, y, visible);
    }
    (void)x;
    (void)y;
    (void)visible;
    return 0;
}
