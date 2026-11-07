#include <pdv/ddk.h>

static const pdv_kernel_api_t* g_kapi;

#define klog_info(fmt, ...) g_kapi->log_printf(fmt "\n", ##__VA_ARGS__)
#define mmio_read32         pdv_mmio_read32

#define NVIDIA_VENDOR_ID       0x10DE
#define PCI_DEV_7600GT_G73     0x0391
#define PCI_DEV_7600GS_G73     0x0392
#define PCI_DEV_7300GT_G73     0x0393
#define PCI_DEV_7300GT_G73_B   0x0395
#define PCI_DEV_7300GT_AGP     0x02E2
#define PCI_DEV_7300GS_G72     0x01DF
#define PCI_DEV_7300LE_G72     0x01D1
#define PCI_DEV_7300SE_G72     0x01D3
#define PCI_DEV_6200LE_NV44    0x0163
#define PCI_DEV_6600LE_NV43_A  0x00F4
#define PCI_DEV_6600LE_NV43_B  0x0142
#define PCI_CAP_ID_EXP         0x10

static const pdv_pci_device_t* g_pci;
static int g_active;
static unsigned int g_vram_size;
static uint64_t g_bar0;
static uint64_t g_bar1;
static uint64_t g_bar1_size;
static int g_primary_7300gt;
static const char* g_name = "NVIDIA PCIe";
static const char* g_status = "Not detected";

static unsigned int nvidia_bytes_per_pixel(int bpp) {
    if (bpp <= 8) return 1;
    if (bpp <= 16) return 2;
    if (bpp <= 24) return 3;
    return 4;
}

static int nvidia_framebuffer_in_bar(uint64_t framebuffer, uint64_t size) {
    if (!g_bar1 || !framebuffer || framebuffer < g_bar1) return 0;
    uint64_t offset = framebuffer - g_bar1;
    if (size && offset >= size) return 0;
    return 1;
}

static unsigned int nvidia_probe_hw_vram(uint64_t bar0_phys) {
    if (!bar0_phys) return 0;
    volatile uint8_t* pfb =
        (volatile uint8_t*)g_kapi->mmio_map_uc(bar0_phys + 0x100000ULL, 0x1000);
    if (!pfb) return 0;
    uint32_t cstatus = mmio_read32(pfb + 0x20C);
    if (cstatus == 0 || cstatus == 0xFFFFFFFFU) return 0;
    uint32_t bytes = cstatus & 0xFFF00000U;
    if (bytes >= (16U * 1024U * 1024U) && bytes <= (1024U * 1024U * 1024U)) {
        return bytes;
    }
    return 0;
}

static int nvidia_g7x_probe(void) {
    g_active = 0;
    g_pci = 0;
    g_vram_size = 0;
    g_bar0 = 0;
    g_bar1 = 0;
    g_bar1_size = 0;
    g_primary_7300gt = 0;
    g_status = "NVIDIA G7x PCIe device not found";

    unsigned char* cur_lfb = g_kapi->fb_get_lfb();
    int sw = 0, sh = 0, sbpp = 0;
    g_kapi->fb_get_desktop_mode(&sw, &sh, &sbpp);
    pdv_fb_surface_t surf;
    g_kapi->fb_get_surface(&surf);

    for (int i = 0; i < g_kapi->pci_get_device_count(); i++) {
        const pdv_pci_device_t* dev = g_kapi->pci_get_device(i);
        if (!dev || dev->vendor != NVIDIA_VENDOR_ID) continue;

        unsigned int vram = 0;
        switch (dev->device) {
            case PCI_DEV_7300GT_G73:
            case PCI_DEV_7300GT_G73_B:
            case PCI_DEV_7300GT_AGP:
                g_name = "NVIDIA GeForce 7300 GT (G73 PCIe)";
                vram = 256U * 1024U * 1024U;
                g_primary_7300gt = 1;
                break;
            case PCI_DEV_7600GT_G73:
                g_name = "NVIDIA GeForce 7600 GT (G73 PCIe)";
                vram = 256U * 1024U * 1024U;
                g_primary_7300gt = 1;
                break;
            case PCI_DEV_7600GS_G73:
                g_name = "NVIDIA GeForce 7600 GS (G73 PCIe)";
                vram = 256U * 1024U * 1024U;
                g_primary_7300gt = 1;
                break;
            case PCI_DEV_7300GS_G72:
            case PCI_DEV_7300LE_G72:
            case PCI_DEV_7300SE_G72:
                g_name = "NVIDIA GeForce 7300 Series (G72 PCIe)";
                vram = 256U * 1024U * 1024U;
                g_primary_7300gt = 1;
                break;
            case PCI_DEV_6200LE_NV44:
                g_name = "NVIDIA GeForce 6200 LE (NV44 PCIe)";
                vram = 128U * 1024U * 1024U;
                break;
            case PCI_DEV_6600LE_NV43_A:
            case PCI_DEV_6600LE_NV43_B:
                g_name = "NVIDIA GeForce 6600 LE (NV43 PCIe)";
                vram = 128U * 1024U * 1024U;
                break;
            default:
                if (dev->class_code == 0x03) {
                    g_name = "NVIDIA GeForce PCIe Graphics";
                    vram = 256U * 1024U * 1024U;
                    g_primary_7300gt = 1;
                    break;
                }
                continue;
        }

        uint8_t pcie = g_kapi->pci_find_capability(dev, PCI_CAP_ID_EXP);
        uint64_t bar_size = 0;
        if (dev->bar[1].valid && !dev->bar[1].is_io) {
            bar_size = dev->bar[1].size;
        }
        if (vram && bar_size < (64ULL * 1024ULL * 1024ULL)) {
            bar_size = vram;
        }
        if (!cur_lfb || sw <= 0 || sh <= 0) {
            continue;
        }

        if (dev->bar[0].valid && !dev->bar[0].is_io) {
            g_kapi->pci_enable_memory(dev);
            g_kapi->pci_enable_bus_master(dev);
        }
        g_pci = dev;
        g_bar0 = (dev->bar[0].valid && !dev->bar[0].is_io)
                     ? dev->bar[0].address
                     : 0;
        g_bar1 = (dev->bar[1].valid && !dev->bar[1].is_io)
                     ? dev->bar[1].address
                     : 0;
        g_bar1_size = bar_size;

        unsigned int hw_vram = nvidia_probe_hw_vram(g_bar0);
        g_vram_size = hw_vram ? hw_vram : (vram ? vram : (unsigned int)bar_size);
        g_active = 1;

        if (g_bar1 && nvidia_framebuffer_in_bar(
                          surf.framebuffer_phys, bar_size)) {
            g_status = "PCIe BAR1 WC LFB active";
        } else {
            g_status = "Linear WC aperture active";
        }
        klog_info("[NV  ] active %s id=%04x:%04x rev=%02x pcie=%02x BAR0=0x%llx BAR1=0x%llx lfb=0x%llx vram=%uMB",
                  g_name, dev->vendor, dev->device, dev->revision, pcie,
                  (unsigned long long)g_bar0, (unsigned long long)g_bar1,
                  (unsigned long long)surf.framebuffer_phys,
                  g_vram_size / (1024U * 1024U));
        return 1;
    }
    return 0;
}

static int nvidia_g7x_is_active(void) { return g_active; }
static int nvidia_g7x_is_2d_active(void) { return 0; }
static const char* nvidia_g7x_get_name(void) { return g_name; }
static const char* nvidia_g7x_get_status(void) { return g_status; }
static unsigned int nvidia_g7x_get_vram_size(void) { return g_vram_size; }

static void nvidia_g7x_present_rect(int x, int y, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void nvidia_g7x_fill_rect(int x, int y, int width, int height,
                                 unsigned int color) {
    unsigned char* cur_lfb = g_kapi->fb_get_lfb();
    if (!g_active || !cur_lfb) return;

    int sw = 0, sh = 0, sbpp = 0;
    g_kapi->fb_get_desktop_mode(&sw, &sh, &sbpp);
    pdv_fb_surface_t surf;
    g_kapi->fb_get_surface(&surf);

    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > sw) width = sw - x;
    if (y + height > sh) height = sh - y;
    if (width <= 0 || height <= 0) return;

    unsigned int bytes = nvidia_bytes_per_pixel(sbpp);
    for (int row = y; row < y + height; row++) {
        unsigned char* destination =
            cur_lfb + (size_t)row * surf.pitch + (size_t)x * bytes;
        if (bytes == 4) {
            unsigned int* pixel = (unsigned int*)destination;
            for (int column = 0; column < width; column++) {
                pixel[column] = color;
            }
        } else if (bytes == 2) {
            unsigned short* pixel = (unsigned short*)destination;
            for (int column = 0; column < width; column++) {
                pixel[column] = (unsigned short)color;
            }
        } else {
            for (int column = 0; column < width; column++) {
                unsigned char* pixel = destination + column * bytes;
                pixel[0] = (unsigned char)color;
                if (bytes > 1) pixel[1] = (unsigned char)(color >> 8);
                if (bytes > 2) pixel[2] = (unsigned char)(color >> 16);
            }
        }
    }
}

static void nvidia_g7x_screen_blt(int sx, int sy, int dx, int dy,
                                  int width, int height) {
    unsigned char* cur_lfb = g_kapi->fb_get_lfb();
    if (!g_active || !cur_lfb) return;

    int sw = 0, sh = 0, sbpp = 0;
    g_kapi->fb_get_desktop_mode(&sw, &sh, &sbpp);
    pdv_fb_surface_t surf;
    g_kapi->fb_get_surface(&surf);

    if (sx < 0) { width += sx; dx -= sx; sx = 0; }
    if (sy < 0) { height += sy; dy -= sy; sy = 0; }
    if (dx < 0) { width += dx; sx -= dx; dx = 0; }
    if (dy < 0) { height += dy; sy -= dy; dy = 0; }
    if (sx + width > sw) width = sw - sx;
    if (sy + height > sh) height = sh - sy;
    if (dx + width > sw) width = sw - dx;
    if (dy + height > sh) height = sh - dy;
    if (width <= 0 || height <= 0) return;

    unsigned int bytes = nvidia_bytes_per_pixel(sbpp);
    int row = height - 1;
    int row_end = -1;
    int row_step = -1;
    if (dy < sy) {
        row = 0;
        row_end = height;
        row_step = 1;
    }
    for (; row != row_end; row += row_step) {
        unsigned char* source =
            cur_lfb + (size_t)(sy + row) * surf.pitch +
            (size_t)sx * bytes;
        unsigned char* destination =
            cur_lfb + (size_t)(dy + row) * surf.pitch +
            (size_t)dx * bytes;
        int count = width * (int)bytes;
        for (int i = 0; i < count; i++) destination[i] = source[i];
    }
}

static void nvidia_g7x_sync(void) {
    g_kapi->fb_flush();
}

static const pdv_display_driver_t g_nvg7x_drv = {
    .name                 = "NVIDIA G7x PCIe",
    .priority             = 80,
    .requires_hw_mode_set = 0,
    .probe                = nvidia_g7x_probe,
    .start                = 0,
    .stop                 = 0,
    .get_name             = nvidia_g7x_get_name,
    .get_status           = nvidia_g7x_get_status,
    .is_active            = nvidia_g7x_is_active,
    .is_2d_active         = nvidia_g7x_is_2d_active,
    .get_vram_size        = nvidia_g7x_get_vram_size,
    .can_set_mode         = 0,
    .supports_bpp         = 0,
    .set_mode             = 0,
    .fill_rect            = nvidia_g7x_fill_rect,
    .screen_blt           = nvidia_g7x_screen_blt,
    .present_rect         = nvidia_g7x_present_rect,
    .sync                 = nvidia_g7x_sync,
    .has_hw_cursor        = 0,
    .set_cursor           = 0,
};

int pdv_driver_entry(const pdv_kernel_api_t* kapi) {
    if (!kapi || kapi->ddk_version < PDV_DDK_VERSION_1_0) return -1;
    g_kapi = kapi;
    return kapi->register_display_driver(&g_nvg7x_drv);
}
