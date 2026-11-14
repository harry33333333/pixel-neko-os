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
#define MACH64_CNFG_CNTL            0x00DC
#define MACH64_CNFG_CHIP_ID         0x00E0
#define MACH64_CNFG_STAT0           0x00E4
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

#define MACH64_MAX_MODES            24

static int g_ati_active = 0;
static const char* g_ati_chip_name = "None";
static const char* g_ati_status = "Not detected";
static unsigned int g_ati_vram_bytes = 2U * 1024U * 1024U;
static volatile unsigned char* g_ati_regs = 0;
static unsigned short g_ati_io_base = 0;
static int g_cur_w = 800, g_cur_h = 600;
static int g_boot_w = 1024, g_boot_h = 768;
static pdv_display_mode_t g_ati_modes[MACH64_MAX_MODES];
static int g_ati_mode_count = 0;

static inline unsigned int mach64_read32(unsigned int reg) {
    if (g_ati_regs) {
        return pdv_mmio_read32((volatile const void*)(g_ati_regs + reg));
    }
    if (g_ati_io_base && reg < 0x100U) {
        return pdv_inl((uint16_t)(g_ati_io_base + reg));
    }
    return 0xFFFFFFFFU;
}

static unsigned int mach64_decode_mem_cntl_bytes(unsigned int mem_cntl) {
    static const unsigned int k_kb[8] = {
        512U, 1024U, 2048U, 4096U, 6144U, 8192U, 12288U, 16384U
    };
    return k_kb[mem_cntl & 0x07U] * 1024U;
}

static volatile unsigned char* mach64_try_regs(uint64_t phys_base,
                                               unsigned short device) {
    if (!phys_base) return 0;
    volatile unsigned char* regs =
        (volatile unsigned char*)g_kapi->mmio_map_uc(phys_base, 0x1000);
    if (!regs) return 0;
    unsigned int chip_id = pdv_mmio_read32((volatile const void*)(regs + MACH64_CNFG_CHIP_ID));
    unsigned int h_total = pdv_mmio_read32((volatile const void*)regs);
    if (chip_id == 0xFFFFFFFFU || h_total == 0xFFFFFFFFU) return 0;
    if ((unsigned short)(chip_id & 0xFFFFU) == device) {
        return regs;
    }
    return 0;
}

static volatile unsigned char* mach64_find_register_base(
    const pdv_pci_device_t* pci, uint64_t bar0_phys, uint64_t bar2_phys,
    unsigned short device) {
    if (bar2_phys && pci->bar[2].size >= 0x1000ULL) {
        uint64_t b2 = bar2_phys & ~0xFULL;
        volatile unsigned char* regs = mach64_try_regs(b2 + 0x400ULL, device);
        if (regs) return regs;
        regs = mach64_try_regs(b2, device);
        if (regs) return regs;
    }

    if (pci->bar[0].size >= 0x800000ULL) {
        volatile unsigned char* regs8 =
            mach64_try_regs(bar0_phys + 0x7FFC00ULL, device);
        if (regs8) return regs8;
    }

    if (pci->bar[0].size >= 0x400000ULL) {
        volatile unsigned char* regs4 =
            mach64_try_regs(bar0_phys + 0x3FFC00ULL, device);
        if (regs4) return regs4;
    }

    return 0;
}

static int mach64_uc_probe_offset(uint64_t bar0_phys,
                                  volatile unsigned int* base_ptr,
                                  volatile unsigned int* dummy_ptr,
                                  uint64_t offset) {
    volatile unsigned char* page =
        (volatile unsigned char*)g_kapi->mmio_map_uc(bar0_phys + offset, 0x1000);
    if (!page) return 0;
    volatile unsigned int* p = (volatile unsigned int*)page;
    volatile unsigned int* p_chk = (volatile unsigned int*)(page + 0x40);

    unsigned int save_base = *base_ptr;
    unsigned int save_p = *p;
    unsigned int save_chk = *p_chk;
    unsigned int save_dummy = *dummy_ptr;

    unsigned int pat = 0xA55A3C00U ^ (unsigned int)(offset >> 12);
    unsigned int pat_inv = ~pat;
    unsigned int pat_chk = 0x5A3C9600U ^ (unsigned int)(offset >> 12);

    *p = pat;
    *p_chk = pat_chk;
    *dummy_ptr = 0x12345678U;
    (void)*dummy_ptr;

    unsigned int rb = *p;
    unsigned int rb_chk = *p_chk;
    unsigned int rb_base = *base_ptr;
    unsigned int pat_swapped = __builtin_bswap32(pat);
    int ok = (rb == pat && rb_chk == pat_chk &&
              rb_base != pat && rb_base != pat_swapped && rb_base != pat_chk);

    if (ok) {
        *p = pat_inv;
        *dummy_ptr = 0x87654321U;
        (void)*dummy_ptr;
        if (*p != pat_inv || *base_ptr == pat_inv ||
            *base_ptr == __builtin_bswap32(pat_inv)) {
            ok = 0;
        }
    }

    *dummy_ptr = save_dummy;
    *p_chk = save_chk;
    *p = save_p;
    *base_ptr = save_base;
    return ok;
}

static unsigned int mach64_detect_vram_bytes(const pdv_pci_device_t* pci,
                                             uint64_t bar0_phys,
                                             int has_regs) {
    uint64_t aper_limit = pci->bar[0].size;
    if (aper_limit == 0 || aper_limit > 16ULL * 1024ULL * 1024ULL) {
        aper_limit = 8ULL * 1024ULL * 1024ULL;
    }

    unsigned int reg_vram = 0;
    if (has_regs) {
        unsigned int mem_cntl = mach64_read32(MACH64_MEM_CNTL);
        unsigned int cnfg_cntl = mach64_read32(MACH64_CNFG_CNTL);
        if (mem_cntl != 0xFFFFFFFFU) {
            reg_vram = mach64_decode_mem_cntl_bytes(mem_cntl);
        }
        if (cnfg_cntl != 0xFFFFFFFFU) {
            unsigned int ap = cnfg_cntl & 0x03U;
            if (ap == 1U && aper_limit > 4ULL * 1024ULL * 1024ULL) {
                /* 4MB aperture: upper 4MB is Big-Endian mirror; never probe it! */
                aper_limit = 4ULL * 1024ULL * 1024ULL;
            } else if (ap == 2U && aper_limit > 8ULL * 1024ULL * 1024ULL) {
                aper_limit = 8ULL * 1024ULL * 1024ULL;
            }
        }
    } else if (aper_limit > 4ULL * 1024ULL * 1024ULL) {
        /* Without CNFG_CNTL we cannot verify 8MB vs 4MB BE mirror, so avoid >=4MB alias */
        aper_limit = 4ULL * 1024ULL * 1024ULL;
    }

    volatile unsigned char* base_page =
        (volatile unsigned char*)g_kapi->mmio_map_uc(bar0_phys + 0x1000ULL, 0x1000);
    if (!base_page) {
        return reg_vram ? reg_vram : (2U * 1024U * 1024U);
    }

    volatile unsigned int* base_ptr = (volatile unsigned int*)base_page;
    volatile unsigned int* dummy_ptr = (volatile unsigned int*)(base_page + 0x100);

    unsigned int probed = 1U * 1024U * 1024U;
    if (aper_limit >= 0x104000ULL &&
        mach64_uc_probe_offset(bar0_phys, base_ptr, dummy_ptr, 0x101000ULL)) {
        probed = 2U * 1024U * 1024U;
        if (aper_limit >= 0x204000ULL &&
            mach64_uc_probe_offset(bar0_phys, base_ptr, dummy_ptr, 0x201000ULL) &&
            aper_limit >= 0x304000ULL &&
            mach64_uc_probe_offset(bar0_phys, base_ptr, dummy_ptr, 0x301000ULL)) {
            probed = 4U * 1024U * 1024U;
            if (aper_limit >= 0x404000ULL &&
                mach64_uc_probe_offset(bar0_phys, base_ptr, dummy_ptr, 0x401000ULL)) {
                probed = 6U * 1024U * 1024U;
                if (aper_limit >= 0x604000ULL &&
                    mach64_uc_probe_offset(bar0_phys, base_ptr, dummy_ptr, 0x601000ULL)) {
                    probed = 8U * 1024U * 1024U;
                }
            }
        }
    } else if (aper_limit >= 0x084000ULL &&
               !mach64_uc_probe_offset(bar0_phys, base_ptr, dummy_ptr, 0x081000ULL)) {
        probed = 512U * 1024U;
    }

    if (reg_vram > 0) {
        if (reg_vram <= (unsigned int)aper_limit) {
            return (probed < reg_vram) ? probed : reg_vram;
        }
        /* Card has >=8MB in a 4MB aperture: respect verified 4MB probe if >=4MB */
        if (probed >= 4U * 1024U * 1024U) {
            return reg_vram;
        }
        return probed;
    }
    return probed;
}

static void ati_mach64_sync(void) {
    if (!g_ati_regs || !g_ati_active) return;
    int timeout = 200000;
    while ((mach64_read32(MACH64_GUI_STAT) & 1U) && --timeout) {
    }
}

static int ati_mach64_supports_bpp(int w, int h, int bpp) {
    if (!g_ati_active) return 0;
    if (w < PDV_DISPLAY_MIN_WIDTH || h < PDV_DISPLAY_MIN_HEIGHT) return 0;
    if (w > g_boot_w || h > g_boot_h) return 0;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    uint64_t bytes = (uint64_t)(unsigned int)w * (uint64_t)(unsigned int)h *
                     (uint64_t)(unsigned int)(bpp / 8);
    return (bytes <= (uint64_t)g_ati_vram_bytes) ? 1 : 0;
}

static void ati_mach64_build_modes(void) {
    static const int k_preset_w[] = {
        640, 800, 1024, 1152, 1280, 1280, 1280, 1280,
        1366, 1400, 1440, 1600, 1600, 1680, 1920, 1920
    };
    static const int k_preset_h[] = {
        480, 600, 768, 864, 720, 800, 960, 1024,
        768, 1050, 900, 900, 1200, 1050, 1080, 1200
    };
    static const int k_bpps[4] = { 8, 16, 24, 32 };
    int cnt = 0;
    int presets = (int)(sizeof(k_preset_w) / sizeof(k_preset_w[0]));

    for (int i = 0; i < presets && cnt < MACH64_MAX_MODES; i++) {
        int w = k_preset_w[i];
        int h = k_preset_h[i];
        uint8_t mask = 0;
        for (int b = 0; b < 4; b++) {
            if (ati_mach64_supports_bpp(w, h, k_bpps[b])) {
                mask |= pdv_bpp_to_mask(k_bpps[b]);
            }
        }
        if (mask != 0) {
            g_ati_modes[cnt].width = (uint16_t)w;
            g_ati_modes[cnt].height = (uint16_t)h;
            g_ati_modes[cnt].bpp_mask = mask;
            g_ati_modes[cnt].preferred = (w == g_boot_w && h == g_boot_h) ? 1U : 0U;
            g_ati_modes[cnt].reserved = 0U;
            cnt++;
        }
    }

    if (g_boot_w >= PDV_DISPLAY_MIN_WIDTH && g_boot_h >= PDV_DISPLAY_MIN_HEIGHT &&
        cnt < MACH64_MAX_MODES) {
        int dup = 0;
        for (int j = 0; j < cnt; j++) {
            if ((int)g_ati_modes[j].width == g_boot_w &&
                (int)g_ati_modes[j].height == g_boot_h) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            uint8_t mask = 0;
            for (int b = 0; b < 4; b++) {
                if (ati_mach64_supports_bpp(g_boot_w, g_boot_h, k_bpps[b])) {
                    mask |= pdv_bpp_to_mask(k_bpps[b]);
                }
            }
            if (mask != 0) {
                g_ati_modes[cnt].width = (uint16_t)g_boot_w;
                g_ati_modes[cnt].height = (uint16_t)g_boot_h;
                g_ati_modes[cnt].bpp_mask = mask;
                g_ati_modes[cnt].preferred = 1U;
                g_ati_modes[cnt].reserved = 0U;
                cnt++;
            }
        }
    }
    g_ati_mode_count = cnt;
}

static int ati_mach64_probe_hw(void) {
    g_ati_active = 0;
    g_ati_regs = 0;
    g_ati_io_base = 0;

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

        if (pci->bar[1].valid && pci->bar[1].is_io && pci->bar[1].size >= 256ULL) {
            g_ati_io_base = (unsigned short)pci->bar[1].address;
        }

        g_ati_chip_name = name;
        g_ati_regs = mach64_find_register_base(pci, bar0_phys, bar2_phys, device);
        int has_regs = (g_ati_regs != 0) ? 1 : 0;
        if (!has_regs && g_ati_io_base) {
            unsigned int io_chip = pdv_inl((uint16_t)(g_ati_io_base + MACH64_CNFG_CHIP_ID));
            if (io_chip != 0xFFFFFFFFU && (unsigned short)(io_chip & 0xFFFFU) == device) {
                has_regs = 1;
            }
        }

        g_ati_vram_bytes = mach64_detect_vram_bytes(pci, bar0_phys, has_regs);

        pdv_fb_surface_t surf;
        g_kapi->fb_get_surface(&surf);
        g_boot_w = (surf.width >= PDV_DISPLAY_MIN_WIDTH) ? (int)surf.width : 1024;
        g_boot_h = (surf.height >= PDV_DISPLAY_MIN_HEIGHT) ? (int)surf.height : 768;
        g_cur_w = g_boot_w;
        g_cur_h = g_boot_h;

        g_ati_active = 1;
        ati_mach64_build_modes();
        g_ati_status = (g_ati_vram_bytes <= 2U * 1024U * 1024U)
                   ? "Active (LFB, <=2MB VRAM limit)"
                   : "Active (LFB, VRAM verified)";
        klog_info("[ATI ] active %s BAR0=0x%llx vram=%uKB regs=%s modes=%d",
                  g_ati_chip_name, (unsigned long long)bar0_phys,
                  g_ati_vram_bytes / 1024U,
                  has_regs ? "verified" : "probe-only",
                  g_ati_mode_count);
        return 1;
    }
    return 0;
}

static int ati_mach64_probe_entry(void) {
    return ati_mach64_probe_hw();
}

static int ati_mach64_is_active(void) { return g_ati_active; }
static int ati_mach64_is_2d_active(void) { return 0; }
static const char* ati_mach64_get_chip_name(void) { return g_ati_chip_name; }
static const char* ati_mach64_get_status(void) { return g_ati_status; }
static unsigned int ati_mach64_get_vram_size(void) { return g_ati_vram_bytes; }

static int ati_mach64_get_mode_count(void) {
    return g_ati_active ? g_ati_mode_count : 0;
}

static int ati_mach64_get_mode(int index, pdv_display_mode_t* out_mode) {
    if (!g_ati_active || !out_mode || index < 0 || index >= g_ati_mode_count) {
        return 0;
    }
    *out_mode = g_ati_modes[index];
    return 1;
}

static int ati_mach64_get_max_width(void) {
    return g_ati_active ? g_boot_w : 0;
}

static int ati_mach64_get_max_height(void) {
    return g_ati_active ? g_boot_h : 0;
}

static int ati_mach64_set_mode_bpp(int w, int h, int bpp) {
    if (!ati_mach64_supports_bpp(w, h, bpp)) return 0;
    g_cur_w = w;
    g_cur_h = h;
    return 1;
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
    .is_2d_active         = ati_mach64_is_2d_active,
    .get_vram_size        = ati_mach64_get_vram_size,
    .can_set_mode         = ati_mach64_supports_bpp,
    .supports_bpp         = ati_mach64_supports_bpp,
    .set_mode             = ati_mach64_set_mode_bpp,
    .fill_rect            = 0,
    .screen_blt           = 0,
    .present_rect         = 0,
    .sync                 = ati_mach64_sync,
    .has_hw_cursor        = 0,
    .set_cursor           = 0,
    .get_mode_count       = ati_mach64_get_mode_count,
    .get_mode             = ati_mach64_get_mode,
    .get_max_width        = ati_mach64_get_max_width,
    .get_max_height       = ati_mach64_get_max_height,
};

int pdv_driver_entry(const pdv_kernel_api_t* kapi) {
    if (!kapi || kapi->ddk_version < PDV_DDK_VERSION_1_0) return -1;
    g_kapi = kapi;
    return kapi->register_display_driver(&g_mach64_drv);
}
