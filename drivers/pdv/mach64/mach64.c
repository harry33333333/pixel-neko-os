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

#define MACH64_CRTC_H_TOTAL_DISP    0x0000
#define MACH64_CRTC_H_SYNC_STRT_WID 0x0004
#define MACH64_CRTC_V_TOTAL_DISP    0x0008
#define MACH64_CRTC_V_SYNC_STRT_WID 0x000C
#define MACH64_CRTC_OFF_PITCH       0x0014
#define MACH64_CRTC_INT_CNTL        0x0018
#define MACH64_CRTC_GEN_CNTL        0x001C
#define MACH64_MEM_CNTL             0x00B0
#define MACH64_CLOCK_CNTL           0x0090
#define MACH64_DAC_CNTL             0x00C4
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

#define MACH64_MAX_MODES            48

#define MACH64_CRTC_H_SYNC_POL      (1U << 21)
#define MACH64_CRTC_V_SYNC_POL      (1U << 21)
#define MACH64_CRTC_EXT_DISP_EN     (1U << 24)
#define MACH64_CRTC_EN              (1U << 25)
#define MACH64_CRTC_VGA_LINEAR      (1U << 27)
#define MACH64_CRTC_CNT_EN          (1U << 30)
#define MACH64_CRTC_VBLANK          (1U << 0)
#define MACH64_DAC_8BIT_EN          (1U << 8)
#define MACH64_DAC_RAMDAC_SELECT    ((1U << 4) | (1U << 5) | (1U << 14))
#define MACH64_GEN_GUI_EN           0x00000100U

#define MACH64_PLL_WR_EN            0x02U
#define MACH64_PLL_ADDR_MASK        0xFCU
#define MACH64_PLL_VCLK_CNTL        0x05U
#define MACH64_PLL_VCLK_RESET       0x04U
#define MACH64_PLL_VCLK_POST_DIV    0x06U
#define MACH64_PLL_VCLK_FB_DIV0     0x07U
#define MACH64_PLL_XCLK_CNTL        0x0BU
#define MACH64_PLL_VCLK_CLOCK       3U
#define MACH64_CLOCK_CNTL_PROGRAMMED 0x73U

static int g_ati_active = 0;
static const char* g_ati_chip_name = "None";
static const char* g_ati_status = "Not detected";
static unsigned int g_ati_vram_bytes = 2U * 1024U * 1024U;
static volatile unsigned char* g_ati_regs = 0;
static unsigned short g_ati_io_base = 0;
static uint64_t g_ati_bar0_phys;
static int g_cur_w = 800, g_cur_h = 600;
static int g_cur_bpp = 16;
static int g_cur_pitch = 1600;
static int g_boot_w = 1024, g_boot_h = 768;
static int g_boot_bpp = 16;
static int g_ati_hw_modeset_capable;
static const char* g_ati_hw_modeset_last_status = "Mach64 CRTC/PLL unavailable";
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

static void mach64_write32(unsigned int reg, unsigned int value) {
    if (g_ati_regs) {
        pdv_mmio_write32((volatile void*)(g_ati_regs + reg), value);
    } else if (g_ati_io_base && reg < 0x100U) {
        pdv_outl((uint16_t)(g_ati_io_base + reg), value);
    }
}

static unsigned char mach64_read8(unsigned int reg) {
    if (g_ati_regs) {
        return pdv_mmio_read8((volatile const void*)(g_ati_regs + reg));
    }
    if (g_ati_io_base && reg < 0x100U) {
        return pdv_inb((uint16_t)(g_ati_io_base + reg));
    }
    return 0xFFU;
}

static void mach64_write8(unsigned int reg, unsigned char value) {
    if (g_ati_regs) {
        pdv_mmio_write8((volatile void*)(g_ati_regs + reg), value);
    } else if (g_ati_io_base && reg < 0x100U) {
        pdv_outb((uint16_t)(g_ati_io_base + reg), value);
    }
}

static unsigned char mach64_pll_read(unsigned char index) {
    unsigned char control = mach64_read8(MACH64_CLOCK_CNTL + 1U);
    control &= (unsigned char)~(MACH64_PLL_WR_EN | MACH64_PLL_ADDR_MASK);
    control |= (unsigned char)((index & 0x3FU) << 2);
    mach64_write8(MACH64_CLOCK_CNTL + 1U, control);
    return mach64_read8(MACH64_CLOCK_CNTL + 2U);
}

static void mach64_pll_write(unsigned char index, unsigned char value) {
    unsigned char control = mach64_read8(MACH64_CLOCK_CNTL + 1U);
    control &= (unsigned char)~(MACH64_PLL_WR_EN | MACH64_PLL_ADDR_MASK);
    control |= (unsigned char)((index & 0x3FU) << 2);
    control |= MACH64_PLL_WR_EN;
    mach64_write8(MACH64_CLOCK_CNTL + 1U, control);
    mach64_write8(MACH64_CLOCK_CNTL + 2U, value);
    mach64_write8(MACH64_CLOCK_CNTL + 1U,
                  (unsigned char)(control & ~MACH64_PLL_WR_EN));
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

static void mach64_restore_mode(const unsigned int crtc[5],
                                unsigned int gen, unsigned int dac,
                                unsigned int test, unsigned int clock,
                                unsigned char vclk, unsigned char post,
                                unsigned char xclk, unsigned char fb_div) {
    mach64_write32(MACH64_CRTC_H_TOTAL_DISP, crtc[0]);
    mach64_write32(MACH64_CRTC_H_SYNC_STRT_WID, crtc[1]);
    mach64_write32(MACH64_CRTC_V_TOTAL_DISP, crtc[2]);
    mach64_write32(MACH64_CRTC_V_SYNC_STRT_WID, crtc[3]);
    mach64_write32(MACH64_CRTC_OFF_PITCH, crtc[4]);
    mach64_pll_write(MACH64_PLL_VCLK_CNTL, vclk);
    mach64_pll_write(MACH64_PLL_VCLK_POST_DIV, post);
    mach64_pll_write(MACH64_PLL_XCLK_CNTL, xclk);
    mach64_pll_write((unsigned char)(MACH64_PLL_VCLK_FB_DIV0 +
                                     MACH64_PLL_VCLK_CLOCK), fb_div);
    mach64_write32(MACH64_CLOCK_CNTL, clock);
    mach64_write32(MACH64_GEN_TEST_CNTL, test);
    mach64_write32(MACH64_DAC_CNTL, dac);
    mach64_write32(MACH64_CRTC_GEN_CNTL, gen);
}

static int ati_mach64_supports_bpp(int w, int h, int bpp) {
    if (!g_ati_active) return 0;
    if (w < PDV_DISPLAY_MIN_WIDTH || h < PDV_DISPLAY_MIN_HEIGHT) return 0;
    if (w > 2048 || h > 1536) return 0;
    if (bpp != 16 && bpp != 24 && bpp != 32) return 0;
    if (!g_ati_hw_modeset_capable) {
        return w == g_boot_w && h == g_boot_h && bpp == g_boot_bpp;
    }
    if ((w & 7) != 0) return 0;
    pdv_display_timing_t timing;
    if (!g_kapi->display_get_mode_timing ||
        !g_kapi->display_get_mode_timing(w, h, &timing) ||
        timing.pixel_clock_khz == 0 || timing.pixel_clock_khz > 300000U) {
        return 0;
    }
    uint64_t pitch = ((uint64_t)(unsigned int)w *
                      (uint64_t)(unsigned int)(bpp / 8) + 63ULL) & ~63ULL;
    uint64_t bytes = pitch * (uint64_t)(unsigned int)h;
    return (bytes <= (uint64_t)g_ati_vram_bytes) ? 1 : 0;
}

static void ati_mach64_add_mode(int w, int h, uint8_t mask, int preferred) {
    if (mask == 0 || g_ati_mode_count >= MACH64_MAX_MODES) return;
    for (int index = 0; index < g_ati_mode_count; index++) {
        if (g_ati_modes[index].width == (uint16_t)w &&
            g_ati_modes[index].height == (uint16_t)h) {
            g_ati_modes[index].bpp_mask |= mask;
            g_ati_modes[index].preferred |= preferred ? 1U : 0U;
            return;
        }
    }
    pdv_display_mode_t* mode = &g_ati_modes[g_ati_mode_count++];
    mode->width = (uint16_t)w;
    mode->height = (uint16_t)h;
    mode->bpp_mask = mask;
    mode->preferred = preferred ? 1U : 0U;
    mode->reserved = 0;
}

static void ati_mach64_build_modes(void) {
    static const int k_bpps[3] = { 16, 24, 32 };
    g_ati_mode_count = 0;
    int catalog_count = g_kapi->display_get_mode_count
                            ? g_kapi->display_get_mode_count() : 0;
    for (int i = 0; i < catalog_count && g_ati_mode_count < MACH64_MAX_MODES;
         i++) {
        pdv_display_mode_t catalog_mode;
        if (!g_kapi->display_get_mode ||
            !g_kapi->display_get_mode(i, &catalog_mode)) {
            continue;
        }
        int w = catalog_mode.width;
        int h = catalog_mode.height;
        uint8_t mask = 0;
        for (int b = 0; b < 3; b++) {
            if ((catalog_mode.bpp_mask & pdv_bpp_to_mask(k_bpps[b])) &&
                ati_mach64_supports_bpp(w, h, k_bpps[b])) {
                mask |= pdv_bpp_to_mask(k_bpps[b]);
            }
        }
        ati_mach64_add_mode(w, h, mask, catalog_mode.preferred);
    }

    if (g_ati_mode_count == 0) {
        static const uint16_t k_width[] = { 640, 800, 1024, 1280, 1600 };
        static const uint16_t k_height[] = { 480, 600, 768, 1024, 1200 };
        for (size_t index = 0;
             index < sizeof(k_width) / sizeof(k_width[0]) &&
             g_ati_mode_count < MACH64_MAX_MODES; index++) {
            uint8_t mask = 0;
            for (int b = 0; b < 3; b++) {
                if (ati_mach64_supports_bpp(k_width[index],
                                            k_height[index], k_bpps[b])) {
                    mask |= pdv_bpp_to_mask(k_bpps[b]);
                }
            }
            ati_mach64_add_mode(k_width[index], k_height[index], mask, false);
        }
    }

    uint8_t boot_mask = 0;
    for (int b = 0; b < 3; b++) {
        if (ati_mach64_supports_bpp(g_boot_w, g_boot_h, k_bpps[b])) {
            boot_mask |= pdv_bpp_to_mask(k_bpps[b]);
        }
    }
    ati_mach64_add_mode(g_boot_w, g_boot_h, boot_mask, true);
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
        g_ati_bar0_phys = bar0_phys;
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
        g_boot_bpp = (surf.bpp == 16 || surf.bpp == 24 || surf.bpp == 32)
                         ? (int)surf.bpp : 32;
        g_cur_w = g_boot_w;
        g_cur_h = g_boot_h;
        g_cur_bpp = g_boot_bpp;
        g_cur_pitch = (int)surf.pitch;

        unsigned char ref_div = has_regs
            ? mach64_pll_read(0x02U) : 0U;
        g_ati_hw_modeset_capable =
            has_regs &&
            ref_div != 0 && ref_div != 0xFFU;
        g_ati_hw_modeset_last_status = g_ati_hw_modeset_capable
            ? "ATI Mach64 CRTC/PLL hardware modeset ready"
            : "Mach64 CRTC/PLL unavailable";

        g_ati_active = 1;
        ati_mach64_build_modes();
        g_ati_status = g_ati_hw_modeset_last_status;
        klog_info("[ATI ] active %s BAR0=0x%llx vram=%uKB regs=%s hwset=%s modes=%d",
                  g_ati_chip_name, (unsigned long long)bar0_phys,
                  g_ati_vram_bytes / 1024U,
                  has_regs ? "verified" : "probe-only",
                  g_ati_hw_modeset_capable ? "yes" : "no",
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
    int maximum = 0;
    for (int index = 0; index < g_ati_mode_count; index++) {
        if (g_ati_modes[index].bpp_mask &&
            (int)g_ati_modes[index].width > maximum) {
            maximum = g_ati_modes[index].width;
        }
    }
    return g_ati_active ? maximum : 0;
}

static int ati_mach64_get_max_height(void) {
    int maximum = 0;
    for (int index = 0; index < g_ati_mode_count; index++) {
        if (g_ati_modes[index].bpp_mask &&
            (int)g_ati_modes[index].height > maximum) {
            maximum = g_ati_modes[index].height;
        }
    }
    return g_ati_active ? maximum : 0;
}

static int ati_mach64_set_mode_bpp(int w, int h, int bpp) {
    if (!ati_mach64_supports_bpp(w, h, bpp)) return 0;
    if (!g_ati_hw_modeset_capable) {
        return w == g_cur_w && h == g_cur_h && bpp == g_cur_bpp;
    }

    pdv_display_timing_t timing;
    if (!g_kapi->display_get_mode_timing ||
        !g_kapi->display_get_mode_timing(w, h, &timing)) {
        return 0;
    }
    if (timing.h_display != (uint16_t)w ||
        timing.v_display != (uint16_t)h ||
        timing.pixel_clock_khz == 0 ||
        timing.h_total <= timing.h_display ||
        timing.v_total <= timing.v_display) {
        return 0;
    }

    int pitch = ((w * (bpp / 8) + 63) & ~63);
    uint64_t frame_bytes = (uint64_t)(unsigned int)pitch *
                           (uint64_t)(unsigned int)h;
    pdv_fb_surface_t surface;
    g_kapi->fb_get_surface(&surface);
    if (frame_bytes > 16ULL * 1024ULL * 1024ULL ||
        frame_bytes > (uint64_t)g_ati_vram_bytes) {
        return 0;
    }
    if (surface.framebuffer_phys < g_ati_bar0_phys ||
        surface.framebuffer_phys - g_ati_bar0_phys >= g_ati_vram_bytes ||
        surface.framebuffer_phys - g_ati_bar0_phys + frame_bytes >
            (uint64_t)g_ati_vram_bytes) {
        return 0;
    }

    unsigned int saved_crtc[5];
    saved_crtc[0] = mach64_read32(MACH64_CRTC_H_TOTAL_DISP);
    saved_crtc[1] = mach64_read32(MACH64_CRTC_H_SYNC_STRT_WID);
    saved_crtc[2] = mach64_read32(MACH64_CRTC_V_TOTAL_DISP);
    saved_crtc[3] = mach64_read32(MACH64_CRTC_V_SYNC_STRT_WID);
    saved_crtc[4] = mach64_read32(MACH64_CRTC_OFF_PITCH);
    unsigned int saved_gen = mach64_read32(MACH64_CRTC_GEN_CNTL);
    unsigned int saved_dac = mach64_read32(MACH64_DAC_CNTL);
    unsigned int saved_test = mach64_read32(MACH64_GEN_TEST_CNTL);
    unsigned int saved_clock = mach64_read32(MACH64_CLOCK_CNTL);
    unsigned char saved_vclk = mach64_pll_read(MACH64_PLL_VCLK_CNTL);
    unsigned char saved_post = mach64_pll_read(MACH64_PLL_VCLK_POST_DIV);
    unsigned char saved_xclk = mach64_pll_read(MACH64_PLL_XCLK_CNTL);
    unsigned char saved_fb = mach64_pll_read(
        MACH64_PLL_VCLK_FB_DIV0 + MACH64_PLL_VCLK_CLOCK);

    unsigned char ref_div = mach64_pll_read(0x02U);
    if (ref_div == 0 || ref_div == 0xFFU) return 0;
    static const uint16_t k_post_div[8] = { 1, 2, 4, 8, 3, 0, 6, 12 };
    uint32_t best_error = 0xFFFFFFFFU;
    unsigned int best_n = 0, best_post = 0;
    for (unsigned int post = 0; post < 8; post++) {
        if (!k_post_div[post]) continue;
        uint32_t divisor = (uint32_t)ref_div * 11U * k_post_div[post];
        uint32_t numerator = timing.pixel_clock_khz * divisor;
        unsigned int n = (numerator + 315000U / 2U) / 315000U;
        if (n < 2U) n = 2U;
        if (n > 255U) n = 255U;
        uint32_t actual = (n * 315000U + divisor / 2U) / divisor;
        uint32_t error = actual > timing.pixel_clock_khz
            ? actual - timing.pixel_clock_khz
            : timing.pixel_clock_khz - actual;
        if (error < best_error) {
            best_error = error;
            best_n = n;
            best_post = post;
        }
    }
    if (!best_n || best_error > timing.pixel_clock_khz / 100U + 500U) {
        return 0;
    }

    int h_display = (w >> 3) - 1;
    int h_sync_start = ((int)timing.h_sync_start >> 3) - 1;
    int h_sync_end = ((int)timing.h_sync_end >> 3) - 1;
    int h_total = ((int)timing.h_total >> 3) - 1;
    int v_display = h - 1;
    int v_sync_start = (int)timing.v_sync_start - 1;
    int v_sync_end = (int)timing.v_sync_end - 1;
    int v_total = (int)timing.v_total - 1;
    if (h_display < 0 || h_sync_start < 0 || h_sync_end < 0 || h_total < 0 ||
        h_display > 511 || h_total > 511 || h_sync_start > 255 ||
        h_sync_end > 255 || h_sync_start >= h_sync_end ||
        v_display < 0 || v_total < 0 || v_total > 2047 ||
        v_sync_start > 2047 || v_sync_end > 2047 ||
        v_sync_start >= v_sync_end ||
        h_sync_end - h_sync_start > 31 ||
        v_sync_end - v_sync_start > 31) {
        return 0;
    }

    unsigned int h_total_disp =
        ((unsigned int)h_total & 0x1FFU) |
        (((unsigned int)h_display & 0x1FFU) << 16);
    unsigned int h_sync =
        ((unsigned int)h_sync_start & 0xFFU) |
        ((((unsigned int)h_sync_start >> 8) & 1U) ? (1U << 12) : 0U) |
        (timing.h_sync_positive ? 0U : MACH64_CRTC_H_SYNC_POL) |
        (((unsigned int)(h_sync_end - h_sync_start) & 0x1FU) << 16);
    unsigned int v_total_disp =
        ((unsigned int)v_total & 0x7FFU) |
        (((unsigned int)v_display & 0x7FFU) << 16);
    unsigned int v_sync =
        ((unsigned int)v_sync_start & 0x7FFU) |
        (timing.v_sync_positive ? 0U : MACH64_CRTC_V_SYNC_POL) |
        (((unsigned int)(v_sync_end - v_sync_start) & 0x1FU) << 16);
    unsigned int offset_units =
        (unsigned int)((surface.framebuffer_phys - g_ati_bar0_phys) >> 3);
    unsigned int off_pitch =
        (offset_units & 0x000FFFFFU) |
        ((((unsigned int)pitch >> 3) & 0x3FFU) << 20);

    unsigned int pixel_width = bpp == 16 ? 4U :
                               bpp == 24 ? 5U : 6U;
    unsigned int new_gen = saved_gen;
    new_gen &= ~(0x00000FFFU | (1U << 20) | (1U << 21) |
                 (1U << 22) | (1U << 23) | (1U << 26) |
                 (1U << 29) | (1U << 31));
    new_gen |= MACH64_CRTC_EXT_DISP_EN | MACH64_CRTC_EN |
               MACH64_CRTC_VGA_LINEAR | MACH64_CRTC_CNT_EN |
               (pixel_width << 8);
    unsigned int new_dac = saved_dac &
        ~(MACH64_DAC_8BIT_EN | MACH64_DAC_RAMDAC_SELECT);

    int timeout = 200000;
    while (!(mach64_read32(MACH64_CRTC_INT_CNTL) & MACH64_CRTC_VBLANK) &&
           --timeout) {
    }

    mach64_write32(MACH64_CRTC_H_TOTAL_DISP, h_total_disp);
    mach64_write32(MACH64_CRTC_H_SYNC_STRT_WID, h_sync);
    mach64_write32(MACH64_CRTC_V_TOTAL_DISP, v_total_disp);
    mach64_write32(MACH64_CRTC_V_SYNC_STRT_WID, v_sync);
    mach64_write32(MACH64_CRTC_OFF_PITCH, off_pitch);

    unsigned char post_value = saved_post;
    unsigned int shift = MACH64_PLL_VCLK_CLOCK * 2U;
    post_value = (unsigned char)((post_value & ~(0x03U << shift)) |
                                 ((best_post & 0x03U) << shift));
    mach64_pll_write(MACH64_PLL_VCLK_CNTL,
                     (unsigned char)(saved_vclk | 0x03U |
                                     MACH64_PLL_VCLK_RESET));
    mach64_pll_write(MACH64_PLL_VCLK_POST_DIV, post_value);
    unsigned char xclk = saved_xclk;
    unsigned int xbit = 0x10U << MACH64_PLL_VCLK_CLOCK;
    xclk = (unsigned char)((xclk & ~xbit) |
                           (((best_post >> 2) & 1U) ? xbit : 0U));
    mach64_pll_write(MACH64_PLL_XCLK_CNTL, xclk);
    mach64_pll_write((unsigned char)(MACH64_PLL_VCLK_FB_DIV0 +
                                     MACH64_PLL_VCLK_CLOCK),
                     (unsigned char)(best_n - 1U));
    mach64_pll_write(MACH64_PLL_VCLK_CNTL,
                     (unsigned char)((saved_vclk | 0x03U) &
                                     ~MACH64_PLL_VCLK_RESET));
    g_kapi->sleep_ms(10);

    mach64_write32(MACH64_CLOCK_CNTL, MACH64_CLOCK_CNTL_PROGRAMMED);
    mach64_write32(MACH64_GEN_TEST_CNTL, saved_test | MACH64_GEN_GUI_EN);
    mach64_write32(MACH64_GEN_TEST_CNTL, saved_test);
    mach64_write32(MACH64_GEN_TEST_CNTL, saved_test | MACH64_GEN_GUI_EN);
    mach64_write32(MACH64_DAC_CNTL, new_dac);
    mach64_write32(MACH64_CRTC_GEN_CNTL, new_gen);

    unsigned int h_verify = mach64_read32(MACH64_CRTC_H_TOTAL_DISP);
    unsigned int v_verify = mach64_read32(MACH64_CRTC_V_TOTAL_DISP);
    if (h_verify != h_total_disp || v_verify != v_total_disp) {
        mach64_restore_mode(saved_crtc, saved_gen, saved_dac, saved_test,
                            saved_clock, saved_vclk, saved_post, saved_xclk,
                            saved_fb);
        return 0;
    }

    unsigned char* mapped =
        (unsigned char*)g_kapi->mmio_map_wc(surface.framebuffer_phys,
                                            (size_t)frame_bytes);
    if (!mapped) {
        mach64_restore_mode(saved_crtc, saved_gen, saved_dac, saved_test,
                            saved_clock, saved_vclk, saved_post, saved_xclk,
                            saved_fb);
        return 0;
    }
    pdv_fb_surface_t new_surface = surface;
    new_surface.width = (uint32_t)w;
    new_surface.height = (uint32_t)h;
    new_surface.pitch = (uint32_t)pitch;
    new_surface.bpp = (uint32_t)bpp;
    new_surface.format = bpp == 16 ? PDV_PIXEL_FORMAT_RGB565 :
                         bpp == 32 ? PDV_PIXEL_FORMAT_BGRA8888 :
                                     PDV_PIXEL_FORMAT_BGR888;
    new_surface.red_mask_size = bpp == 16 ? 5 : 8;
    new_surface.red_mask_shift = bpp == 16 ? 11 : 16;
    new_surface.green_mask_size = bpp == 16 ? 6 : 8;
    new_surface.green_mask_shift = bpp == 16 ? 5 : 8;
    new_surface.blue_mask_size = bpp == 16 ? 5 : 8;
    new_surface.blue_mask_shift = 0;
    g_kapi->fb_set_surface(&new_surface, mapped);

    g_cur_w = w;
    g_cur_h = h;
    g_cur_bpp = bpp;
    g_cur_pitch = pitch;
    g_ati_hw_modeset_last_status = "Active (Mach64 CRTC/PLL hardware modeset)";
    g_ati_status = g_ati_hw_modeset_last_status;
    return 1;
}

static const pdv_display_driver_t g_mach64_drv = {
    .name                 = "ATI Mach64",
    .priority             = 50,
    .requires_hw_mode_set = 1,
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
