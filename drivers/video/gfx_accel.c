#include "gfx_accel.h"
#include "ati_mach64.h"
#include "boot_framebuffer.h"
#include "driver.h"
#include "log.h"
#include "nvidia_g7x.h"
#include "pci.h"
#include "vmware_svga.h"

extern int screen_w;
extern int screen_h;
extern int screen_bpp;
extern int screen_pitch;
extern unsigned char* lfb;
extern void set_resolution(int width, int height, int bpp);

typedef struct {
    const char* name;
    int kind;
    void (*present_rect)(int x, int y, int width, int height);
    void (*fill_rect)(int x, int y, int width, int height,
                      unsigned int color);
    void (*copy_rect)(int sx, int sy, int dx, int dy, int width, int height);
    void (*sync)(void);
} gfx_backend_t;

#define GFX_BACKEND_SOFTWARE 0
#define GFX_BACKEND_VMWARE   1
#define GFX_BACKEND_ATI      2
#define GFX_BACKEND_NVIDIA   3

static int g_initialized;
static int g_drivers_registered;
static int g_backend_kind = GFX_BACKEND_SOFTWARE;
static const gfx_backend_t* g_backend;
static const char* g_fallback_reason = "WC linear framebuffer active";
static const char* g_generic_vga_name = "Limine Linear Framebuffer";
static unsigned int g_generic_vram_bytes = 0;

static void backend_svga_present(int x, int y, int width, int height) {
    vmware_svga_present_rect(x, y, width, height);
}

static void backend_svga_fill(int x, int y, int width, int height,
                              unsigned int color) {
    vmware_svga_fill_rect(x, y, width, height, color);
}

static void backend_svga_copy(int sx, int sy, int dx, int dy,
                              int width, int height) {
    vmware_svga_screen_blt(sx, sy, dx, dy, width, height);
}

static void backend_ati_present(int x, int y, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void backend_ati_fill(int x, int y, int width, int height,
                             unsigned int color) {
    ati_mach64_fill_rect(x, y, width, height, color);
}

static void backend_ati_copy(int sx, int sy, int dx, int dy,
                             int width, int height) {
    ati_mach64_screen_blt(sx, sy, dx, dy, width, height);
}

static void backend_nvidia_present(int x, int y, int width, int height) {
    nvidia_g7x_present_rect(x, y, width, height);
}

static void backend_nvidia_fill(int x, int y, int width, int height,
                                unsigned int color) {
    nvidia_g7x_fill_rect(x, y, width, height, color);
}

static void backend_nvidia_copy(int sx, int sy, int dx, int dy,
                                int width, int height) {
    nvidia_g7x_screen_blt(sx, sy, dx, dy, width, height);
}

static void backend_software_present(int x, int y, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void backend_software_fill(int x, int y, int width, int height,
                                  unsigned int color) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)color;
}

static void backend_software_copy(int sx, int sy, int dx, int dy,
                                  int width, int height) {
    (void)sx;
    (void)sy;
    (void)dx;
    (void)dy;
    (void)width;
    (void)height;
}

static const gfx_backend_t g_svga_backend = {
    "VMware SVGA II",
    GFX_BACKEND_VMWARE,
    backend_svga_present,
    backend_svga_fill,
    backend_svga_copy,
    vmware_svga_sync
};

static const gfx_backend_t g_ati_backend = {
    "ATI Mach64",
    GFX_BACKEND_ATI,
    backend_ati_present,
    backend_ati_fill,
    backend_ati_copy,
    ati_mach64_sync
};

static const gfx_backend_t g_nvidia_backend = {
    "NVIDIA G7x PCIe",
    GFX_BACKEND_NVIDIA,
    backend_nvidia_present,
    backend_nvidia_fill,
    backend_nvidia_copy,
    nvidia_g7x_sync
};

static const gfx_backend_t g_software_backend = {
    "Limine Linear Framebuffer",
    GFX_BACKEND_SOFTWARE,
    backend_software_present,
    backend_software_fill,
    backend_software_copy,
    boot_framebuffer_flush
};

static int probe_vmware_svga(void) {
    if (!vmware_svga_probe()) return 0;
    int w = screen_w > 0 ? screen_w : (int)g_main_surface.width;
    int h = screen_h > 0 ? screen_h : (int)g_main_surface.height;
    int bpp = screen_bpp > 0 ? screen_bpp : 32;
    if (w <= 0 || w > 1280) w = 1024;
    if (h <= 0 || h > 1024) h = 768;
    if (!vmware_svga_set_mode(w, h, bpp)) return 0;
    return 1;
}

static int probe_ati_mach64(void) {
    if (!ati_mach64_probe()) return 0;
    ati_mach64_set_mode_bpp(screen_w, screen_h, screen_bpp);
    return 1;
}

static int probe_nvidia_g7x(void) {
    return nvidia_g7x_probe();
}

static const driver_descriptor_t g_vmware_driver = {
    "VMware SVGA II",
    DRIVER_CATEGORY_DISPLAY,
    100,
    probe_vmware_svga,
    0,
    0
};

static const driver_descriptor_t g_nvidia_driver = {
    "NVIDIA G7x PCIe",
    DRIVER_CATEGORY_DISPLAY,
    80,
    probe_nvidia_g7x,
    0,
    0
};

static const driver_descriptor_t g_ati_driver = {
    "ATI Mach64",
    DRIVER_CATEGORY_DISPLAY,
    50,
    probe_ati_mach64,
    0,
    0
};

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

static void register_gfx_drivers(void) {
    if (g_drivers_registered) return;
    driver_register(&g_vmware_driver);
    driver_register(&g_nvidia_driver);
    driver_register(&g_ati_driver);
    g_drivers_registered = 1;
}

void gfx_accel_init(void) {
    if (g_initialized) return;

    detect_generic_vga_info();
    register_gfx_drivers();
    g_backend = &g_software_backend;
    g_backend_kind = GFX_BACKEND_SOFTWARE;

    if (driver_probe_category(DRIVER_CATEGORY_DISPLAY)) {
        if (driver_is_active(DRIVER_CATEGORY_DISPLAY, "VMware SVGA II")) {
            g_backend = &g_svga_backend;
            g_backend_kind = GFX_BACKEND_VMWARE;
            g_fallback_reason = "None";
        } else if (driver_is_active(DRIVER_CATEGORY_DISPLAY,
                                    "NVIDIA G7x PCIe")) {
            g_backend = &g_nvidia_backend;
            g_backend_kind = GFX_BACKEND_NVIDIA;
            g_fallback_reason = "None";
        } else if (driver_is_active(DRIVER_CATEGORY_DISPLAY, "ATI Mach64")) {
            g_backend = &g_ati_backend;
            g_backend_kind = GFX_BACKEND_ATI;
            g_fallback_reason = "None";
        }
    }

    g_initialized = 1;
    if (g_backend_kind == GFX_BACKEND_SOFTWARE) {
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
    if (g_backend_kind != GFX_BACKEND_SOFTWARE) return 1;
    return g_main_surface.framebuffer_virt != 0;
}

const char* gfx_accel_get_name(void) {
    if (!g_initialized) gfx_accel_init();
    if (g_backend_kind == GFX_BACKEND_VMWARE) {
        return vmware_svga_get_name();
    }
    if (g_backend_kind == GFX_BACKEND_ATI) {
        return ati_mach64_get_chip_name();
    }
    if (g_backend_kind == GFX_BACKEND_NVIDIA) {
        return nvidia_g7x_get_name();
    }
    return g_generic_vga_name;
}

const char* gfx_accel_get_status(void) {
    if (!g_initialized) gfx_accel_init();
    if (g_backend_kind == GFX_BACKEND_VMWARE) {
        return vmware_svga_get_status();
    }
    if (g_backend_kind == GFX_BACKEND_ATI) {
        return "GUI accelerator active";
    }
    if (g_backend_kind == GFX_BACKEND_NVIDIA) {
        return nvidia_g7x_get_status();
    }
    return g_fallback_reason;
}

const char* gfx_accel_get_fallback_reason(void) {
    return g_fallback_reason;
}

unsigned int gfx_accel_get_vram_size(void) {
    if (!g_initialized) gfx_accel_init();
    if (g_backend_kind == GFX_BACKEND_VMWARE) {
        return vmware_svga_get_vram_size();
    }
    if (g_backend_kind == GFX_BACKEND_ATI) {
        return ati_mach64_get_vram_size();
    }
    if (g_backend_kind == GFX_BACKEND_NVIDIA) {
        return nvidia_g7x_get_vram_size();
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
    return g_backend_kind == GFX_BACKEND_VMWARE ||
           g_backend_kind == GFX_BACKEND_ATI;
}

int gfx_accel_supports_1280x1024(void) {
    return 1;
}

int gfx_accel_supports_bpp(int w, int h, int bpp) {
    if (w < 320 || w > 1280 || h < 200 || h > 1024) return 0;
    return (bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32);
}

int gfx_accel_try_set_mode(int w, int h, int bpp) {
    if (!gfx_accel_supports_bpp(w, h, bpp)) return 0;
    set_resolution(w, h, bpp);
    gfx_accel_set_mode(w, h, bpp);
    return 1;
}

void gfx_accel_set_mode(int w, int h, int bpp) {
    if (!gfx_accel_supports_bpp(w, h, bpp)) return;
    if (screen_w != w || screen_h != h || screen_bpp != bpp) {
        set_resolution(w, h, bpp);
    }
    if (g_backend_kind == GFX_BACKEND_VMWARE) {
        vmware_svga_set_mode(w, h, bpp);
    } else if (g_backend_kind == GFX_BACKEND_ATI) {
        ati_mach64_set_mode_bpp(w, h, bpp);
    }
}

void gfx_accel_fill_rect(int x, int y, int w, int h, unsigned int color) {
    if (g_backend && g_backend->fill_rect) {
        g_backend->fill_rect(x, y, w, h, color);
    }
}

void gfx_accel_screen_blt(int sx, int sy, int dx, int dy, int w, int h) {
    if (g_backend && g_backend->copy_rect) {
        g_backend->copy_rect(sx, sy, dx, dy, w, h);
    }
}

void gfx_accel_present_rect(int x, int y, int w, int h) {
    if (g_backend && g_backend->present_rect) {
        g_backend->present_rect(x, y, w, h);
    }
}

void gfx_accel_sync(void) {
    if (g_backend && g_backend->sync) {
        g_backend->sync();
    } else {
        boot_framebuffer_flush();
    }
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
