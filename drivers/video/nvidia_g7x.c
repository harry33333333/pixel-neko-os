#include "nvidia_g7x.h"
#include "boot_framebuffer.h"
#include "log.h"
#include "mmio.h"
#include "pci.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

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

extern int screen_w;
extern int screen_h;
extern int screen_bpp;
extern int screen_pitch;
extern unsigned char* lfb;

static const pci_device_t* g_pci;
static int g_active;
static unsigned int g_vram_size;
static uint64_t g_bar0;
static uint64_t g_bar1;
static uint64_t g_bar1_size;
static int g_primary_7300gt;
static const char* g_name = "NVIDIA PCIe";
static const char* g_status = "Not detected";

static unsigned int nvidia_bytes_per_pixel(void) {
    if (screen_bpp <= 8) return 1;
    if (screen_bpp <= 16) return 2;
    if (screen_bpp <= 24) return 3;
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
        (volatile uint8_t*)mmio_map_phys(bar0_phys + 0x100000ULL, 0x1000);
    if (!pfb) return 0;
    uint32_t cstatus = mmio_read32(pfb + 0x20C);
    if (cstatus == 0 || cstatus == 0xFFFFFFFFU) return 0;
    uint32_t bytes = cstatus & 0xFFF00000U;
    if (bytes >= (16U * 1024U * 1024U) && bytes <= (1024U * 1024U * 1024U)) {
        return bytes;
    }
    return 0;
}

int nvidia_g7x_probe(void) {
    g_active = 0;
    g_pci = 0;
    g_vram_size = 0;
    g_bar0 = 0;
    g_bar1 = 0;
    g_bar1_size = 0;
    g_primary_7300gt = 0;
    g_status = "NVIDIA G7x PCIe device not found";

    pci_scan_all();
    for (int i = 0; i < pci_get_device_count(); i++) {
        const pci_device_t* dev = pci_get_device(i);
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

        uint8_t pcie = pci_find_capability(dev, PCI_CAP_ID_EXP);
        uint64_t bar_size = 0;
        if (dev->bar[1].valid && !dev->bar[1].is_io) {
            bar_size = dev->bar[1].size;
        }
        if (vram && bar_size < (64ULL * 1024ULL * 1024ULL)) {
            bar_size = vram;
        }
        if (!lfb || screen_w <= 0 || screen_h <= 0) {
            continue;
        }

        if (dev->bar[0].valid && !dev->bar[0].is_io) {
            pci_enable_memory(dev);
            pci_enable_bus_master(dev);
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
                          g_main_surface.framebuffer_phys, bar_size)) {
            g_status = "PCIe BAR1 WC LFB active";
        } else {
            g_status = "Linear WC aperture active";
        }
        klog_info("[NV  ] active %s id=%04x:%04x rev=%02x pcie=%02x BAR0=0x%llx BAR1=0x%llx lfb=0x%llx vram=%uMB",
                  g_name, dev->vendor, dev->device, dev->revision, pcie,
                  (unsigned long long)g_bar0, (unsigned long long)g_bar1,
                  (unsigned long long)g_main_surface.framebuffer_phys,
                  g_vram_size / (1024U * 1024U));
        return 1;
    }
    return 0;
}

int nvidia_g7x_is_active(void) {
    return g_active;
}

const char* nvidia_g7x_get_name(void) {
    return g_name;
}

const char* nvidia_g7x_get_status(void) {
    return g_status;
}

unsigned int nvidia_g7x_get_vram_size(void) {
    return g_vram_size;
}

int nvidia_g7x_supports_1280x1024(void) {
    return g_active && g_primary_7300gt;
}

void nvidia_g7x_present_rect(int x, int y, int width, int height) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

void nvidia_g7x_fill_rect(int x, int y, int width, int height,
                          unsigned int color) {
    if (!g_active || !lfb) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > screen_w) width = screen_w - x;
    if (y + height > screen_h) height = screen_h - y;
    if (width <= 0 || height <= 0) return;

    unsigned int bytes = nvidia_bytes_per_pixel();
    for (int row = y; row < y + height; row++) {
        unsigned char* destination = lfb + row * screen_pitch + x * bytes;
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

void nvidia_g7x_screen_blt(int sx, int sy, int dx, int dy,
                           int width, int height) {
    if (!g_active || !lfb) return;
    if (sx < 0) { width += sx; dx -= sx; sx = 0; }
    if (sy < 0) { height += sy; dy -= sy; sy = 0; }
    if (dx < 0) { width += dx; sx -= dx; dx = 0; }
    if (dy < 0) { height += dy; sy -= dy; dy = 0; }
    if (sx + width > screen_w) width = screen_w - sx;
    if (sy + height > screen_h) height = screen_h - sy;
    if (dx + width > screen_w) width = screen_w - dx;
    if (dy + height > screen_h) height = screen_h - dy;
    if (width <= 0 || height <= 0) return;

    unsigned int bytes = nvidia_bytes_per_pixel();
    int row = height - 1;
    int row_end = -1;
    int row_step = -1;
    if (dy < sy) {
        row = 0;
        row_end = height;
        row_step = 1;
    }
    for (; row != row_end; row += row_step) {
        unsigned char* source = lfb + (sy + row) * screen_pitch + sx * bytes;
        unsigned char* destination =
            lfb + (dy + row) * screen_pitch + dx * bytes;
        int count = width * (int)bytes;
        for (int i = 0; i < count; i++) destination[i] = source[i];
    }
}

void nvidia_g7x_sync(void) {
    boot_framebuffer_flush();
}
