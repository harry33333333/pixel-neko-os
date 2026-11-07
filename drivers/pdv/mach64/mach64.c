#include <pdv/ddk.h>

static const pdv_kernel_api_t* g_kapi;

#define klog_info(fmt, ...) g_kapi->log_printf(fmt "\n", ##__VA_ARGS__)

#define ATI_VENDOR_ID               0x1002
#define PCI_DEVICE_ATI_RAGE_GT      0x4754
#define PCI_DEVICE_ATI_RAGE_II      0x4755
#define PCI_DEVICE_ATI_RAGE_IIC_PCI 0x4756
#define PCI_DEVICE_ATI_RAGE_IIC_AGP 0x4757
#define PCI_DEVICE_ATI_RAGE_IIC_B   0x4759
#define PCI_DEVICE_ATI_RAGE_IIC_C   0x475A
#define PCI_DEVICE_ATI_RAGE_PRO_PCI 0x4742
#define PCI_DEVICE_ATI_RAGE_PRO_TRB 0x4744
#define PCI_DEVICE_ATI_RAGE_PRO_AGP 0x4749
#define PCI_DEVICE_ATI_RAGE_XL      0x474D
#define PCI_DEVICE_ATI_RAGE_XL_PCI  0x4752
#define PCI_DEVICE_ATI_MACH64_VT    0x5654
#define PCI_DEVICE_ATI_MACH64_VT4   0x5655

#define MACH64_CRTC_OFF_PITCH       0x0014
#define MACH64_CRTC_GEN_CNTL        0x001C
#define MACH64_MEM_CNTL             0x00B0
#define MACH64_GEN_TEST_CNTL        0x00D0
#define MACH64_CNFG_CHIP_ID         0x00E0
#define MACH64_DST_OFF_PITCH        0x0100
#define MACH64_DST_Y_X              0x010C
#define MACH64_DST_WIDTH_HEIGHT     0x0118
#define MACH64_SRC_OFF_PITCH        0x0180
#define MACH64_SRC_Y_X              0x018C
#define MACH64_SC_LEFT_RIGHT        0x02A8
#define MACH64_SC_TOP_BOTTOM        0x02B4
#define MACH64_DP_FRGD_CLR          0x02C4
#define MACH64_DP_CHAIN_MASK        0x02CC
#define MACH64_DP_PIX_WIDTH         0x02D0
#define MACH64_DP_MIX               0x02D4
#define MACH64_DP_SRC               0x02D8
#define MACH64_FIFO_STAT            0x0310
#define MACH64_GUI_TRAJ_CNTL        0x0330
#define MACH64_GUI_STAT             0x0338

static int g_ati_active = 0;
static const char* g_ati_chip_name = "None";
static unsigned int g_ati_vram_bytes = 4 * 1024 * 1024;
static volatile unsigned char* g_ati_regs = 0;
static volatile unsigned char* g_ati_lfb = 0;
static int g_cur_w = 800, g_cur_h = 600;

static inline unsigned int mach64_read32(unsigned int reg) {
    if (!g_ati_regs) return 0xFFFFFFFFU;
    return *(volatile unsigned int*)(g_ati_regs + reg);
}

static inline void mach64_write32(unsigned int reg, unsigned int val) {
    if (!g_ati_regs) return;
    *(volatile unsigned int*)(g_ati_regs + reg) = val;
}

static unsigned int mach64_pitch_units(int w) {
    return (((unsigned int)w + 7U) & ~7U) / 8U;
}

static unsigned int mach64_pixel_code(int bpp) {
    if (bpp <= 8) return 2;
    if (bpp == 15) return 3;
    if (bpp == 16) return 4;
    if (bpp == 24) return 5;
    return 6;
}

static volatile unsigned char* mach64_try_regs(uint64_t phys_base,
                                               unsigned short device) {
    if (!phys_base) return 0;
    volatile unsigned int* regs =
        (volatile unsigned int*)g_kapi->mmio_map_uc(phys_base, 0x1000);
    if (!regs) return 0;
    unsigned int chip_id = regs[MACH64_CNFG_CHIP_ID / 4];
    unsigned int h_total = regs[0];
    if (chip_id == 0xFFFFFFFFU || h_total == 0xFFFFFFFFU) return 0;
    if ((unsigned short)(chip_id & 0xFFFFU) == device) {
        return (volatile unsigned char*)regs;
    }
    return 0;
}

static volatile unsigned char* mach64_find_register_base(
    uint64_t bar0_phys, uint64_t bar2_phys, unsigned short device) {
    if (bar2_phys) {
        uint64_t base = (bar2_phys & ~0xFULL) + 0x400ULL;
        volatile unsigned char* regs = mach64_try_regs(base, device);
        if (regs) return regs;
    }

    volatile unsigned char* regs8 =
        mach64_try_regs(bar0_phys + 0x7FFC00ULL, device);
    if (regs8) return regs8;

    volatile unsigned char* regs4 =
        mach64_try_regs(bar0_phys + 0x3FFC00ULL, device);
    if (regs4) return regs4;

    return 0;
}

static inline void wait_fifo(int entries) {
    if (!g_ati_regs) return;
    int timeout = 100000;
    while (((mach64_read32(MACH64_FIFO_STAT) & 0xFFFFU) <=
            (unsigned int)(0x8000U >> entries)) &&
           --timeout) {
    }
}

static void ati_mach64_sync(void) {
    if (!g_ati_regs || !g_ati_active) return;
    int timeout = 200000;
    while ((mach64_read32(MACH64_GUI_STAT) & 1U) && --timeout) {
    }
}

static int ati_mach64_probe_hw(void) {
    g_ati_active = 0;
    g_ati_regs = 0;
    g_ati_lfb = 0;

    for (int index = 0; index < g_kapi->pci_get_device_count(); index++) {
        const pdv_pci_device_t* pci = g_kapi->pci_get_device(index);
        if (!pci || pci->vendor != ATI_VENDOR_ID) continue;

        unsigned short device = pci->device;
        const char* name = 0;
        switch (device) {
            case PCI_DEVICE_ATI_RAGE_GT:      name = "ATI 3D Rage (Mach64 GT)"; break;
            case PCI_DEVICE_ATI_RAGE_II:      name = "ATI 3D Rage II+DVD (Mach64 GT-B)"; break;
            case PCI_DEVICE_ATI_RAGE_IIC_PCI: name = "ATI 3D Rage IIc (PCI)"; break;
            case PCI_DEVICE_ATI_RAGE_IIC_AGP: name = "ATI 3D Rage IIc (AGP)"; break;
            case PCI_DEVICE_ATI_RAGE_IIC_B:
            case PCI_DEVICE_ATI_RAGE_IIC_C:   name = "ATI 3D Rage IIC"; break;
            case PCI_DEVICE_ATI_RAGE_PRO_PCI:
            case PCI_DEVICE_ATI_RAGE_PRO_TRB:
            case PCI_DEVICE_ATI_RAGE_PRO_AGP: name = "ATI 3D Rage Pro"; break;
            case PCI_DEVICE_ATI_RAGE_XL:
            case PCI_DEVICE_ATI_RAGE_XL_PCI:  name = "ATI Rage XL"; break;
            case PCI_DEVICE_ATI_MACH64_VT:
            case PCI_DEVICE_ATI_MACH64_VT4:   name = "ATI Mach64 VT"; break;
            default: break;
        }
        if (!name) continue;

        uint64_t bar0_phys = 0;
        uint64_t bar2_phys = 0;
        if (pci->bar[0].valid && !pci->bar[0].is_io) {
            bar0_phys = pci->bar[0].address;
        }
        if (pci->bar[2].valid && !pci->bar[2].is_io) {
            bar2_phys = pci->bar[2].address;
        }
        if (!bar0_phys) continue;

        g_kapi->pci_enable_io(pci);
        g_kapi->pci_enable_memory(pci);
        g_kapi->pci_enable_bus_master(pci);

        g_ati_chip_name = name;
        g_ati_lfb = (volatile unsigned char*)g_kapi->mmio_map_wc(
            bar0_phys, 8 * 1024 * 1024);
        g_ati_regs = mach64_find_register_base(bar0_phys, bar2_phys, device);

        if (g_ati_lfb) {
            volatile unsigned int* p0 = (volatile unsigned int*)(g_ati_lfb);
            volatile unsigned int* p1 =
                (volatile unsigned int*)(g_ati_lfb + 1 * 1024 * 1024);
            volatile unsigned int* p2 =
                (volatile unsigned int*)(g_ati_lfb + 2 * 1024 * 1024);
            volatile unsigned int* p4 =
                (volatile unsigned int*)(g_ati_lfb + 4 * 1024 * 1024);

            unsigned int save0 = *p0;
            unsigned int save1 = *p1;
            unsigned int save2 = *p2;
            unsigned int save4 = *p4;

            *p0 = 0xA55A1234U;
            *p1 = 0x5AA54321U;
            if (*p1 != 0x5AA54321U || *p0 == 0x5AA54321U) {
                g_ati_vram_bytes = 1 * 1024 * 1024;
            } else {
                *p2 = 0x5AA5789AU;
                if (*p2 != 0x5AA5789AU || *p0 == 0x5AA5789AU) {
                    g_ati_vram_bytes = 2 * 1024 * 1024;
                } else {
                    *p4 = 0x5AA5BCDEU;
                    if (*p4 != 0x5AA5BCDEU || *p0 == 0x5AA5BCDEU) {
                        g_ati_vram_bytes = 4 * 1024 * 1024;
                    } else {
                        g_ati_vram_bytes = 8 * 1024 * 1024;
                    }
                }
            }

            *p0 = save0;
            *p1 = save1;
            *p2 = save2;
            *p4 = save4;
        }

        if (g_ati_regs) {
            mach64_write32(MACH64_GEN_TEST_CNTL,
                           mach64_read32(MACH64_GEN_TEST_CNTL) | (1U << 8));
            ati_mach64_sync();
            mach64_write32(MACH64_GEN_TEST_CNTL,
                           mach64_read32(MACH64_GEN_TEST_CNTL) & ~(1U << 8));
        }

        g_ati_active = 1;
        klog_info("[ATI ] active %s BAR0=0x%llx vram=%uKB regs=%s",
                  g_ati_chip_name, (unsigned long long)bar0_phys,
                  g_ati_vram_bytes / 1024U,
                  g_ati_regs ? "mapped" : "lfb-only");
        return 1;
    }
    return 0;
}

static int ati_mach64_probe_entry(void) {
    if (!ati_mach64_probe_hw()) return 0;
    g_ati_active = 0;
    g_kapi->fb_apply_ati_cap();
    return 0; /* Keep safe linear WC framebuffer active with 1024x768x16 cap */
}

static int ati_mach64_is_active(void) { return g_ati_active; }
static const char* ati_mach64_get_chip_name(void) { return g_ati_chip_name; }
static const char* ati_mach64_get_status(void) { return "GUI accelerator active"; }
static unsigned int ati_mach64_get_vram_size(void) { return g_ati_vram_bytes; }

static int ati_mach64_set_mode_bpp(int w, int h, int bpp) {
    if (!g_ati_active) return 0;
    g_cur_w = w;
    g_cur_h = h;
    if (!g_ati_regs) return 1;

    ati_mach64_sync();
    unsigned int pitch_units = mach64_pitch_units(w);
    mach64_write32(MACH64_DST_OFF_PITCH, (pitch_units & 0x3FFU) << 22);
    mach64_write32(MACH64_SRC_OFF_PITCH, (pitch_units & 0x3FFU) << 22);

    unsigned int code = mach64_pixel_code(bpp);
    mach64_write32(MACH64_DP_PIX_WIDTH,
                   code | (code << 8) | (code << 16) | (1U << 24));

    unsigned int chain_mask = 0x8080U;
    if (bpp == 15) chain_mask = 0x4210U;
    else if (bpp == 16) chain_mask = 0x8410U;
    mach64_write32(MACH64_DP_CHAIN_MASK, chain_mask);

    mach64_write32(MACH64_SC_LEFT_RIGHT,
                   ((unsigned int)(w - 1) & 0x3FFFU) << 16);
    mach64_write32(MACH64_SC_TOP_BOTTOM,
                   ((unsigned int)(h - 1) & 0x7FFFU) << 16);
    mach64_write32(MACH64_DP_MIX, 0x00070007U);
    return 1;
}

static void ati_mach64_fill_rect(int x, int y, int w, int h, unsigned int color) {
    if (!g_ati_active || !g_ati_regs) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_cur_w) w = g_cur_w - x;
    if (y + h > g_cur_h) h = g_cur_h - y;
    if (w <= 0 || h <= 0) return;

    wait_fifo(5);
    mach64_write32(MACH64_DP_FRGD_CLR, color);
    mach64_write32(MACH64_DP_SRC, 0x00000100U);
    mach64_write32(MACH64_DP_MIX, 0x00070007U);
    mach64_write32(MACH64_DST_Y_X,
                   ((unsigned int)x << 16) | ((unsigned int)y & 0xFFFFU));
    mach64_write32(MACH64_DST_WIDTH_HEIGHT,
                   ((unsigned int)w << 16) | ((unsigned int)h & 0xFFFFU));
}

static void ati_mach64_screen_blt(int sx, int sy, int dx, int dy, int w, int h) {
    if (!g_ati_active || !g_ati_regs) return;
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (sx + w > g_cur_w) w = g_cur_w - sx;
    if (sy + h > g_cur_h) h = g_cur_h - sy;
    if (dx + w > g_cur_w) w = g_cur_w - dx;
    if (dy + h > g_cur_h) h = g_cur_h - dy;
    if (w <= 0 || h <= 0) return;

    unsigned int traj = 0x0003U;
    if (dy > sy) {
        traj &= ~0x0002U;
        sy += h - 1;
        dy += h - 1;
    }
    if (dx > sx) {
        traj &= ~0x0001U;
        sx += w - 1;
        dx += w - 1;
    }

    wait_fifo(5);
    mach64_write32(MACH64_GUI_TRAJ_CNTL, traj);
    mach64_write32(MACH64_DP_SRC, 0x00000300U);
    mach64_write32(MACH64_DP_MIX, 0x00070007U);
    mach64_write32(MACH64_SRC_Y_X,
                   ((unsigned int)sx << 16) | ((unsigned int)sy & 0xFFFFU));
    mach64_write32(MACH64_DST_Y_X,
                   ((unsigned int)dx << 16) | ((unsigned int)dy & 0xFFFFU));
    mach64_write32(MACH64_DST_WIDTH_HEIGHT,
                   ((unsigned int)w << 16) | ((unsigned int)h & 0xFFFFU));
}

static const pdv_display_driver_t g_mach64_drv = {
    .name                 = "ATI Mach64",
    .priority             = 50,
    .requires_hw_mode_set = 0,
    .probe                = ati_mach64_probe_entry,
    .start                = 0,
    .stop                 = 0,
    .get_name             = ati_mach64_get_chip_name,
    .get_status           = ati_mach64_get_status,
    .is_active            = ati_mach64_is_active,
    .is_2d_active         = ati_mach64_is_active,
    .get_vram_size        = ati_mach64_get_vram_size,
    .can_set_mode         = 0,
    .supports_bpp         = 0,
    .set_mode             = ati_mach64_set_mode_bpp,
    .fill_rect            = ati_mach64_fill_rect,
    .screen_blt           = ati_mach64_screen_blt,
    .present_rect         = 0,
    .sync                 = ati_mach64_sync,
    .has_hw_cursor        = 0,
    .set_cursor           = 0,
};

int pdv_driver_entry(const pdv_kernel_api_t* kapi) {
    if (!kapi || kapi->ddk_version < PDV_DDK_VERSION_1_0) return -1;
    g_kapi = kapi;
    return kapi->register_display_driver(&g_mach64_drv);
}
