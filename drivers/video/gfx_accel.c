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
extern bool set_resolution_ex(int width, int height, int bpp);
extern bool ui_can_allocate_resolution(int width, int height, int bpp);

#define MAX_PDV_DISPLAY_DRIVERS 8
#define MAX_SYNTH_MODES 48

static pdv_display_driver_t g_pdv_display_drivers[MAX_PDV_DISPLAY_DRIVERS];
static driver_descriptor_t  g_pdv_display_descs[MAX_PDV_DISPLAY_DRIVERS];
static int g_pdv_display_count = 0;

static int g_initialized = 0;
static const pdv_display_driver_t* g_active_drv = NULL;
static const char* g_fallback_reason = "Fallback LFB (max 1024x768, 8/16-bit)";
static const char* g_generic_vga_name = "Limine Linear Framebuffer";
static unsigned int g_generic_vram_bytes = 0;

static int gfx_is_valid_std_bpp(int bpp) {
    return (bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32);
}

static unsigned int gfx_bpp_to_mask_byte(int bpp) {
    if (bpp == 8)  return PDV_BPP_MASK_8;
    if (bpp == 16) return PDV_BPP_MASK_16;
    if (bpp == 24) return PDV_BPP_MASK_24;
    if (bpp == 32) return PDV_BPP_MASK_32;
    return 0U;
}

static int gfx_mode_fits_vram(int w, int h, int bpp, unsigned int vram_bytes) {
    if (w < GFX_MIN_WIDTH || h < GFX_MIN_HEIGHT || !gfx_is_valid_std_bpp(bpp)) {
        return 0;
    }
    if (vram_bytes == 0) return 1;
    uint64_t bytes = (uint64_t)(unsigned int)w * (uint64_t)(unsigned int)h *
                     (uint64_t)(unsigned int)(bpp / 8);
    return bytes <= (uint64_t)vram_bytes;
}

static int gfx_fallback_max_w(void) {
    int hw = (int)g_main_surface.width;
    if (hw <= 0) return GFX_FALLBACK_MAX_WIDTH;
    if (hw > GFX_FALLBACK_MAX_WIDTH) hw = GFX_FALLBACK_MAX_WIDTH;
    if (hw < GFX_MIN_WIDTH) hw = GFX_MIN_WIDTH;
    return hw;
}

static int gfx_fallback_max_h(void) {
    int hh = (int)g_main_surface.height;
    if (hh <= 0) return GFX_FALLBACK_MAX_HEIGHT;
    if (hh > GFX_FALLBACK_MAX_HEIGHT) hh = GFX_FALLBACK_MAX_HEIGHT;
    if (hh < GFX_MIN_HEIGHT) hh = GFX_MIN_HEIGHT;
    return hh;
}

static void gfx_build_synth_modes(pdv_display_mode_t* out_modes, int* out_count, int max_out) {
    static const int k_preset_w[] = {
        640, 800, 1024, 1152, 1280, 1280, 1280, 1280,
        1366, 1400, 1440, 1600, 1600, 1680, 1920, 1920,
        2560, 2560, 3840
    };
    static const int k_preset_h[] = {
        480, 600, 768, 864, 720, 800, 960, 1024,
        768, 1050, 900, 900, 1200, 1050, 1080, 1200,
        1440, 1600, 2160
    };
    static const int k_bpps[4] = { 8, 16, 24, 32 };
    int preset_count = (int)(sizeof(k_preset_w) / sizeof(k_preset_w[0]));
    int cnt = 0;

    for (int i = 0; i < preset_count && cnt < max_out; i++) {
        int w = k_preset_w[i];
        int h = k_preset_h[i];
        uint8_t mask = 0;
        for (int b = 0; b < 4; b++) {
            if (gfx_accel_supports_bpp(w, h, k_bpps[b])) {
                mask |= (uint8_t)gfx_bpp_to_mask_byte(k_bpps[b]);
            }
        }
        if (mask != 0) {
            out_modes[cnt].width = (uint16_t)w;
            out_modes[cnt].height = (uint16_t)h;
            out_modes[cnt].bpp_mask = mask;
            out_modes[cnt].preferred = (w == screen_w && h == screen_h) ? 1U : 0U;
            out_modes[cnt].reserved = 0U;
            cnt++;
        }
    }

    int boot_w = (int)g_main_surface.width;
    int boot_h = (int)g_main_surface.height;
    if (!g_active_drv) {
        if (boot_w > GFX_FALLBACK_MAX_WIDTH) boot_w = GFX_FALLBACK_MAX_WIDTH;
        if (boot_h > GFX_FALLBACK_MAX_HEIGHT) boot_h = GFX_FALLBACK_MAX_HEIGHT;
    }
    if (boot_w >= GFX_MIN_WIDTH && boot_h >= GFX_MIN_HEIGHT && cnt < max_out) {
        int exists = 0;
        for (int j = 0; j < cnt; j++) {
            if (out_modes[j].width == (uint16_t)boot_w &&
                out_modes[j].height == (uint16_t)boot_h) {
                exists = 1;
                break;
            }
        }
        if (!exists) {
            uint8_t mask = 0;
            for (int b = 0; b < 4; b++) {
                if (gfx_accel_supports_bpp(boot_w, boot_h, k_bpps[b])) {
                    mask |= (uint8_t)gfx_bpp_to_mask_byte(k_bpps[b]);
                }
            }
            if (mask != 0) {
                out_modes[cnt].width = (uint16_t)boot_w;
                out_modes[cnt].height = (uint16_t)boot_h;
                out_modes[cnt].bpp_mask = mask;
                out_modes[cnt].preferred = 1U;
                out_modes[cnt].reserved = 0U;
                cnt++;
            }
        }
    }

    *out_count = cnt;
}

static bool gfx_names_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int gfx_accel_register_pdv_driver(const pdv_display_driver_t* drv) {
    if (!drv || !drv->name || !drv->probe) return -1;

    int slot = -1;
    for (int i = 0; i < g_pdv_display_count; i++) {
        if (gfx_names_equal(g_pdv_display_drivers[i].name, drv->name)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_pdv_display_count >= MAX_PDV_DISPLAY_DRIVERS) return -1;
        slot = g_pdv_display_count++;
    }
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
    /* Legacy DDK stub preserved for ABI slot compatibility; mach64 enforces its own VRAM caps */
    klog_info("[GFX ] fb_apply_ati_cap ignored (capability delegated to mach64 driver)");
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
        g_fallback_reason = "No valid display driver (max 1024x768, 8/16-bit)";
        int fw = screen_w;
        int fh = screen_h;
        int fbpp = screen_bpp;
        int max_w = gfx_fallback_max_w();
        int max_h = gfx_fallback_max_h();
        if (fw > max_w) fw = max_w;
        if (fh > max_h) fh = max_h;
        if (fw < GFX_MIN_WIDTH) fw = GFX_MIN_WIDTH;
        if (fh < GFX_MIN_HEIGHT) fh = GFX_MIN_HEIGHT;
        if (fbpp != 8 && fbpp != 16) fbpp = 16;
        if (fw != screen_w || fh != screen_h || fbpp != screen_bpp) {
            (void)set_resolution_ex(fw, fh, fbpp);
        }
        klog_info("[GFX ] fallback LFB active (%s): mode %dx%dx%d (max %dx%dx16)",
                  g_generic_vga_name, screen_w, screen_h, screen_bpp,
                  max_w, max_h);
    } else {
        if (!gfx_accel_can_change_mode(screen_w, screen_h, screen_bpp)) {
            int cnt = gfx_accel_get_mode_count();
            int picked = 0;
            for (int i = cnt - 1; i >= 0; i--) {
                int mw = 0, mh = 0;
                unsigned char mask = 0;
                if (gfx_accel_get_mode(i, &mw, &mh, &mask, NULL) && mask != 0) {
                    static const int try_bpps[4] = { 16, 32, 24, 8 };
                    for (int b = 0; b < 4; b++) {
                        if (mask & (unsigned char)gfx_bpp_to_mask_byte(try_bpps[b])) {
                            if (gfx_accel_try_set_mode(mw, mh, try_bpps[b])) {
                                picked = 1;
                                break;
                            }
                        }
                    }
                    if (picked) break;
                }
            }
        }
        klog_info("[GFX ] active backend: %s, status: %s (VRAM=%uKB)",
                  gfx_accel_get_name(), gfx_accel_get_status(),
                  gfx_accel_get_vram_size() / 1024U);
    }
}

int gfx_accel_is_active(void) {
    if (!g_initialized) gfx_accel_init();
    if (g_active_drv) return 1;
    return g_main_surface.framebuffer_virt != NULL;
}

int gfx_accel_has_valid_driver(void) {
    if (!g_initialized) gfx_accel_init();
    return g_active_drv != NULL ? 1 : 0;
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

static int gfx_collect_modes(pdv_display_mode_t* out, int max_out) {
    if (!out || max_out <= 0) return 0;
    int cnt = 0;
    if (g_active_drv && g_active_drv->get_mode_count && g_active_drv->get_mode) {
        int drv_cnt = g_active_drv->get_mode_count();
        for (int i = 0; i < drv_cnt && cnt < max_out; i++) {
            pdv_display_mode_t m;
            memset(&m, 0, sizeof(m));
            if (!g_active_drv->get_mode(i, &m)) continue;
            if (m.width < GFX_MIN_WIDTH || m.height < GFX_MIN_HEIGHT) continue;
            static const int k_bpps[4] = { 8, 16, 24, 32 };
            uint8_t valid_mask = 0;
            for (int b = 0; b < 4; b++) {
                uint8_t bit = (uint8_t)gfx_bpp_to_mask_byte(k_bpps[b]);
                if ((m.bpp_mask & bit) &&
                    ui_can_allocate_resolution((int)m.width, (int)m.height, k_bpps[b])) {
                    valid_mask |= bit;
                }
            }
            if (valid_mask == 0) continue;
            m.bpp_mask = valid_mask;
            out[cnt++] = m;
        }
        return cnt;
    }
    gfx_build_synth_modes(out, &cnt, max_out);
    return cnt;
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
    if (fb_bytes < 2U * 1024U * 1024U) fb_bytes = 2U * 1024U * 1024U;
    return fb_bytes;
}

int gfx_accel_get_max_width(void) {
    if (!g_initialized) gfx_accel_init();
    if (!g_active_drv) {
        return gfx_fallback_max_w();
    }
    if (g_active_drv->get_max_width) {
        int mw = g_active_drv->get_max_width();
        if (mw >= GFX_MIN_WIDTH) return mw;
    }
    pdv_display_mode_t modes[MAX_SYNTH_MODES];
    int cnt = gfx_collect_modes(modes, MAX_SYNTH_MODES);
    int max_w = (int)g_main_surface.width;
    if (max_w < GFX_MIN_WIDTH) max_w = GFX_MIN_WIDTH;
    for (int i = 0; i < cnt; i++) {
        if ((int)modes[i].width > max_w) max_w = (int)modes[i].width;
    }
    return max_w;
}

int gfx_accel_get_max_height(void) {
    if (!g_initialized) gfx_accel_init();
    if (!g_active_drv) {
        return gfx_fallback_max_h();
    }
    if (g_active_drv->get_max_height) {
        int mh = g_active_drv->get_max_height();
        if (mh >= GFX_MIN_HEIGHT) return mh;
    }
    pdv_display_mode_t modes[MAX_SYNTH_MODES];
    int cnt = gfx_collect_modes(modes, MAX_SYNTH_MODES);
    int max_h = (int)g_main_surface.height;
    if (max_h < GFX_MIN_HEIGHT) max_h = GFX_MIN_HEIGHT;
    for (int i = 0; i < cnt; i++) {
        if ((int)modes[i].height > max_h) max_h = (int)modes[i].height;
    }
    return max_h;
}

int gfx_accel_get_supported_bpp_mask(void) {
    if (!g_initialized) gfx_accel_init();
    if (!g_active_drv) {
        return (int)(PDV_BPP_MASK_8 | PDV_BPP_MASK_16);
    }
    pdv_display_mode_t modes[MAX_SYNTH_MODES];
    int cnt = gfx_collect_modes(modes, MAX_SYNTH_MODES);
    int mask = 0;
    for (int i = 0; i < cnt; i++) {
        mask |= (int)(modes[i].bpp_mask & PDV_BPP_MASK_ALL_STD);
    }
    if (mask == 0) {
        static const int k_bpps[4] = { 8, 16, 24, 32 };
        for (int b = 0; b < 4; b++) {
            if (gfx_accel_supports_bpp(GFX_MIN_WIDTH, GFX_MIN_HEIGHT, k_bpps[b])) {
                mask |= (int)gfx_bpp_to_mask_byte(k_bpps[b]);
            }
        }
    }
    return mask;
}

int gfx_accel_get_mode_count(void) {
    if (!g_initialized) gfx_accel_init();
    pdv_display_mode_t modes[MAX_SYNTH_MODES];
    return gfx_collect_modes(modes, MAX_SYNTH_MODES);
}

int gfx_accel_get_mode(int index, int* out_w, int* out_h,
                       unsigned char* out_bpp_mask,
                       unsigned char* out_preferred) {
    if (!g_initialized) gfx_accel_init();
    if (index < 0) return 0;
    pdv_display_mode_t modes[MAX_SYNTH_MODES];
    int cnt = gfx_collect_modes(modes, MAX_SYNTH_MODES);
    if (index >= cnt) return 0;
    if (out_w) *out_w = (int)modes[index].width;
    if (out_h) *out_h = (int)modes[index].height;
    if (out_bpp_mask) *out_bpp_mask = (unsigned char)(modes[index].bpp_mask & PDV_BPP_MASK_ALL_STD);
    if (out_preferred) *out_preferred = modes[index].preferred;
    return 1;
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
    if (w < GFX_MIN_WIDTH || h < GFX_MIN_HEIGHT || w > 16384 || h > 16384) return 0;
    if (!gfx_is_valid_std_bpp(bpp)) return 0;

    if (!g_active_drv) {
        if (g_main_surface.framebuffer_virt == NULL) return 0;
        if (bpp != 8 && bpp != 16) return 0;
        if (w > gfx_fallback_max_w() || h > gfx_fallback_max_h()) return 0;
        if (!gfx_mode_fits_vram(w, h, bpp, gfx_accel_get_vram_size())) return 0;
        return ui_can_allocate_resolution(w, h, bpp) ? 1 : 0;
    }

    if (g_active_drv->can_set_mode) {
        if (!g_active_drv->can_set_mode(w, h, bpp)) return 0;
    } else {
        if (w > (int)g_main_surface.width || h > (int)g_main_surface.height) return 0;
        if (!gfx_mode_fits_vram(w, h, bpp, gfx_accel_get_vram_size())) return 0;
        if (g_active_drv->is_active && !g_active_drv->is_active()) return 0;
    }

    return ui_can_allocate_resolution(w, h, bpp) ? 1 : 0;
}

int gfx_accel_supports_bpp(int w, int h, int bpp) {
    if (!g_initialized) gfx_accel_init();
    if (w < GFX_MIN_WIDTH || h < GFX_MIN_HEIGHT || w > 16384 || h > 16384) return 0;
    if (!gfx_is_valid_std_bpp(bpp)) return 0;
    if (g_active_drv && g_active_drv->supports_bpp) {
        if (!g_active_drv->supports_bpp(w, h, bpp)) return 0;
        return ui_can_allocate_resolution(w, h, bpp) ? 1 : 0;
    }
    return gfx_accel_can_change_mode(w, h, bpp);
}

int gfx_accel_try_set_mode(int w, int h, int bpp) {
    if (!gfx_accel_can_change_mode(w, h, bpp)) return 0;
    if (g_active_drv && g_active_drv->requires_hw_mode_set) {
        if (!ui_can_allocate_resolution(w, h, bpp)) return 0;
        int prev_w = screen_w;
        int prev_h = screen_h;
        int prev_bpp = screen_bpp;
        if (!g_active_drv->set_mode || !g_active_drv->set_mode(w, h, bpp)) return 0;
        if (!set_resolution_ex(w, h, bpp)) {
            if (prev_w >= GFX_MIN_WIDTH && prev_h >= GFX_MIN_HEIGHT) {
                (void)g_active_drv->set_mode(prev_w, prev_h, prev_bpp);
            }
            return 0;
        }
        return 1;
    }

    if (!set_resolution_ex(w, h, bpp)) return 0;
    if (g_active_drv && g_active_drv->set_mode) {
        (void)g_active_drv->set_mode(w, h, bpp);
    }
    return 1;
}

void gfx_accel_set_mode(int w, int h, int bpp) {
    (void)gfx_accel_try_set_mode(w, h, bpp);
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
