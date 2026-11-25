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
#define MACH64_CRTC_VLINE_CRNT_VLINE 0x0010
#define MACH64_CRTC_OFF_PITCH       0x0014
#define MACH64_CRTC_INT_CNTL        0x0018
#define MACH64_CRTC_GEN_CNTL        0x001C
#define MACH64_DSP_CONFIG           0x0020
#define MACH64_DSP_ON_OFF           0x0024
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

#define MACH64_MAX_MODES            128
#define MACH64_CRTC_VBLANK_INT_EN   (1U << 1)
#define MACH64_CRTC_VBLANK_INT      (1U << 2)

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
#define MACH64_PLL_REF_DIV          0x02U
#define MACH64_PLL_GEN_CNTL         0x03U
#define MACH64_PLL_MCLK_FB_DIV      0x04U
#define MACH64_PLL_VCLK_CNTL        0x05U
#define MACH64_PLL_VCLK_RESET       0x04U
#define MACH64_PLL_VCLK_POST_DIV    0x06U
#define MACH64_PLL_VCLK_FB_DIV0     0x07U
#define MACH64_PLL_XCLK_CNTL        0x0BU
#define MACH64_PLL_DLL_CNTL         0x0CU
#define MACH64_PLL_VFC_CNTL         0x0DU
#define MACH64_PLL_VCLK_CLOCK       3U
#define MACH64_CLOCK_STROBE         0x40U
#define MACH64_DAC_MASK             0x00C2U

static int g_ati_active = 0;
static const pdv_pci_device_t* g_ati_pci = 0;
static unsigned short g_ati_device_id = 0;
static const char* g_ati_chip_name = "None";
static const char* g_ati_status = "Not detected";
static unsigned int g_ati_vram_bytes = 2U * 1024U * 1024U;
static unsigned int g_ati_xtal_khz = 14318U;
static unsigned int g_ati_max_dac_khz = 200000U;
static volatile unsigned char* g_ati_regs = 0;
static unsigned short g_ati_io_base = 0;
static uint64_t g_ati_bar0_phys;
static int g_cur_w = 800, g_cur_h = 600;
static int g_cur_bpp = 16;
static int g_cur_pitch = 1600;
static int g_cur_refresh_hz = 60;
static void (*g_ati_vblank_cb)(void) = 0;
static int g_ati_vblank_irq_registered = 0;
static int g_boot_w = 1024, g_boot_h = 768;
static int g_boot_bpp = 16;
static int g_ati_hw_modeset_capable;
static const char* g_ati_hw_modeset_last_status = "Mach64 CRTC/PLL unavailable";
static pdv_display_mode_t g_ati_modes[MACH64_MAX_MODES];
static int g_ati_mode_count = 0;

static int g_boot_regs_saved = 0;
static unsigned int g_boot_crtc[5];
static unsigned int g_boot_dsp_config = 0;
static unsigned int g_boot_dsp_on_off = 0;
static unsigned int g_boot_gen = 0;
static unsigned int g_boot_dac = 0;
static unsigned int g_boot_test = 0;
static unsigned int g_boot_mem_cntl = 0;
static unsigned char g_boot_clock_cntl = 0;
static unsigned char g_boot_pll_gen = 0;
static unsigned char g_boot_vclk = 0;
static unsigned char g_boot_post = 0;
static unsigned char g_boot_xclk = 0;
static unsigned char g_boot_fb = 0;
static unsigned char g_boot_dll_cntl = 0;
static unsigned char g_boot_vfc_cntl = 0;

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

static void mach64_pulse_dll_dsp(unsigned int dsp_config,
                                 unsigned int dsp_on_off) {
    unsigned int ram_type = mach64_read32(MACH64_CNFG_STAT0) & 0x07U;
    unsigned char dll_cntl = g_boot_dll_cntl;
    if (dll_cntl == 0 || dll_cntl == 0xFFU) {
        if (g_ati_device_id == PCI_DEVICE_ATI_RAGE_XL ||
            g_ati_device_id == PCI_DEVICE_ATI_RAGE_XL_PCI) {
            dll_cntl = 0x80U;
        } else if (ram_type >= 4U) {
            dll_cntl = 0xA6U;
        } else {
            dll_cntl = 0xA0U;
        }
    } else {
        dll_cntl &= (unsigned char)~0x40U;
    }
    unsigned char vfc_cntl = (g_boot_vfc_cntl != 0 && g_boot_vfc_cntl != 0xFFU)
                                 ? g_boot_vfc_cntl
                                 : 0x1BU;
    mach64_pll_write(MACH64_PLL_DLL_CNTL, dll_cntl);
    mach64_pll_write(MACH64_PLL_VFC_CNTL, vfc_cntl);
    mach64_write32(MACH64_DSP_CONFIG, dsp_config);
    mach64_write32(MACH64_DSP_ON_OFF, dsp_on_off);
    g_kapi->sleep_ms(10);
    mach64_pll_write(MACH64_PLL_DLL_CNTL, dll_cntl);
    g_kapi->sleep_ms(10);
    mach64_pll_write(MACH64_PLL_DLL_CNTL, (unsigned char)(dll_cntl | 0x40U));
    g_kapi->sleep_ms(10);
    mach64_pll_write(MACH64_PLL_DLL_CNTL, (unsigned char)(dll_cntl & ~0x40U));
}

static void mach64_restore_mode(const unsigned int crtc[5],
                                unsigned int dsp_config,
                                unsigned int dsp_on_off,
                                unsigned int gen, unsigned int dac,
                                unsigned int test, unsigned int mem_cntl,
                                unsigned char clock_cntl,
                                unsigned char pll_gen,
                                unsigned char vclk, unsigned char post,
                                unsigned char xclk, unsigned char fb_div,
                                unsigned char fb_clk_idx) {
    mach64_write32(MACH64_CRTC_GEN_CNTL, gen & ~MACH64_CRTC_EN);
    mach64_write32(MACH64_CRTC_H_TOTAL_DISP, crtc[0]);
    mach64_write32(MACH64_CRTC_H_SYNC_STRT_WID, crtc[1]);
    mach64_write32(MACH64_CRTC_V_TOTAL_DISP, crtc[2]);
    mach64_write32(MACH64_CRTC_V_SYNC_STRT_WID, crtc[3]);
    mach64_write32(MACH64_CRTC_OFF_PITCH, crtc[4]);
    mach64_write32(MACH64_CRTC_VLINE_CRNT_VLINE, 0U);
    mach64_write8(MACH64_CLOCK_CNTL,
                  (unsigned char)((clock_cntl & 0x0FU) | MACH64_CLOCK_STROBE));
    mach64_pll_write(MACH64_PLL_VCLK_CNTL,
                     (unsigned char)(vclk | MACH64_PLL_VCLK_RESET));
    mach64_pll_write(MACH64_PLL_VCLK_POST_DIV, post);
    mach64_pll_write(MACH64_PLL_XCLK_CNTL, xclk);
    mach64_pll_write((unsigned char)(MACH64_PLL_VCLK_FB_DIV0 +
                                     (fb_clk_idx & 0x03U)), fb_div);
    mach64_pll_write(MACH64_PLL_GEN_CNTL,
                     (unsigned char)((pll_gen & ~(0x01U | 0x02U)) | 0x04U));
    mach64_pll_write(MACH64_PLL_VCLK_CNTL,
                     (unsigned char)(vclk & ~MACH64_PLL_VCLK_RESET));
    g_kapi->sleep_ms(5);
    mach64_pll_write(MACH64_PLL_GEN_CNTL, pll_gen);
    mach64_pll_write(MACH64_PLL_VCLK_CNTL, vclk);
    g_kapi->sleep_ms(1);
    if (dsp_config || dsp_on_off) {
        mach64_pulse_dll_dsp(dsp_config, dsp_on_off);
    }
    if (mem_cntl && mem_cntl != 0xFFFFFFFFU) {
        mach64_write32(MACH64_MEM_CNTL, mem_cntl);
    }
    mach64_write8(MACH64_DAC_MASK, 0xFFU);
    mach64_write32(MACH64_GEN_TEST_CNTL, test | MACH64_GEN_GUI_EN);
    mach64_write32(MACH64_DAC_CNTL, dac);
    mach64_write32(MACH64_CRTC_GEN_CNTL, gen);
}

/*
 * Calculate Mach64 VT/GT/GT-B Display FIFO watermarks (DSP_CONFIG & DSP_ON_OFF)
 * using the exact algorithm from Linux drivers/video/fbdev/aty/mach64_ct.c
 * (aty_init_pll_ct + aty_dsp_gt).
 */
static int mach64_calc_dsp_gt(unsigned char ref_div,
                              unsigned int vclk_fb_div,
                              unsigned int vclk_post_div_real,
                              int bpp,
                              unsigned int* out_dsp_config,
                              unsigned int* out_dsp_on_off) {
    unsigned char mclk_fb_div = mach64_pll_read(MACH64_PLL_MCLK_FB_DIV);
    unsigned char pll_ext_cntl = mach64_pll_read(MACH64_PLL_XCLK_CNTL);
    unsigned int memcntl = mach64_read32(MACH64_MEM_CNTL);
    unsigned int ram_type = mach64_read32(MACH64_CNFG_STAT0) & 0x07U;
    if (ref_div == 0 || mclk_fb_div == 0 || mclk_fb_div == 0xFFU ||
        vclk_fb_div == 0 || vclk_post_div_real == 0 || bpp < 8) {
        return 0;
    }

    int xclk_post_div = (int)(pll_ext_cntl & 0x07U);
    unsigned int xclk_ref_div = 1U;
    switch (xclk_post_div) {
        case 0:
        case 1:
        case 2:
        case 3:
            break;
        case 4:
            xclk_ref_div = 3U;
            xclk_post_div = 0;
            break;
        default:
            xclk_post_div = 0;
            break;
    }
    /* PLL_MFB_TIMES_4_2B is bit 3 (0x08) of PLL_EXT_CNTL */
    if (pll_ext_cntl & 0x08U) {
        xclk_post_div -= 1;
    }

    unsigned int trp = (memcntl & 0x0300U) >> 8;
    unsigned int xclkpagefaultdelay =
        ((memcntl & 0x0C00U) >> 10) + ((memcntl & 0x1000U) >> 12) + trp + 2U;
    unsigned int xclkmaxrasdelay =
        ((memcntl & 0x070000U) >> 16) + trp + 2U;

    /*
     * Determine FIFO depth (24 vs 32 qwords). On Mach64 VT / GT / GT-B
     * (including 3D Rage II+DVD 1002:4755), we can also verify the exact FIFO
     * depth from the VBIOS boot DSP_ON_OFF / DSP_CONFIG watermarks:
     * (boot_dsp_off * 32) / boot_dsp_xclks == fifo_size - 1.
     */
    unsigned int fifo_size = 24U;
    if (g_ati_device_id == PCI_DEVICE_ATI_RAGE_PRO_PCI ||
        g_ati_device_id == PCI_DEVICE_ATI_RAGE_PRO_TRB ||
        g_ati_device_id == PCI_DEVICE_ATI_RAGE_PRO_AGP ||
        g_ati_device_id == PCI_DEVICE_ATI_RAGE_XL ||
        g_ati_device_id == PCI_DEVICE_ATI_RAGE_XL_PCI ||
        g_ati_device_id == PCI_DEVICE_ATI_RAGE_IIC_PCI ||
        g_ati_device_id == PCI_DEVICE_ATI_RAGE_IIC_AGP ||
        g_ati_device_id == PCI_DEVICE_ATI_RAGE_IIC_B ||
        g_ati_device_id == PCI_DEVICE_ATI_RAGE_IIC_C) {
        fifo_size = 32U;
    }
    unsigned int boot_xclks = g_boot_dsp_config & 0x3FFFU;
    unsigned int boot_off = g_boot_dsp_on_off & 0x7FFU;
    if (boot_xclks > 0 && boot_off > 0) {
        unsigned int est_fifo =
            ((boot_off << 5) + (boot_xclks >> 1)) / boot_xclks + 1U;
        fifo_size = (est_fifo >= 28U) ? 32U : 24U;
    }

    if (fifo_size == 24U) {
        xclkpagefaultdelay += 2U;
        xclkmaxrasdelay += 3U;
    }

    unsigned int dsp_loop_latency = 10U;
    switch (ram_type) {
        case 1U: /* DRAM */
            if (g_ati_vram_bytes <= 1024U * 1024U) {
                dsp_loop_latency = 10U;
            } else {
                dsp_loop_latency = 8U;
                xclkpagefaultdelay += 2U;
            }
            break;
        case 2U: /* EDO */
        case 3U: /* PSEUDO_EDO */
            if (g_ati_vram_bytes <= 1024U * 1024U) {
                dsp_loop_latency = 9U;
            } else {
                dsp_loop_latency = 8U;
                xclkpagefaultdelay += 1U;
            }
            break;
        case 4U: /* SDRAM */
            if (g_ati_vram_bytes <= 1024U * 1024U) {
                dsp_loop_latency = 11U;
            } else {
                dsp_loop_latency = 10U;
                xclkpagefaultdelay += 1U;
            }
            break;
        case 5U: /* SGRAM */
            dsp_loop_latency = 8U;
            xclkpagefaultdelay += 3U;
            break;
        default:
            dsp_loop_latency = 11U;
            xclkpagefaultdelay += 3U;
            break;
    }

    if (xclkmaxrasdelay <= xclkpagefaultdelay) {
        xclkmaxrasdelay = xclkpagefaultdelay + 1U;
    }
    if (g_boot_dsp_config) {
        dsp_loop_latency = (g_boot_dsp_config >> 16) & 0x0FU;
    }

    uint32_t multiplier = (uint32_t)mclk_fb_div * vclk_post_div_real;
    uint32_t divider = (uint32_t)vclk_fb_div * xclk_ref_div * ((uint32_t)bpp >> 2);
    int vshift = (6 - 2) - xclk_post_div;
    if (vshift < 0) vshift = 0;

    while (((multiplier | divider) & 1U) == 0U &&
           multiplier > 0U && divider > 0U) {
        multiplier >>= 1;
        divider >>= 1;
    }
    if (multiplier == 0U || divider == 0U) return 0;

    /* Determine DSP precision first (aty_dsp_gt lines 163-173) */
    uint32_t tmp = ((multiplier * fifo_size) << (unsigned int)vshift) / divider;
    int dsp_precision = -5;
    for (; tmp != 0U; dsp_precision++) {
        tmp >>= 1;
    }
    if (dsp_precision < 0) {
        dsp_precision = 0;
    } else if (dsp_precision > 7) {
        dsp_precision = 7;
    }

    unsigned int xshift = (unsigned int)(6 - dsp_precision);
    unsigned int uvshift = (unsigned int)vshift + xshift;

    /* Move on to dsp_off (aty_dsp_gt lines 176-177) */
    uint32_t dsp_off =
        ((multiplier * (fifo_size - 1U)) << uvshift) / divider -
        (1U << (uvshift - xshift));

    /* Calculate dsp_on (aty_dsp_gt lines 183-197) */
    uint32_t dsp_on = ((multiplier << uvshift) + divider) / divider;
    tmp = (xclkmaxrasdelay << xshift) + 1U;
    if (dsp_on < tmp) {
        dsp_on = tmp;
    }
    dsp_on = dsp_on + (tmp * 2U) + (xclkpagefaultdelay << xshift);

    /* Calculate rounding factor and apply it to dsp_on */
    tmp = ((1U << (unsigned int)(7 - dsp_precision)) - 1U) >> 1;
    dsp_on = ((dsp_on + tmp) / (tmp + 1U)) * (tmp + 1U);
    if (dsp_on >= ((dsp_off / (tmp + 1U)) * (tmp + 1U))) {
        dsp_on = dsp_off - (multiplier << uvshift) / divider;
        dsp_on = (dsp_on / (tmp + 1U)) * (tmp + 1U);
    }

    /* Last but not least: dsp_xclks (aty_dsp_gt line 200) */
    uint32_t dsp_xclks =
        ((multiplier << (uvshift + 5U)) + divider) / divider;
    if (dsp_off > 0x7FFU) dsp_off = 0x7FFU;
    if (dsp_on > 0x7FFU) dsp_on = 0x7FFU;
    if (dsp_xclks > 0x3FFFU) dsp_xclks = 0x3FFFU;

    *out_dsp_on_off = ((dsp_on & 0x7FFU) << 16) | (dsp_off & 0x7FFU);
    *out_dsp_config = (((uint32_t)dsp_precision & 0x07U) << 20) |
                      ((dsp_loop_latency & 0x0FU) << 16) |
                      (dsp_xclks & 0x3FFFU);
    return 1;
}

static int mach64_check_pll_and_dsp(uint32_t pixel_clock_khz, int bpp) {
    if (!g_ati_hw_modeset_capable || pixel_clock_khz == 0 ||
        pixel_clock_khz > g_ati_max_dac_khz) {
        return 0;
    }
    unsigned char ref_div = mach64_pll_read(MACH64_PLL_REF_DIV);
    if (ref_div == 0 || ref_div == 0xFFU) return 0;
    unsigned int xtal_2x = 2U * g_ati_xtal_khz;
    static const uint16_t k_post_div[8] = { 1, 2, 4, 8, 3, 0, 6, 12 };
    uint32_t best_error = 0xFFFFFFFFU;
    unsigned int best_n = 0, best_post = 0;
    int best_in_vco = 0;
    for (unsigned int post = 0; post < 8; post++) {
        if (!k_post_div[post]) continue;
        uint32_t divisor = (uint32_t)ref_div * (uint32_t)k_post_div[post];
        uint64_t numerator = (uint64_t)pixel_clock_khz * (uint64_t)divisor;
        unsigned int n = (unsigned int)((numerator + xtal_2x / 2U) / xtal_2x);
        if (n < 2U) n = 2U;
        if (n > 255U) n = 255U;
        int in_vco = (n >= 128U && n <= 255U) ? 1 : 0;
        uint32_t actual = (uint32_t)(((uint64_t)n * xtal_2x + divisor / 2U) / divisor);
        uint32_t error = actual > pixel_clock_khz
            ? actual - pixel_clock_khz : pixel_clock_khz - actual;
        if ((in_vco && !best_in_vco) ||
            (in_vco == best_in_vco && error < best_error)) {
            best_in_vco = in_vco;
            best_error = error;
            best_n = n;
            best_post = post;
        }
    }
    if (!best_n || best_error > pixel_clock_khz / 20U + 1000U) return 0;
    unsigned int dsp_cfg = 0, dsp_on_off = 0;
    if (mach64_calc_dsp_gt(ref_div, best_n, k_post_div[best_post], bpp,
                           &dsp_cfg, &dsp_on_off)) {
        uint32_t dsp_on  = (dsp_on_off >> 16) & 0x7FFU;
        uint32_t dsp_off = dsp_on_off & 0x7FFU;
        if (dsp_on >= dsp_off) return 0;
    }
    return 1;
}

static int ati_mach64_supports_mode_ex(int w, int h, int bpp, int refresh_hz) {
    if (!g_ati_active) return 0;
    if (w < PDV_DISPLAY_MIN_WIDTH || h < PDV_DISPLAY_MIN_HEIGHT) return 0;
    if (w > 2048 || h > 1536) return 0;
    if (bpp != 16 && bpp != 24 && bpp != 32) return 0;
    if (refresh_hz <= 0) refresh_hz = 60;
    if (refresh_hz < 24 || refresh_hz > 144) return 0;
    if (!g_ati_hw_modeset_capable) {
        return (w == g_boot_w && h == g_boot_h && bpp == g_boot_bpp && refresh_hz == 60) ? 1 : 0;
    }
    if ((w & 7) != 0) return 0;
    uint64_t pitch = (uint64_t)(unsigned int)w * (uint64_t)(unsigned int)(bpp / 8);
    uint64_t bytes = pitch * (uint64_t)(unsigned int)h;
    if (bytes > (uint64_t)g_ati_vram_bytes) return 0;

    pdv_display_timing_t timing;
    int got_timing = 0;
    if (g_kapi->display_get_mode_timing_ex) {
        got_timing = g_kapi->display_get_mode_timing_ex(w, h, refresh_hz, &timing);
    } else if (refresh_hz == 60 && g_kapi->display_get_mode_timing) {
        got_timing = g_kapi->display_get_mode_timing(w, h, &timing);
    }
    if (!got_timing || timing.pixel_clock_khz == 0 ||
        timing.pixel_clock_khz > g_ati_max_dac_khz) {
        return 0;
    }
    if (refresh_hz > 60 && !mach64_check_pll_and_dsp(timing.pixel_clock_khz, bpp)) {
        return 0;
    }
    return 1;
}

static int ati_mach64_supports_bpp(int w, int h, int bpp) {
    return ati_mach64_supports_mode_ex(w, h, bpp, 60);
}

static void ati_mach64_add_mode(int w, int h, uint8_t mask, uint16_t refresh_hz, int preferred) {
    if (mask == 0 || g_ati_mode_count >= MACH64_MAX_MODES) return;
    if (refresh_hz == 0) refresh_hz = 60U;
    for (int index = 0; index < g_ati_mode_count; index++) {
        uint16_t existing_hz = g_ati_modes[index].refresh_hz ? g_ati_modes[index].refresh_hz : 60U;
        if (g_ati_modes[index].width == (uint16_t)w &&
            g_ati_modes[index].height == (uint16_t)h &&
            existing_hz == refresh_hz) {
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
    mode->refresh_hz = refresh_hz;
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
        uint16_t hz = catalog_mode.refresh_hz ? catalog_mode.refresh_hz : 60U;
        uint8_t mask = 0;
        for (int b = 0; b < 3; b++) {
            if ((catalog_mode.bpp_mask & pdv_bpp_to_mask(k_bpps[b])) &&
                ati_mach64_supports_mode_ex(w, h, k_bpps[b], (int)hz)) {
                mask |= pdv_bpp_to_mask(k_bpps[b]);
            }
        }
        ati_mach64_add_mode(w, h, mask, hz, catalog_mode.preferred);
    }

    /* Direct bound mode table filtered by VRAM, chip DAC limit, and DSP FIFO watermarks */
    static const uint16_t k_width[]  = { 640, 800, 1024, 1280, 1280 };
    static const uint16_t k_height[] = { 480, 600,  768,  720, 1024 };
    static const uint16_t k_hz_list[5] = { 60U, 75U, 85U, 100U, 120U };
    for (size_t index = 0;
         index < sizeof(k_width) / sizeof(k_width[0]) &&
         g_ati_mode_count < MACH64_MAX_MODES; index++) {
        for (int r = 0; r < 5 && g_ati_mode_count < MACH64_MAX_MODES; r++) {
            uint8_t mask = 0;
            for (int b = 0; b < 3; b++) {
                if (ati_mach64_supports_mode_ex(k_width[index], k_height[index],
                                                k_bpps[b], (int)k_hz_list[r])) {
                    mask |= pdv_bpp_to_mask(k_bpps[b]);
                }
            }
            if (mask != 0) {
                ati_mach64_add_mode(k_width[index], k_height[index], mask, k_hz_list[r],
                                    (r == 0 && k_width[index] == g_boot_w && k_height[index] == g_boot_h));
            }
        }
    }

    uint8_t boot_mask = 0;
    for (int b = 0; b < 3; b++) {
        if (ati_mach64_supports_mode_ex(g_boot_w, g_boot_h, k_bpps[b], 60)) {
            boot_mask |= pdv_bpp_to_mask(k_bpps[b]);
        }
    }
    ati_mach64_add_mode(g_boot_w, g_boot_h, boot_mask, 60U, 1);
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

        g_ati_pci = pci;
        g_ati_device_id = device;
        g_ati_chip_name = name;
        if (device == PCI_DEVICE_ATI_RAGE_PRO_PCI ||
            device == PCI_DEVICE_ATI_RAGE_PRO_TRB ||
            device == PCI_DEVICE_ATI_RAGE_PRO_AGP ||
            device == PCI_DEVICE_ATI_RAGE_XL ||
            device == PCI_DEVICE_ATI_RAGE_XL_PCI) {
            g_ati_max_dac_khz = 230000U;
        } else if (device == PCI_DEVICE_ATI_RAGE_II ||
                   device == PCI_DEVICE_ATI_RAGE_IIC_PCI ||
                   device == PCI_DEVICE_ATI_RAGE_IIC_AGP ||
                   device == PCI_DEVICE_ATI_RAGE_IIC_B ||
                   device == PCI_DEVICE_ATI_RAGE_IIC_C) {
            g_ati_max_dac_khz = 200000U;
        } else {
            g_ati_max_dac_khz = 170000U;
        }
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
            ? mach64_pll_read(MACH64_PLL_REF_DIV) : 0U;
        g_ati_hw_modeset_capable =
            has_regs &&
            ref_div != 0 && ref_div != 0xFFU;
        g_boot_regs_saved = 0;
        if (g_ati_hw_modeset_capable) {
            g_boot_crtc[0] = mach64_read32(MACH64_CRTC_H_TOTAL_DISP);
            g_boot_crtc[1] = mach64_read32(MACH64_CRTC_H_SYNC_STRT_WID);
            g_boot_crtc[2] = mach64_read32(MACH64_CRTC_V_TOTAL_DISP);
            g_boot_crtc[3] = mach64_read32(MACH64_CRTC_V_SYNC_STRT_WID);
            g_boot_crtc[4] = mach64_read32(MACH64_CRTC_OFF_PITCH);
            g_boot_dsp_config = mach64_read32(MACH64_DSP_CONFIG);
            g_boot_dsp_on_off = mach64_read32(MACH64_DSP_ON_OFF);
            g_boot_gen = mach64_read32(MACH64_CRTC_GEN_CNTL);
            g_boot_dac = mach64_read32(MACH64_DAC_CNTL);
            g_boot_test = mach64_read32(MACH64_GEN_TEST_CNTL);
            g_boot_mem_cntl = mach64_read32(MACH64_MEM_CNTL);
            g_boot_clock_cntl = mach64_read8(MACH64_CLOCK_CNTL);
            g_boot_pll_gen = mach64_pll_read(MACH64_PLL_GEN_CNTL);
            g_boot_vclk = mach64_pll_read(MACH64_PLL_VCLK_CNTL);
            g_boot_post = mach64_pll_read(MACH64_PLL_VCLK_POST_DIV);
            g_boot_xclk = mach64_pll_read(MACH64_PLL_XCLK_CNTL);
            g_boot_dll_cntl = mach64_pll_read(MACH64_PLL_DLL_CNTL);
            g_boot_vfc_cntl = mach64_pll_read(MACH64_PLL_VFC_CNTL);
            unsigned char boot_clk_idx = (unsigned char)(g_boot_clock_cntl & 0x03U);
            g_boot_fb = mach64_pll_read(
                (unsigned char)(MACH64_PLL_VCLK_FB_DIV0 + boot_clk_idx));
            g_boot_regs_saved = 1;

            /*
             * Calibrate reference crystal frequency (14.318 MHz vs 29.498928 MHz
             * vs 28.636 MHz) from the active VBIOS boot VCLK dividers and
             *PLL_REF_DIV, with Linux atyfb_base.c ref_div heuristic fallback.
             */
            g_ati_xtal_khz = (ref_div > 48U) ? 29499U : 14318U;
            static const uint16_t k_post_div[8] = { 1, 2, 4, 8, 3, 0, 6, 12 };
            unsigned int post_code =
                ((g_boot_post >> (boot_clk_idx * 2U)) & 0x03U) |
                (((g_boot_xclk >> (4U + boot_clk_idx)) & 0x01U) << 2);
            unsigned int post_div = k_post_div[post_code & 7U];
            pdv_display_timing_t boot_timing;
            if (g_boot_fb > 0 && g_boot_fb != 0xFFU && post_div > 0 &&
                g_kapi->display_get_mode_timing &&
                g_kapi->display_get_mode_timing(g_boot_w, g_boot_h, &boot_timing) &&
                boot_timing.pixel_clock_khz > 0) {
                static const unsigned int k_xtals[3] = { 14318U, 29499U, 28636U };
                unsigned int best_xtal = g_ati_xtal_khz;
                unsigned int best_err = 0xFFFFFFFFU;
                unsigned int denom = (unsigned int)ref_div * post_div;
                for (int xi = 0; xi < 3; xi++) {
                    unsigned int vclk =
                        (2U * k_xtals[xi] * (unsigned int)g_boot_fb + denom / 2U) /
                        denom;
                    unsigned int err = (vclk > boot_timing.pixel_clock_khz)
                        ? (vclk - boot_timing.pixel_clock_khz)
                        : (boot_timing.pixel_clock_khz - vclk);
                    if (err < best_err) {
                        best_err = err;
                        best_xtal = k_xtals[xi];
                    }
                }
                if (best_err < boot_timing.pixel_clock_khz / 4U) {
                    g_ati_xtal_khz = best_xtal;
                }
            }
        }
        g_ati_hw_modeset_last_status = g_ati_hw_modeset_capable
            ? "ATI Mach64 CRTC/PLL hardware modeset ready"
            : "Mach64 CRTC/PLL unavailable";

        g_ati_active = 1;
        ati_mach64_build_modes();
        g_ati_status = g_ati_hw_modeset_last_status;
        klog_info("[ATI ] active %s BAR0=0x%llx vram=%uKB xtal=%ukHz ref_div=%u dsp=0x%08x/0x%08x regs=%s hwset=%s modes=%d",
                  g_ati_chip_name, (unsigned long long)bar0_phys,
                  g_ati_vram_bytes / 1024U,
                  g_ati_xtal_khz, (unsigned int)ref_div,
                  g_boot_dsp_config, g_boot_dsp_on_off,
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

static int ati_mach64_set_mode_ex(int w, int h, int bpp, int refresh_hz,
                                  const pdv_display_timing_t* in_timing) {
    if (refresh_hz <= 0) refresh_hz = 60;
    if (!ati_mach64_supports_mode_ex(w, h, bpp, refresh_hz)) return 0;
    if (w == g_cur_w && h == g_cur_h && bpp == g_cur_bpp &&
        refresh_hz == g_cur_refresh_hz) {
        return 1;
    }
    if (!g_ati_hw_modeset_capable) {
        return 0;
    }

    pdv_display_timing_t timing;
    if (in_timing && in_timing->pixel_clock_khz > 0) {
        timing = *in_timing;
    } else if (g_kapi->display_get_mode_timing_ex &&
               g_kapi->display_get_mode_timing_ex(w, h, refresh_hz, &timing)) {
        /* ok */
    } else if (g_kapi->display_get_mode_timing &&
               g_kapi->display_get_mode_timing(w, h, &timing)) {
        /* fallback */
    } else {
        return 0;
    }
    if (timing.h_display != (uint16_t)w ||
        timing.v_display != (uint16_t)h ||
        timing.pixel_clock_khz == 0 ||
        timing.pixel_clock_khz > g_ati_max_dac_khz ||
        timing.h_total <= timing.h_display ||
        timing.v_total <= timing.v_display) {
        return 0;
    }

    /*
     * Mach64 CRTC_OFF_PITCH bits 31:22 store pitch in units of 8 pixels
     * ((line_length / bytes_per_pixel) >> 3 == w >> 3), so line pitch in bytes
     * must be exact w * (bpp / 8). Since w is a multiple of 8, pitch is also
     * a multiple of 8 bytes.
     */
    int pitch = w * (bpp / 8);
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

    if (w == g_boot_w && h == g_boot_h && bpp == g_boot_bpp &&
        refresh_hz == 60 && g_boot_regs_saved) {
        unsigned char* mapped =
            (unsigned char*)g_kapi->mmio_map_wc(surface.framebuffer_phys,
                                                (size_t)frame_bytes);
        if (!mapped) return 0;
        mach64_restore_mode(g_boot_crtc, g_boot_dsp_config, g_boot_dsp_on_off,
                            g_boot_gen, g_boot_dac, g_boot_test,
                            g_boot_mem_cntl, g_boot_clock_cntl,
                            g_boot_pll_gen, g_boot_vclk, g_boot_post,
                            g_boot_xclk, g_boot_fb,
                            (unsigned char)(g_boot_clock_cntl & 0x03U));
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
        g_cur_refresh_hz = 60;
        return 1;
    }

    unsigned int saved_crtc[5];
    saved_crtc[0] = mach64_read32(MACH64_CRTC_H_TOTAL_DISP);
    saved_crtc[1] = mach64_read32(MACH64_CRTC_H_SYNC_STRT_WID);
    saved_crtc[2] = mach64_read32(MACH64_CRTC_V_TOTAL_DISP);
    saved_crtc[3] = mach64_read32(MACH64_CRTC_V_SYNC_STRT_WID);
    saved_crtc[4] = mach64_read32(MACH64_CRTC_OFF_PITCH);
    unsigned int saved_dsp_config = mach64_read32(MACH64_DSP_CONFIG);
    unsigned int saved_dsp_on_off = mach64_read32(MACH64_DSP_ON_OFF);
    unsigned int saved_gen = mach64_read32(MACH64_CRTC_GEN_CNTL);
    unsigned int saved_dac = mach64_read32(MACH64_DAC_CNTL);
    unsigned int saved_test = mach64_read32(MACH64_GEN_TEST_CNTL);
    unsigned int saved_mem_cntl = mach64_read32(MACH64_MEM_CNTL);
    unsigned char saved_clock_cntl = mach64_read8(MACH64_CLOCK_CNTL);
    unsigned char saved_pll_gen = mach64_pll_read(MACH64_PLL_GEN_CNTL);
    unsigned char saved_vclk = mach64_pll_read(MACH64_PLL_VCLK_CNTL);
    unsigned char saved_post = mach64_pll_read(MACH64_PLL_VCLK_POST_DIV);
    unsigned char saved_xclk = mach64_pll_read(MACH64_PLL_XCLK_CNTL);
    unsigned char saved_fb = mach64_pll_read(
        MACH64_PLL_VCLK_FB_DIV0 + MACH64_PLL_VCLK_CLOCK);

    unsigned char ref_div = mach64_pll_read(MACH64_PLL_REF_DIV);
    if (ref_div == 0 || ref_div == 0xFFU) return 0;
    unsigned int xtal_2x = 2U * g_ati_xtal_khz;
    static const uint16_t k_post_div[8] = { 1, 2, 4, 8, 3, 0, 6, 12 };
    uint32_t best_error = 0xFFFFFFFFU;
    unsigned int best_n = 0, best_post = 0;
    int best_in_vco = 0;
    for (unsigned int post = 0; post < 8; post++) {
        if (!k_post_div[post]) continue;
        uint32_t divisor = (uint32_t)ref_div * (uint32_t)k_post_div[post];
        uint64_t numerator = (uint64_t)timing.pixel_clock_khz * (uint64_t)divisor;
        unsigned int n = (unsigned int)((numerator + xtal_2x / 2U) / xtal_2x);
        if (n < 2U) n = 2U;
        if (n > 255U) n = 255U;
        int in_vco = (n >= 128U && n <= 255U) ? 1 : 0;
        uint32_t actual = (uint32_t)(((uint64_t)n * xtal_2x + divisor / 2U) / divisor);
        uint32_t error = actual > timing.pixel_clock_khz
            ? actual - timing.pixel_clock_khz
            : timing.pixel_clock_khz - actual;
        if ((in_vco && !best_in_vco) ||
            (in_vco == best_in_vco && error < best_error)) {
            best_in_vco = in_vco;
            best_error = error;
            best_n = n;
            best_post = post;
        }
    }
    if (!best_n || best_error > timing.pixel_clock_khz / 20U + 1000U) {
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
        h_display > 511 || h_total > 511 || h_sync_start > 511 ||
        h_sync_end > 511 || h_sync_start >= h_sync_end ||
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
    unsigned int pitch_in_8px = (unsigned int)(w >> 3);
    unsigned int off_pitch =
        (offset_units & 0x000FFFFFU) |
        ((pitch_in_8px & 0x3FFU) << 22);

    unsigned int pixel_width = bpp == 16 ? 4U :
                               bpp == 24 ? 5U : 6U;
    /*
     * Preserve CRTC_FIFO_LWM (bits 19:16) and lock bits (bits 15:12);
     * do NOT set MACH64_CRTC_CNT_EN (bit 30), matching Linux aty_var_to_crtc.
     */
    unsigned int new_gen = (saved_gen & 0x000FE000U) |
                           MACH64_CRTC_EXT_DISP_EN |
                           MACH64_CRTC_EN |
                           MACH64_CRTC_VGA_LINEAR |
                           (pixel_width << 8);
    if (bpp == 24) {
        new_gen |= (1U << 26); /* CRTC_24BPP_ACCELERATION_ENABLE / BYTE_PIX_ORDER */
    }
    unsigned int new_dac = saved_dac &
        ~(MACH64_DAC_8BIT_EN | MACH64_DAC_RAMDAC_SELECT);

    unsigned int dsp_config = saved_dsp_config;
    unsigned int dsp_on_off = saved_dsp_on_off;
    (void)mach64_calc_dsp_gt(ref_div, best_n, k_post_div[best_post], bpp,
                             &dsp_config, &dsp_on_off);

    int timeout = 200000;
    while (!(mach64_read32(MACH64_CRTC_INT_CNTL) & MACH64_CRTC_VBLANK) &&
           --timeout) {
    }

    /*
     * Program CRTC and PLL following Linux atyfb_set_par / aty_set_crtc /
     * aty_set_pll_ct (drivers/video/fbdev/aty/atyfb_base.c & mach64_ct.c).
     */
    mach64_write32(MACH64_CRTC_GEN_CNTL, new_gen & ~MACH64_CRTC_EN);
    mach64_write32(MACH64_CRTC_H_TOTAL_DISP, h_total_disp);
    mach64_write32(MACH64_CRTC_H_SYNC_STRT_WID, h_sync);
    mach64_write32(MACH64_CRTC_V_TOTAL_DISP, v_total_disp);
    mach64_write32(MACH64_CRTC_V_SYNC_STRT_WID, v_sync);
    mach64_write32(MACH64_CRTC_OFF_PITCH, off_pitch);
    mach64_write32(MACH64_CRTC_VLINE_CRNT_VLINE, 0U);
    mach64_write32(MACH64_CRTC_GEN_CNTL, new_gen);

    /* Select VCLK3 with CLOCK_STROBE (0x43) via 8-bit write */
    mach64_write8(MACH64_CLOCK_CNTL,
                  (unsigned char)(MACH64_PLL_VCLK_CLOCK | MACH64_CLOCK_STROBE));

    unsigned char post_value = saved_post;
    unsigned int shift = MACH64_PLL_VCLK_CLOCK * 2U;
    post_value = (unsigned char)((post_value & ~(0x03U << shift)) |
                                 ((best_post & 0x03U) << shift));
    unsigned char vclk_cntl_val = (unsigned char)((saved_vclk & ~0x30U) | 0x03U);
    mach64_pll_write(MACH64_PLL_VCLK_CNTL,
                     (unsigned char)(vclk_cntl_val | MACH64_PLL_VCLK_RESET));
    mach64_pll_write(MACH64_PLL_VCLK_POST_DIV, post_value);
    unsigned char xclk = saved_xclk;
    unsigned int xbit = 0x10U << MACH64_PLL_VCLK_CLOCK;
    xclk = (unsigned char)((xclk & ~xbit) |
                           (((best_post >> 2) & 1U) ? xbit : 0U));
    mach64_pll_write(MACH64_PLL_XCLK_CNTL, xclk);
    mach64_pll_write((unsigned char)(MACH64_PLL_VCLK_FB_DIV0 +
                                     MACH64_PLL_VCLK_CLOCK),
                     (unsigned char)best_n);
    mach64_pll_write(MACH64_PLL_GEN_CNTL,
                     (unsigned char)((saved_pll_gen & ~(0x01U | 0x02U)) | 0x04U));
    mach64_pll_write(MACH64_PLL_VCLK_CNTL,
                     (unsigned char)(vclk_cntl_val & ~MACH64_PLL_VCLK_RESET));
    g_kapi->sleep_ms(5);
    mach64_pll_write(MACH64_PLL_GEN_CNTL, saved_pll_gen);
    mach64_pll_write(MACH64_PLL_VCLK_CNTL, vclk_cntl_val);
    g_kapi->sleep_ms(1);

    if (dsp_config || dsp_on_off) {
        mach64_pulse_dll_dsp(dsp_config, dsp_on_off);
    }

    /* Update MEM_CNTL aperture pixel depth bits 27:26 (atyfb_set_par lines 178-208) */
    if (saved_mem_cntl != 0xFFFFFFFFU) {
        unsigned int new_mem_cntl = saved_mem_cntl & 0xF3FFFFFFU;
        if (bpp == 16) {
            new_mem_cntl |= 0x04000000U;
        } else if (bpp == 32) {
            new_mem_cntl |= 0x08000000U;
        }
        mach64_write32(MACH64_MEM_CNTL, new_mem_cntl);
    }
    mach64_write8(MACH64_DAC_MASK, 0xFFU);
    mach64_write32(MACH64_GEN_TEST_CNTL, saved_test | MACH64_GEN_GUI_EN);
    mach64_write32(MACH64_DAC_CNTL, new_dac);
    mach64_write32(MACH64_CRTC_GEN_CNTL, new_gen);

    klog_info("[ATI ] set_mode %dx%d@%d pclk=%ukHz fb_div=%u post=%u(div=%u) off_pitch=0x%08x dsp=0x%08x/0x%08x gen=0x%08x",
              w, h, bpp, timing.pixel_clock_khz, best_n, best_post,
              (unsigned int)k_post_div[best_post], off_pitch,
              dsp_config, dsp_on_off, new_gen);

    unsigned int h_verify = mach64_read32(MACH64_CRTC_H_TOTAL_DISP);
    unsigned int v_verify = mach64_read32(MACH64_CRTC_V_TOTAL_DISP);
    if (h_verify != h_total_disp || v_verify != v_total_disp) {
        mach64_restore_mode(saved_crtc, saved_dsp_config, saved_dsp_on_off,
                            saved_gen, saved_dac, saved_test,
                            saved_mem_cntl, saved_clock_cntl,
                            saved_pll_gen, saved_vclk, saved_post,
                            saved_xclk, saved_fb, MACH64_PLL_VCLK_CLOCK);
        return 0;
    }

    unsigned char* mapped =
        (unsigned char*)g_kapi->mmio_map_wc(surface.framebuffer_phys,
                                            (size_t)frame_bytes);
    if (!mapped) {
        mach64_restore_mode(saved_crtc, saved_dsp_config, saved_dsp_on_off,
                            saved_gen, saved_dac, saved_test,
                            saved_mem_cntl, saved_clock_cntl,
                            saved_pll_gen, saved_vclk, saved_post,
                            saved_xclk, saved_fb, MACH64_PLL_VCLK_CLOCK);
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
    g_cur_refresh_hz = timing.refresh_hz ? (int)timing.refresh_hz : refresh_hz;
    g_ati_hw_modeset_last_status = "Active (Mach64 CRTC/PLL hardware modeset)";
    g_ati_status = g_ati_hw_modeset_last_status;
    return 1;
}

static int ati_mach64_set_mode_bpp(int w, int h, int bpp) {
    return ati_mach64_set_mode_ex(w, h, bpp, 60, 0);
}

static int ati_mach64_get_current_refresh_hz(void) {
    return g_cur_refresh_hz > 0 ? g_cur_refresh_hz : 60;
}

static void ati_mach64_wait_vblank(void) {
    if (!g_ati_active || !g_ati_regs) return;
    int timeout = 200000;
    while (!(mach64_read32(MACH64_CRTC_INT_CNTL) & MACH64_CRTC_VBLANK) && --timeout) {}
    timeout = 200000;
    while ((mach64_read32(MACH64_CRTC_INT_CNTL) & MACH64_CRTC_VBLANK) && --timeout) {}
}

static void ati_mach64_irq_handler(void* ctx) {
    (void)ctx;
    if (!g_ati_active || !g_ati_regs) return;
    unsigned int val = mach64_read32(MACH64_CRTC_INT_CNTL);
    if (val & MACH64_CRTC_VBLANK_INT) {
        mach64_write32(MACH64_CRTC_INT_CNTL, val | MACH64_CRTC_VBLANK_INT);
        if (g_ati_vblank_cb) g_ati_vblank_cb();
    }
}

static int ati_mach64_enable_vblank_irq(void (*cb)(void)) {
    if (!g_ati_active || !g_ati_regs || !g_ati_pci) return 0;
    g_ati_vblank_cb = cb;
    if (cb) {
        if (!g_ati_vblank_irq_registered && g_kapi->pci_register_irq) {
            if (g_kapi->pci_register_irq(g_ati_pci, ati_mach64_irq_handler, 0) == 0) {
                g_ati_vblank_irq_registered = 1;
            }
        }
        if (g_ati_vblank_irq_registered) {
            unsigned int val = mach64_read32(MACH64_CRTC_INT_CNTL);
            mach64_write32(MACH64_CRTC_INT_CNTL,
                           val | MACH64_CRTC_VBLANK_INT_EN | MACH64_CRTC_VBLANK_INT);
            return 1;
        }
    } else {
        unsigned int val = mach64_read32(MACH64_CRTC_INT_CNTL);
        mach64_write32(MACH64_CRTC_INT_CNTL, val & ~MACH64_CRTC_VBLANK_INT_EN);
    }
    return 0;
}

static int ati_mach64_flip_scanout(uint64_t fb_offset_bytes) {
    if (!g_ati_active || !g_ati_regs) return 0;
    if (fb_offset_bytes >= (uint64_t)g_ati_vram_bytes) return 0;
    unsigned int cur = mach64_read32(MACH64_CRTC_OFF_PITCH);
    unsigned int offset_units = (unsigned int)(fb_offset_bytes >> 3) & 0x000FFFFFU;
    mach64_write32(MACH64_CRTC_OFF_PITCH, (cur & ~0x000FFFFFU) | offset_units);
    return 1;
}

static const pdv_display_driver_t g_mach64_drv = {
    .name                   = "ATI Mach64",
    .priority               = 50,
    .requires_hw_mode_set   = 1,
    .probe                  = ati_mach64_probe_entry,
    .start                  = 0,
    .stop                   = 0,
    .get_name               = ati_mach64_get_chip_name,
    .get_status             = ati_mach64_get_status,
    .is_active              = ati_mach64_is_active,
    .is_2d_active           = ati_mach64_is_2d_active,
    .get_vram_size          = ati_mach64_get_vram_size,
    .can_set_mode           = ati_mach64_supports_bpp,
    .supports_bpp           = ati_mach64_supports_bpp,
    .set_mode               = ati_mach64_set_mode_bpp,
    .fill_rect              = 0,
    .screen_blt             = 0,
    .present_rect           = 0,
    .sync                   = ati_mach64_sync,
    .has_hw_cursor          = 0,
    .set_cursor             = 0,
    .get_mode_count         = ati_mach64_get_mode_count,
    .get_mode               = ati_mach64_get_mode,
    .get_max_width          = ati_mach64_get_max_width,
    .get_max_height         = ati_mach64_get_max_height,
    .can_set_mode_ex        = ati_mach64_supports_mode_ex,
    .set_mode_ex            = ati_mach64_set_mode_ex,
    .get_current_refresh_hz = ati_mach64_get_current_refresh_hz,
    .wait_vblank            = ati_mach64_wait_vblank,
    .enable_vblank_irq      = ati_mach64_enable_vblank_irq,
    .flip_scanout           = ati_mach64_flip_scanout,
};

int pdv_driver_entry(const pdv_kernel_api_t* kapi) {
    if (!kapi || kapi->ddk_version < PDV_DDK_VERSION_1_0) return -1;
    g_kapi = kapi;
    return kapi->register_display_driver(&g_mach64_drv);
}
