#include <pdv/ddk.h>

static const pdv_kernel_api_t* g_kapi;

#define klog_info(fmt, ...) g_kapi->log_printf(fmt "\n", ##__VA_ARGS__)
#define mmio_read32         pdv_mmio_read32

#define NVIDIA_VENDOR_ID       0x10DE
#define PCI_DEV_GTX1060_3GB    0x1C02
#define PCI_DEV_GTX1060_6GB    0x1C03
#define PCI_DEV_GTX1060_5GB    0x1C06
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
#define PCI_SUBVEN_COLORFUL    0x7377
#define PCI_CAP_ID_EXP         0x10

#define NV_MC_BOOT_0_REG              0x00000000U
#define NV_GP100_MEM_CTRLR_RAM_AMOUNT 0x0090020CU

#define NV_MAX_MODES           128
#define NV_PCRTC_BASE          0x00600000U
#define NV_PCRTC_INTR_0        0x00000100U
#define NV_PCRTC_INTR_EN_0     0x00000140U
#define NV_PDISP_BASE          0x00610000U
#define NV_PRAMDAC_BASE        0x00680000U
#define NV_PCIO_BASE           0x00601000U
#define NV_PVIO_BASE           0x000C0000U
#define NV_HEAD_STRIDE         0x00002000U
#define NV_PCIO_HEAD_STRIDE    0x00002000U
#define NV_CRTC_PITCH_REG      0x00000820U
#define NV_CRTC_START_REG      0x00000800U
#define NV_HEAD_CONTROL_REG    0x00000860U
#define NV_SECOND_HEAD_REG     0x00002860U
#define NV_RAMDAC_PLL_REG      0x00000500U
#define NV_RAMDAC_PLL_SEL_REG  0x0000050CU
#define NV_RAMDAC_PLL_B_REG    0x00000578U
#define NV_RAMDAC_GENERAL_REG  0x00000600U

static const unsigned char g_nv_crtc_state_regs[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09,
    0x10, 0x11, 0x12, 0x13, 0x15, 0x16, 0x19, 0x1A, 0x1B,
    0x1C, 0x1F, 0x20, 0x25, 0x28, 0x2D, 0x39, 0x41, 0x44, 0x47
};

static const pdv_pci_device_t* g_pci;
static int g_active;
static int g_is_pascal_nvdisplay;
static int g_is_nv4x;
static uint32_t g_chip_arch;
static unsigned int g_vram_size;
static uint64_t g_bar0;
static uint64_t g_bar1;
static uint64_t g_bar1_size;
static int g_primary_7300gt;
static pdv_display_driver_t g_nvg7x_drv;
static int g_boot_w = 1024;
static int g_boot_h = 768;
static int g_boot_bpp = 16;
static int g_cur_w = 1024;
static int g_cur_h = 768;
static int g_cur_bpp = 16;
static int g_cur_pitch = 2048;
static int g_cur_refresh_hz = 60;
static uint32_t g_max_pclk_khz = 400000U;
static void (*g_vblank_cb)(void) = 0;
static int g_vblank_irq_registered = 0;
static int g_head;
static uint32_t g_crystal_khz = 13500U;
static uint32_t g_vpll_ref_khz = 54000U;
static uint32_t g_boot_vco_khz = 320000U;
static uint32_t g_boot_vpll;
static uint32_t g_boot_vpllb;
static uint32_t g_boot_vpll2b;
static uint32_t g_boot_0580;
static int g_has_boot_vpll;
static volatile unsigned char* g_regs;
static pdv_display_mode_t g_nv_modes[NV_MAX_MODES];
static int g_nv_mode_count;
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

static volatile unsigned char* nvidia_pcrtc(void) {
    return g_regs ? g_regs + NV_PCRTC_BASE + (unsigned int)g_head * NV_HEAD_STRIDE
                  : 0;
}

static volatile unsigned char* nvidia_ramdac(void) {
    return g_regs ? g_regs + NV_PRAMDAC_BASE +
                    (unsigned int)g_head * NV_HEAD_STRIDE : 0;
}

static unsigned int nvidia_pcrtc_read(unsigned int reg) {
    volatile unsigned char* pcrtc = nvidia_pcrtc();
    return pcrtc ? pdv_mmio_read32(pcrtc + reg) : 0xFFFFFFFFU;
}

static void nvidia_pcrtc_write(unsigned int reg, unsigned int value) {
    volatile unsigned char* pcrtc = nvidia_pcrtc();
    if (pcrtc) pdv_mmio_write32(pcrtc + reg, value);
}

static unsigned int nvidia_ramdac_read(unsigned int reg) {
    volatile unsigned char* dac = nvidia_ramdac();
    return dac ? pdv_mmio_read32(dac + reg) : 0xFFFFFFFFU;
}

static void nvidia_ramdac_write(unsigned int reg, unsigned int value) {
    volatile unsigned char* dac = nvidia_ramdac();
    if (dac) pdv_mmio_write32(dac + reg, value);
}

static unsigned int nvidia_ramdac0_read(unsigned int reg) {
    return g_regs ? pdv_mmio_read32(g_regs + NV_PRAMDAC_BASE + reg)
                  : 0xFFFFFFFFU;
}

static void nvidia_ramdac0_write(unsigned int reg, unsigned int value) {
    if (g_regs) pdv_mmio_write32(g_regs + NV_PRAMDAC_BASE + reg, value);
}

static volatile unsigned char* nvidia_pcio(void) {
    return g_regs ? g_regs + NV_PCIO_BASE +
                    (unsigned int)g_head * NV_PCIO_HEAD_STRIDE : 0;
}

static volatile unsigned char* nvidia_pvio(void) {
    return g_regs ? g_regs + NV_PVIO_BASE : 0;
}

static unsigned char nvidia_read_misc(void);

static unsigned int nvidia_crtc_index_port(void) {
    return (nvidia_read_misc() & 0x01U) ? 0x03D4U : 0x03B4U;
}

static unsigned int nvidia_vga_status_port(void) {
    return (nvidia_read_misc() & 0x01U) ? 0x03DAU : 0x03BAU;
}

static unsigned char nv_vga_read(unsigned char index) {
    volatile unsigned char* pcio = nvidia_pcio();
    if (!pcio) return 0xFFU;
    unsigned int index_port = nvidia_crtc_index_port();
    pdv_mmio_write8(pcio + index_port, index);
    return pdv_mmio_read8(pcio + index_port + 1U);
}

static void nv_vga_write(unsigned char index, unsigned char value) {
    volatile unsigned char* pcio = nvidia_pcio();
    if (!pcio) return;
    unsigned int index_port = nvidia_crtc_index_port();
    pdv_mmio_write8(pcio + index_port, index);
    pdv_mmio_write8(pcio + index_port + 1U, value);
}

static void nvidia_wait_vblank(void) {
    volatile unsigned char* pcio = nvidia_pcio();
    if (!pcio) return;
    unsigned int status_port = nvidia_vga_status_port();
    int timeout = 200000;
    while (!(pdv_mmio_read8(pcio + status_port) & 0x08U) && --timeout) {
    }
    timeout = 200000;
    while ((pdv_mmio_read8(pcio + status_port) & 0x08U) && --timeout) {
    }
}

static unsigned char nvidia_read_misc(void) {
    volatile unsigned char* pvio = nvidia_pvio();
    return pvio ? pdv_mmio_read8(pvio + 0x03CCU) : 0xFFU;
}

static void nvidia_write_misc(unsigned char value) {
    volatile unsigned char* pvio = nvidia_pvio();
    if (pvio) pdv_mmio_write8(pvio + 0x03C2U, value);
}

static void nvidia_write_seq(unsigned char index, unsigned char value) {
    volatile unsigned char* pvio = nvidia_pvio();
    if (!pvio) return;
    pdv_mmio_write8(pvio + 0x03C4U, index);
    pdv_mmio_write8(pvio + 0x03C5U, value);
}

static void nvidia_write_graphics(unsigned char index, unsigned char value) {
    volatile unsigned char* pvio = nvidia_pvio();
    if (!pvio) return;
    pdv_mmio_write8(pvio + 0x03CEU, index);
    pdv_mmio_write8(pvio + 0x03CFU, value);
}

static void nvidia_write_attribute(unsigned char index, unsigned char value) {
    volatile unsigned char* pcio = nvidia_pcio();
    if (!pcio) return;
    unsigned int status_port = nvidia_vga_status_port();
    (void)pdv_mmio_read8(pcio + status_port);
    pdv_mmio_write8(pcio + 0x03C0U, index);
    pdv_mmio_write8(pcio + 0x03C0U, value);
}

static void nvidia_disable_attribute_palette(void) {
    volatile unsigned char* pcio = nvidia_pcio();
    if (!pcio) return;
    unsigned int status_port = nvidia_vga_status_port();
    (void)pdv_mmio_read8(pcio + status_port);
    pdv_mmio_write8(pcio + 0x03C0U, 0x20U);
}

static unsigned char nvidia_read_attribute(unsigned char index) {
    volatile unsigned char* pcio = nvidia_pcio();
    if (!pcio) return 0xFFU;
    unsigned int status_port = nvidia_vga_status_port();
    (void)pdv_mmio_read8(pcio + status_port);
    pdv_mmio_write8(pcio + 0x03C0U, index);
    return pdv_mmio_read8(pcio + 0x03C1U);
}

typedef struct {
    unsigned int pcrtc[7];
    unsigned int dac[4];
    unsigned int dac0[8];
    unsigned char crtc[sizeof(g_nv_crtc_state_regs)];
    unsigned char crtc_select;
    unsigned char misc_out;
    unsigned char sequencer[5];
    unsigned char graphics[9];
    unsigned char attribute[21];
    unsigned int head_control;
    unsigned int second_head_control;
} nvidia_mode_state_t;

static int nvidia_calc_vpll(uint32_t target_khz, uint32_t ref_khz,
                            uint32_t* out_pll);

static int nvidia_has_edid_catalog(void) {
    if (!g_kapi || !g_kapi->display_get_mode_source) return 0;
    const char* src = g_kapi->display_get_mode_source();
    if (!src) return 0;
    return src[0] == 'E' && src[1] == 'D' && src[2] == 'I' && src[3] == 'D';
}

static int nvidia_g7x_hw_supports_mode_ex(int w, int h, int bpp, int refresh_hz) {
    if (!g_active) return 0;
    if (w < PDV_DISPLAY_MIN_WIDTH || h < PDV_DISPLAY_MIN_HEIGHT) return 0;
    if (w > 4096 || h > 2160) return 0;
    if (bpp != 16 && bpp != 32) return 0;
    if ((w & 7) != 0) return 0;
    if (refresh_hz <= 0) refresh_hz = 60;
    if (refresh_hz < 24 || refresh_hz > 240) return 0;

    if (g_is_pascal_nvdisplay) {
        return (w == g_boot_w && h == g_boot_h && bpp == g_boot_bpp &&
                refresh_hz == g_cur_refresh_hz) ? 1 : 0;
    }

    uint64_t pitch = ((uint64_t)(unsigned int)w *
                      (uint64_t)(unsigned int)(bpp / 8) + 63ULL) & ~63ULL;
    uint64_t bytes = pitch * (uint64_t)(unsigned int)h;
    if (g_vram_size > 0 && bytes > (uint64_t)g_vram_size) return 0;
    if (g_bar1_size > 0 && bytes > g_bar1_size) return 0;
    pdv_display_timing_t timing;
    int got_timing = 0;
    if (g_kapi->display_get_mode_timing_ex) {
        got_timing = g_kapi->display_get_mode_timing_ex(w, h, refresh_hz, &timing);
    } else if (refresh_hz == 60 && g_kapi->display_get_mode_timing) {
        got_timing = g_kapi->display_get_mode_timing(w, h, &timing);
    }
    if (!got_timing || timing.pixel_clock_khz == 0 ||
        timing.pixel_clock_khz > g_max_pclk_khz) {
        return 0;
    }
    uint32_t ref_khz = g_has_boot_vpll ? g_vpll_ref_khz : (g_crystal_khz * 4U);
    uint32_t vpll_check = 0;
    if (!nvidia_calc_vpll(timing.pixel_clock_khz, ref_khz, &vpll_check)) {
        return 0;
    }
    return 1;
}

static int nvidia_g7x_supports_mode_ex(int w, int h, int bpp, int refresh_hz) {
    if (refresh_hz <= 0) refresh_hz = 60;
    if (!nvidia_g7x_hw_supports_mode_ex(w, h, bpp, refresh_hz)) return 0;
    if (g_is_pascal_nvdisplay) return 1;
    if (g_nv_mode_count > 0) {
        uint8_t bpp_bit = pdv_bpp_to_mask(bpp);
        for (int index = 0; index < g_nv_mode_count; index++) {
            uint16_t mode_hz = g_nv_modes[index].refresh_hz
                                   ? g_nv_modes[index].refresh_hz : 60U;
            if ((int)g_nv_modes[index].width == w &&
                (int)g_nv_modes[index].height == h &&
                (int)mode_hz == refresh_hz &&
                (g_nv_modes[index].bpp_mask & bpp_bit) != 0) {
                return 1;
            }
        }
        return 0;
    }
    return 1;
}

static int nvidia_g7x_supports_bpp(int w, int h, int bpp) {
    return nvidia_g7x_supports_mode_ex(w, h, bpp, 60);
}

static void nvidia_add_mode(int w, int h, uint8_t mask, uint16_t refresh_hz, int preferred) {
    if (mask == 0 || g_nv_mode_count >= NV_MAX_MODES) return;
    if (refresh_hz == 0) refresh_hz = 60U;
    for (int index = 0; index < g_nv_mode_count; index++) {
        uint16_t existing_hz = g_nv_modes[index].refresh_hz ? g_nv_modes[index].refresh_hz : 60U;
        if (g_nv_modes[index].width == (uint16_t)w &&
            g_nv_modes[index].height == (uint16_t)h &&
            existing_hz == refresh_hz) {
            g_nv_modes[index].bpp_mask |= mask;
            g_nv_modes[index].preferred |= preferred ? 1U : 0U;
            return;
        }
    }
    pdv_display_mode_t* mode = &g_nv_modes[g_nv_mode_count++];
    mode->width = (uint16_t)w;
    mode->height = (uint16_t)h;
    mode->bpp_mask = mask;
    mode->preferred = preferred ? 1U : 0U;
    mode->refresh_hz = refresh_hz;
}

static void nvidia_g7x_build_modes(void) {
    static const int k_bpps[2] = { 16, 32 };
    g_nv_mode_count = 0;

    if (g_is_pascal_nvdisplay) {
        /* Pascal GP106: declare active UEFI GOP/VBE scanout mode with real EDID/GOP Hz */
        uint16_t active_hz = 60U;
        int catalog_count = g_kapi->display_get_mode_count
                                ? g_kapi->display_get_mode_count() : 0;
        for (int i = 0; i < catalog_count; i++) {
            pdv_display_mode_t cm;
            if (g_kapi->display_get_mode && g_kapi->display_get_mode(i, &cm)) {
                if ((int)cm.width == g_boot_w && (int)cm.height == g_boot_h &&
                    cm.refresh_hz > active_hz && cm.preferred) {
                    active_hz = cm.refresh_hz;
                }
            }
        }
        g_cur_refresh_hz = (int)active_hz;
        nvidia_add_mode(g_boot_w, g_boot_h, pdv_bpp_to_mask(g_boot_bpp), active_hz, 1);
        return;
    }

    int has_edid = nvidia_has_edid_catalog();
    int edid_max_w = 0;
    int edid_max_h = 0;
    int catalog_count = g_kapi->display_get_mode_count
                            ? g_kapi->display_get_mode_count() : 0;
    for (int i = 0; i < catalog_count && g_nv_mode_count < NV_MAX_MODES; i++) {
        pdv_display_mode_t catalog_mode;
        if (!g_kapi->display_get_mode ||
            !g_kapi->display_get_mode(i, &catalog_mode)) {
            continue;
        }
        int w = catalog_mode.width;
        int h = catalog_mode.height;
        uint16_t hz = catalog_mode.refresh_hz ? catalog_mode.refresh_hz : 60U;
        uint8_t mask = 0;
        for (int b = 0; b < 2; b++) {
            if ((catalog_mode.bpp_mask & pdv_bpp_to_mask(k_bpps[b])) &&
                nvidia_g7x_hw_supports_mode_ex(w, h, k_bpps[b], (int)hz)) {
                mask |= pdv_bpp_to_mask(k_bpps[b]);
            }
        }
        if (mask != 0) {
            if (w > edid_max_w) edid_max_w = w;
            if (h > edid_max_h) edid_max_h = h;
            nvidia_add_mode(w, h, mask, hz, catalog_mode.preferred);
        }
    }

    /*
     * Classic & widescreen presets:
     * - When valid EDID modes are present, only supplement standard 60 Hz modes
     *   within the monitor's advertised panel bounds so unadvertised high-Hz
     *   timings (e.g. 120/144/240 Hz) never drive the monitor out of range.
     * - When EDID is absent, offer conservative 60 Hz (plus 75 Hz <= 1280x1024).
     */
    static const uint16_t k_width[] = {
        640, 800, 1024, 1280, 1280, 1366, 1440, 1600, 1680, 1920
    };
    static const uint16_t k_height[] = {
        480, 600, 768, 720, 1024, 768, 900, 900, 1050, 1080
    };
    int strict_edid = (has_edid && g_nv_mode_count > 0);
    for (size_t index = 0;
         index < sizeof(k_width) / sizeof(k_width[0]) &&
         g_nv_mode_count < NV_MAX_MODES; index++) {
        if (strict_edid &&
            ((int)k_width[index] > edid_max_w || (int)k_height[index] > edid_max_h)) {
            continue;
        }
        int max_r = (!strict_edid && k_width[index] <= 1280U && k_height[index] <= 1024U) ? 2 : 1;
        static const uint16_t k_safe_hz[2] = { 60U, 75U };
        for (int r = 0; r < max_r && g_nv_mode_count < NV_MAX_MODES; r++) {
            uint8_t mask = 0;
            for (int b = 0; b < 2; b++) {
                if (nvidia_g7x_hw_supports_mode_ex(k_width[index], k_height[index],
                                                   k_bpps[b], (int)k_safe_hz[r])) {
                    mask |= pdv_bpp_to_mask(k_bpps[b]);
                }
            }
            if (mask != 0) {
                nvidia_add_mode(k_width[index], k_height[index], mask, k_safe_hz[r],
                                (r == 0 && k_width[index] == g_boot_w && k_height[index] == g_boot_h));
            }
        }
    }

    uint8_t boot_mask = 0;
    for (int b = 0; b < 2; b++) {
        if (nvidia_g7x_hw_supports_mode_ex(g_boot_w, g_boot_h, k_bpps[b], 60)) {
            boot_mask |= pdv_bpp_to_mask(k_bpps[b]);
        }
    }
    nvidia_add_mode(g_boot_w, g_boot_h, boot_mask, 60U, 1);
}

static int nvidia_g7x_get_mode_count(void) {
    return g_active ? g_nv_mode_count : 0;
}

static int nvidia_g7x_get_mode(int index, pdv_display_mode_t* out_mode) {
    if (!g_active || !out_mode || index < 0 || index >= g_nv_mode_count) {
        return 0;
    }
    *out_mode = g_nv_modes[index];
    return 1;
}

static int nvidia_g7x_get_max_width(void) {
    int maximum = 0;
    for (int index = 0; index < g_nv_mode_count; index++) {
        if (g_nv_modes[index].bpp_mask &&
            (int)g_nv_modes[index].width > maximum) {
            maximum = g_nv_modes[index].width;
        }
    }
    return g_active ? maximum : 0;
}

static int nvidia_g7x_get_max_height(void) {
    int maximum = 0;
    for (int index = 0; index < g_nv_mode_count; index++) {
        if (g_nv_modes[index].bpp_mask &&
            (int)g_nv_modes[index].height > maximum) {
            maximum = g_nv_modes[index].height;
        }
    }
    return g_active ? maximum : 0;
}

static void nvidia_save_mode_state(nvidia_mode_state_t* state) {
    static const unsigned int pcrtc_regs[7] = {
        0x00000100U, 0x00000140U, 0x00000800U, 0x00000810U,
        0x00000820U, 0x00000830U, 0x00000834U
    };
    static const unsigned int dac_regs[4] = {
        0x00000404U, 0x00000600U, 0x00000608U, 0x00000848U
    };
    for (int index = 0; index < 7; index++) {
        state->pcrtc[index] = nvidia_pcrtc_read(pcrtc_regs[index]);
    }
    for (int index = 0; index < 4; index++) {
        state->dac[index] = nvidia_ramdac_read(dac_regs[index]);
    }
    static const unsigned int dac0_regs[8] = {
        0x00000500U, 0x00000504U, 0x00000508U, 0x0000050CU,
        0x00000520U, 0x00000578U, 0x0000057CU, 0x00000580U
    };
    for (int index = 0; index < 8; index++) {
        state->dac0[index] = nvidia_ramdac0_read(dac0_regs[index]);
    }
    volatile unsigned char* pcio = nvidia_pcio();
    state->crtc_select = 0;
    if (pcio) {
        unsigned int index_port = nvidia_crtc_index_port();
        pdv_mmio_write8(pcio + index_port, 0x1FU);
        pdv_mmio_write8(pcio + index_port + 1U, 0x57U);
        pdv_mmio_write8(pcio + index_port, 0x44U);
        state->crtc_select = pdv_mmio_read8(pcio + index_port + 1U);
    }
    state->head_control =
        g_regs ? pdv_mmio_read32(g_regs + NV_PCRTC_BASE +
                                 NV_HEAD_CONTROL_REG) : 0xFFFFFFFFU;
    state->second_head_control =
        g_regs ? pdv_mmio_read32(g_regs + NV_PCRTC_BASE +
                                 NV_SECOND_HEAD_REG) : 0xFFFFFFFFU;
    for (size_t index = 0;
         index < sizeof(g_nv_crtc_state_regs); index++) {
        state->crtc[index] = nv_vga_read(g_nv_crtc_state_regs[index]);
    }
    state->misc_out = nvidia_read_misc();
    volatile unsigned char* pvio = nvidia_pvio();
    for (unsigned char index = 1; index < 5; index++) {
        if (pvio) pdv_mmio_write8(pvio + 0x03C4U, index);
        state->sequencer[index] =
            pvio ? pdv_mmio_read8(pvio + 0x03C5U) : 0xFFU;
    }
    for (unsigned char index = 0; index < 9; index++) {
        if (pvio) pdv_mmio_write8(pvio + 0x03CEU, index);
        state->graphics[index] =
            pvio ? pdv_mmio_read8(pvio + 0x03CFU) : 0xFFU;
    }
    for (unsigned char index = 0; index < 21; index++) {
        state->attribute[index] = nvidia_read_attribute(index);
    }
    nvidia_disable_attribute_palette();
}

static void nvidia_restore_mode_state(const nvidia_mode_state_t* state) {
    static const unsigned int pcrtc_regs[7] = {
        0x00000100U, 0x00000140U, 0x00000800U, 0x00000810U,
        0x00000820U, 0x00000830U, 0x00000834U
    };
    static const unsigned int dac_regs[4] = {
        0x00000404U, 0x00000600U, 0x00000608U, 0x00000848U
    };
    unsigned char protected_crtc = 0;
    unsigned char extended_lock = 0;
    for (size_t index = 0;
         index < sizeof(g_nv_crtc_state_regs); index++) {
        if (g_nv_crtc_state_regs[index] == 0x11) {
            protected_crtc = state->crtc[index];
        } else if (g_nv_crtc_state_regs[index] == 0x1F) {
            extended_lock = state->crtc[index];
        }
    }
    nv_vga_write(0x1F, 0x57U);
    nv_vga_write(0x11, (unsigned char)(protected_crtc & 0x7F));
    for (size_t index = 0;
         index < sizeof(g_nv_crtc_state_regs); index++) {
        if (g_nv_crtc_state_regs[index] != 0x11 &&
            g_nv_crtc_state_regs[index] != 0x1F) {
            nv_vga_write(g_nv_crtc_state_regs[index], state->crtc[index]);
        }
    }
    nv_vga_write(0x11, protected_crtc);
    nv_vga_write(0x1F, extended_lock);
    for (int index = 0; index < 7; index++) {
        nvidia_pcrtc_write(pcrtc_regs[index], state->pcrtc[index]);
    }
    for (int index = 0; index < 4; index++) {
        nvidia_ramdac_write(dac_regs[index], state->dac[index]);
    }
    static const unsigned int dac0_regs[8] = {
        0x00000500U, 0x00000504U, 0x00000508U, 0x0000050CU,
        0x00000520U, 0x00000578U, 0x0000057CU, 0x00000580U
    };
    for (int index = 0; index < 8; index++) {
        nvidia_ramdac0_write(dac0_regs[index], state->dac0[index]);
    }
    if (g_regs) {
        pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_HEAD_CONTROL_REG,
                         state->head_control);
        pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_SECOND_HEAD_REG,
                         state->second_head_control);
    }
    volatile unsigned char* pcio = nvidia_pcio();
    if (pcio) {
        unsigned int index_port = nvidia_crtc_index_port();
        pdv_mmio_write8(pcio + index_port, 0x44U);
        pdv_mmio_write8(pcio + index_port + 1U, state->crtc_select);
    }
    nvidia_write_misc(state->misc_out);
    for (unsigned char index = 1; index < 5; index++) {
        nvidia_write_seq(index, state->sequencer[index]);
    }
    for (unsigned char index = 0; index < 9; index++) {
        nvidia_write_graphics(index, state->graphics[index]);
    }
    for (unsigned char index = 0; index < 21; index++) {
        nvidia_write_attribute(index, state->attribute[index]);
    }
    nvidia_disable_attribute_palette();
}

static void nvidia_load_linear_palette(void) {
    if (!g_regs) return;
    volatile unsigned char* pdio =
        g_regs + NV_PRAMDAC_BASE + (unsigned int)g_head * NV_HEAD_STRIDE + 0x00001000U;
    pdv_mmio_write8(pdio + 0x03C6U, 0xFFU);
    pdv_mmio_write8(pdio + 0x03C8U, 0x00U);
    for (unsigned int i = 0; i < 256U; i++) {
        unsigned char c = (unsigned char)i;
        pdv_mmio_write8(pdio + 0x03C9U, c);
        pdv_mmio_write8(pdio + 0x03C9U, c);
        pdv_mmio_write8(pdio + 0x03C9U, c);
    }
    pdv_mmio_write8(pdio + 0x03C7U, 0xFFU);
    unsigned char r255 = pdv_mmio_read8(pdio + 0x03C9U);
    unsigned char g255 = pdv_mmio_read8(pdio + 0x03C9U);
    unsigned char b255 = pdv_mmio_read8(pdio + 0x03C9U);
    klog_info("[NV  ] RAMDAC gen=0x%08x lut[255]=(%u,%u,%u)",
              nvidia_ramdac_read(NV_RAMDAC_GENERAL_REG), r255, g255, b255);
}

static int nvidia_calc_vpll(uint32_t target_khz, uint32_t ref_khz,
                            uint32_t* out_pll) {
    if (!target_khz || !ref_khz || !out_pll || target_khz > 400000U) {
        return 0;
    }
    uint32_t boot_m = g_has_boot_vpll ? (g_boot_vpll & 0xFFU) : 0U;
    uint32_t boot_hi = g_has_boot_vpll ? (g_boot_vpll & 0xFFF80000U) : 0U;
    uint64_t vco_min = 128000ULL;
    uint64_t vco_max = 1000000ULL;
    if (g_has_boot_vpll && g_boot_vco_khz >= 100000U) {
        uint64_t low = (uint64_t)g_boot_vco_khz / 2ULL;
        uint64_t high = (uint64_t)g_boot_vco_khz * 2ULL;
        if (low >= 100000ULL && low < 400000ULL) vco_min = low;
        if (high >= 350000ULL && high <= 1200000ULL) vco_max = high;
    }
    uint32_t max_m = (boot_m > 14U && boot_m <= 32U) ? boot_m : 14U;
    uint32_t best_error = 0xFFFFFFFFU;
    for (unsigned int p = 0; p <= 6; p++) {
        uint64_t scaled = (uint64_t)target_khz << p;
        if (scaled < vco_min || scaled > vco_max) continue;
        for (unsigned int m = 1; m <= max_m; m++) {
            uint64_t n64 = (scaled * m + (ref_khz / 2ULL)) /
                           (uint64_t)ref_khz;
            if (n64 < 5ULL || n64 > 255ULL) continue;
            unsigned int n = (unsigned int)n64;
            uint32_t actual = (uint32_t)((((uint64_t)ref_khz * n) / m) >> p);
            uint32_t error = actual > target_khz
                ? actual - target_khz : target_khz - actual;
            if (error < best_error ||
                (error == best_error && m == boot_m)) {
                best_error = error;
                *out_pll = boot_hi | (p << 16) | (n << 8) | m;
            }
        }
    }
    return best_error != 0xFFFFFFFFU &&
           best_error <= target_khz / 100U + 500U;
}

static int nvidia_g7x_set_mode_ex(int w, int h, int bpp, int refresh_hz,
                                  const pdv_display_timing_t* in_timing) {
    if (refresh_hz <= 0) refresh_hz = 60;
    if (!nvidia_g7x_supports_mode_ex(w, h, bpp, refresh_hz)) return 0;
    if (g_is_pascal_nvdisplay) {
        pdv_fb_surface_t surf;
        g_kapi->fb_get_surface(&surf);
        if (w == (int)surf.width && h == (int)surf.height && bpp == (int)surf.bpp) {
            g_cur_w = w;
            g_cur_h = h;
            g_cur_bpp = bpp;
            g_cur_pitch = (int)surf.pitch;
            return 1;
        }
        return 0;
    }
    if (!g_regs) return 0;
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
        timing.pixel_clock_khz > g_max_pclk_khz ||
        timing.h_total <= timing.h_display ||
        timing.v_total <= timing.v_display) {
        return 0;
    }
    int pitch = (w * (bpp / 8) + 63) & ~63;
    uint64_t frame_bytes = (uint64_t)(unsigned int)pitch *
                           (uint64_t)(unsigned int)h;
    pdv_fb_surface_t surface;
    g_kapi->fb_get_surface(&surface);
    if (!surface.framebuffer_phys || !g_bar1 ||
        surface.framebuffer_phys < g_bar1 ||
        surface.framebuffer_phys - g_bar1 + frame_bytes > g_bar1_size) {
        return 0;
    }
    unsigned char* mapped =
        (unsigned char*)g_kapi->mmio_map_wc(surface.framebuffer_phys,
                                            (size_t)frame_bytes);
    if (!mapped) return 0;

    uint32_t boot0 = pdv_mmio_read32(g_regs + 0x00101000U);
    uint32_t crystal_khz = (boot0 & 0x40U) ? 14318U : 13500U;
    if (boot0 & (1U << 22)) {
        crystal_khz = 27000U;
    }
    if (g_crystal_khz == 13500U || g_crystal_khz == 14318U ||
        g_crystal_khz == 27000U) {
        crystal_khz = g_crystal_khz;
    }
    uint32_t ref_khz = g_has_boot_vpll ? g_vpll_ref_khz : (crystal_khz * 4U);
    uint32_t vpll;
    if (!nvidia_calc_vpll(timing.pixel_clock_khz, ref_khz, &vpll)) {
        return 0;
    }
    klog_info("[NV  ] modeset %dx%dx%d pclk=%ukHz xtal=%ukHz ref=%ukHz h=%u/%u/%u/%u v=%u/%u/%u/%u vpll=0x%08x",
              w, h, bpp, timing.pixel_clock_khz, crystal_khz, ref_khz,
              timing.h_display, timing.h_sync_start, timing.h_sync_end,
              timing.h_total, timing.v_display, timing.v_sync_start,
              timing.v_sync_end, timing.v_total, vpll);

    int horiz_display = (w / 8) - 1;
    int horiz_start = ((int)timing.h_sync_start / 8) - 1;
    int horiz_end = ((int)timing.h_sync_end / 8) - 1;
    int horiz_total = ((int)timing.h_total / 8) - 5;
    int horiz_blank_start = horiz_display;
    int horiz_blank_end = ((int)timing.h_total / 8) - 1;
    int vert_display = h - 1;
    int vert_start = (int)timing.v_sync_start - 1;
    int vert_end = (int)timing.v_sync_end - 1;
    int vert_total = (int)timing.v_total - 2;
    int vert_blank_start = vert_display;
    int vert_blank_end = (int)timing.v_total - 1;
    if (horiz_display < 0 || horiz_start < 0 || horiz_end < 0 ||
        horiz_total < 0 || vert_display < 0 || vert_start < 0 ||
        vert_end < 0 || vert_total < 0 ||
        horiz_start >= horiz_end || vert_start >= vert_end) {
        return 0;
    }

    nvidia_mode_state_t saved;
    nvidia_save_mode_state(&saved);
    nv_vga_write(0x1F, 0x57U);
    nv_vga_write(0x44, (unsigned char)(g_head ? 0x03U : 0x00U));
    nv_vga_write(0x1F, 0x57U);
    unsigned char saved_protect = nv_vga_read(0x11);
    nv_vga_write(0x11, (unsigned char)(saved_protect & 0x7FU));
    g_kapi->sleep_ms(20);
    nvidia_wait_vblank();

    unsigned char misc_out = nvidia_read_misc();
    unsigned char old_misc_out = misc_out;
    if (misc_out == 0xFFU || misc_out == 0x00U) {
        misc_out = 0x2BU;
    }
    misc_out |= 0x03U;
    misc_out &= (unsigned char)~(0x40U | 0x80U);
    if (!timing.h_sync_positive) misc_out |= 0x40U;
    if (!timing.v_sync_positive) misc_out |= 0x80U;
    nvidia_write_misc(misc_out);
    klog_info("[NV  ] VGA misc output 0x%02x -> 0x%02x (hsync=%s vsync=%s)",
              old_misc_out, misc_out,
              timing.h_sync_positive ? "+" : "-",
              timing.v_sync_positive ? "+" : "-");
    nvidia_write_seq(1, 0x01U);
    nvidia_write_seq(2, 0x0FU);
    nvidia_write_seq(3, 0x00U);
    nvidia_write_seq(4, 0x0EU);

    nv_vga_write(0x00, (unsigned char)horiz_total);
    nv_vga_write(0x01, (unsigned char)horiz_display);
    nv_vga_write(0x02, (unsigned char)horiz_blank_start);
    nv_vga_write(0x03, (unsigned char)((horiz_blank_end & 0x1F) | 0x80));
    nv_vga_write(0x04, (unsigned char)horiz_start);
    nv_vga_write(0x05, (unsigned char)(((horiz_blank_end >> 5) & 1) << 7) |
                              (unsigned char)(horiz_end & 0x1F));
    nv_vga_write(0x06, (unsigned char)vert_total);
    nv_vga_write(0x07,
        (unsigned char)(((vert_total >> 8) & 1) |
                        (((vert_display >> 8) & 1) << 1) |
                        (((vert_start >> 8) & 1) << 2) |
                        (((vert_blank_start >> 8) & 1) << 3) | 0x10 |
                        (((vert_total >> 9) & 1) << 5) |
                        (((vert_display >> 9) & 1) << 6) |
                        (((vert_start >> 9) & 1) << 7)));
    nv_vga_write(0x09, (unsigned char)((((vert_blank_start >> 9) & 1) << 5) |
                                        0x40));
    nv_vga_write(0x10, (unsigned char)vert_start);
    nv_vga_write(0x11, (unsigned char)((vert_end & 0x0F) | 0x20));
    nv_vga_write(0x12, (unsigned char)vert_display);
    nv_vga_write(0x13, (unsigned char)(pitch / 8));
    nv_vga_write(0x15, (unsigned char)vert_blank_start);
    nv_vga_write(0x16, (unsigned char)vert_blank_end);

    unsigned int pixel_depth = (unsigned int)(bpp / 8);
    unsigned int screen_value =
        (((unsigned int)horiz_blank_end >> 6) & 1U) << 4 |
        (((unsigned int)vert_blank_start >> 10) & 1U) << 3 |
        (((unsigned int)vert_start >> 10) & 1U) << 2 |
        (((unsigned int)vert_display >> 10) & 1U) << 1 |
        (((unsigned int)vert_total >> 10) & 1U);
    unsigned int horiz_value =
        (((unsigned int)horiz_total >> 8) & 1U) |
        ((((unsigned int)horiz_display >> 8) & 1U) << 1) |
        ((((unsigned int)horiz_blank_start >> 8) & 1U) << 2) |
        ((((unsigned int)horiz_start >> 8) & 1U) << 3);
    unsigned int extra_value =
        (((unsigned int)vert_total >> 11) & 1U) |
        ((((unsigned int)vert_display >> 11) & 1U) << 2) |
        ((((unsigned int)vert_start >> 11) & 1U) << 4) |
        ((((unsigned int)vert_blank_start >> 11) & 1U) << 6);
    unsigned int repaint0 = (((unsigned int)pitch / 8U) & 0x700U) >> 3;
    unsigned int repaint1 = w < 1280 ? 0x04U : 0x00U;
    unsigned int pixel_mode = pixel_depth > 2U ? 3U : pixel_depth;

    nv_vga_write(0x19, (unsigned char)repaint0);
    nv_vga_write(0x1a, (unsigned char)repaint1);
    nv_vga_write(0x1b, 128U);
    nv_vga_write(0x1c, (unsigned char)(nv_vga_read(0x1c) & ~0x20U));
    nv_vga_write(0x20, (unsigned char)0x80U);
    nv_vga_write(0x25, (unsigned char)screen_value);
    nv_vga_write(0x28, (unsigned char)((nv_vga_read(0x28) & 0x80U) | pixel_mode));
    nv_vga_write(0x2d, (unsigned char)horiz_value);
    nv_vga_write(0x39, 0xFFU);
    nv_vga_write(0x41, (unsigned char)extra_value);
    nv_vga_write(0x47, 0x04U);
    nv_vga_write(0x11, (unsigned char)((vert_end & 0x0F) | 0xA0U));

    nvidia_pcrtc_write(NV_CRTC_PITCH_REG, (unsigned int)pitch);
    nvidia_pcrtc_write(NV_CRTC_START_REG,
        (unsigned int)(surface.framebuffer_phys - g_bar1));
    nvidia_pcrtc_write(0x00000810U, 0x00000100U);
    nvidia_pcrtc_write(0x00000830U, (unsigned int)(h - 3));
    nvidia_pcrtc_write(0x00000834U, (unsigned int)(h - 1));

    if (g_regs) {
        uint32_t head_control =
            pdv_mmio_read32(g_regs + NV_PCRTC_BASE + NV_HEAD_CONTROL_REG);
        uint32_t second =
            pdv_mmio_read32(g_regs + NV_PCRTC_BASE + NV_SECOND_HEAD_REG);
        if (g_head == 0) {
            pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_HEAD_CONTROL_REG,
                             head_control | 0x00001000U);
            pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_SECOND_HEAD_REG,
                             second & ~0x00001000U);
        } else {
            pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_HEAD_CONTROL_REG,
                             head_control & ~0x00001000U);
            pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_SECOND_HEAD_REG,
                             second | 0x00001000U);
        }
    }

    unsigned int dac_control =
        nvidia_ramdac_read(0x00000404U) | (1U << 25);
    nvidia_ramdac_write(0x00000404U, dac_control);
    nvidia_ramdac_write(0x00000608U,
        nvidia_ramdac_read(0x00000608U) | (1U << 20));
    unsigned int pll_sel = nvidia_ramdac0_read(NV_RAMDAC_PLL_SEL_REG);
    if (pll_sel == 0xFFFFFFFFU || pll_sel == 0U) {
        pll_sel = 0x10000700U;
    }
    pll_sel |= 0x10000700U | (g_head ? 0x20000000U : 0U);
    nvidia_ramdac0_write(NV_RAMDAC_PLL_SEL_REG, pll_sel);
    if (g_has_boot_vpll) {
        nvidia_ramdac0_write(0x00000580U, g_boot_0580);
        nvidia_ramdac0_write(0x00000508U, vpll);
        nvidia_ramdac0_write(0x00000520U, vpll);
        nvidia_ramdac0_write(NV_RAMDAC_PLL_B_REG, g_boot_vpllb);
        nvidia_ramdac0_write(0x0000057CU, g_boot_vpll2b);
    } else {
        nvidia_ramdac0_write(0x00000580U,
            nvidia_ramdac0_read(0x00000580U) & 0xEFFFFEFFU);
        nvidia_ramdac0_write(0x00000508U, vpll);
        nvidia_ramdac0_write(0x00000520U, vpll);
        nvidia_ramdac0_write(NV_RAMDAC_PLL_B_REG, 0x80000401U);
        nvidia_ramdac0_write(0x0000057CU, 0x80000401U);
    }
    unsigned int general = bpp == 16 ? 0x00101100U : 0x00100100U;
    nvidia_ramdac_write(NV_RAMDAC_GENERAL_REG, general | 0x30U);
    nvidia_load_linear_palette();
    nvidia_write_graphics(0, 0x00U);
    nvidia_write_graphics(1, 0x00U);
    nvidia_write_graphics(2, 0x00U);
    nvidia_write_graphics(3, 0x00U);
    nvidia_write_graphics(4, 0x00U);
    nvidia_write_graphics(5, 0x40U);
    nvidia_write_graphics(6, 0x05U);
    nvidia_write_graphics(7, 0x0FU);
    nvidia_write_graphics(8, 0xFFU);
    for (unsigned char index = 0; index < 16; index++) {
        nvidia_write_attribute(index, index);
    }
    nvidia_write_attribute(0x10U, 0x01U);
    nvidia_write_attribute(0x11U, 0x00U);
    nvidia_write_attribute(0x12U, 0x0FU);
    nvidia_write_attribute(0x13U, 0x00U);
    nvidia_write_attribute(0x14U, 0x00U);
    nvidia_disable_attribute_palette();
    nvidia_pcrtc_write(0x00000140U, 0);
    nvidia_pcrtc_write(0x00000100U, 1);
    g_kapi->sleep_ms(20);

    unsigned char read_h_total = nv_vga_read(0x00);
    unsigned char read_h_high = nv_vga_read(0x2d);
    unsigned char read_pitch = nv_vga_read(0x13);
    unsigned char read_pixel = nv_vga_read(0x28);
    klog_info("[NV  ] CRTC readback h_total=0x%02x(expect 0x%02x) "
              "h_high=0x%02x(expect 0x%02x) pitch=0x%02x(expect 0x%02x) "
              "pixel=0x%02x(expect 0x%02x)",
              read_h_total, (unsigned char)horiz_total,
              read_h_high & 0x0FU, (unsigned char)(horiz_value & 0x0F),
              read_pitch, (unsigned char)(pitch / 8),
              read_pixel, (unsigned char)pixel_mode);
    /*
     * CRTC pitch and pixel-width are not reliably readable on every G7x
     * board even though writes commit correctly.  Use the two timer values
     * for rollback, and keep the remaining values diagnostic-only.
     */
    if (read_h_total != (unsigned char)horiz_total ||
        (read_h_high & 0x0F) != (unsigned char)(horiz_value & 0x0F)) {
        nvidia_restore_mode_state(&saved);
        return 0;
    }
    if (read_pitch != (unsigned char)(pitch / 8) ||
        read_pixel != (unsigned char)pixel_mode) {
        klog_info("[NV  ] CRTC advisory mismatch ignored (pitch/pixel)");
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
    if (g_vblank_cb && pdv_mmio_read32(g_regs + 0x00000140U) != 0U) {
        nvidia_pcrtc_write(NV_PCRTC_INTR_0, 0xFFFFFFFFU);
        nvidia_pcrtc_write(NV_PCRTC_INTR_EN_0, 1U);
    }
    g_status = g_is_nv4x ? "NV4x CRTC/VPLL hardware modeset active"
                         : "G7x CRTC/VPLL hardware modeset active";
    return 1;
}

static int nvidia_g7x_set_mode(int w, int h, int bpp) {
    return nvidia_g7x_set_mode_ex(w, h, bpp, 60, 0);
}

static int nvidia_g7x_get_current_refresh_hz(void) {
    return g_cur_refresh_hz > 0 ? g_cur_refresh_hz : 60;
}

static void nvidia_g7x_irq_handler(void* ctx) {
    (void)ctx;
    if (!g_active || !g_regs || g_is_pascal_nvdisplay) return;
    uint32_t pmc_intr = pdv_mmio_read32(g_regs + 0x00000100U);
    if (pmc_intr == 0U) return;

    uint32_t intr = nvidia_pcrtc_read(NV_PCRTC_INTR_0);
    if (intr != 0U) {
        nvidia_pcrtc_write(NV_PCRTC_INTR_0, intr);
    }

    unsigned int other_head = g_head ? 0U : 1U;
    volatile unsigned char* other_pcrtc =
        g_regs + NV_PCRTC_BASE + other_head * NV_HEAD_STRIDE;
    uint32_t other_intr = pdv_mmio_read32(other_pcrtc + NV_PCRTC_INTR_0);
    if (other_intr != 0U) {
        pdv_mmio_write32(other_pcrtc + NV_PCRTC_INTR_EN_0, 0U);
        pdv_mmio_write32(other_pcrtc + NV_PCRTC_INTR_0, other_intr);
    }

    /* Safety circuit-breaker: disarm INTA if any unmasked engine keeps PMC_INTR_0 asserted */
    if (pdv_mmio_read32(g_regs + 0x00000100U) != 0U) {
        pdv_mmio_write32(g_regs + 0x00000140U, 0U);
        nvidia_pcrtc_write(NV_PCRTC_INTR_EN_0, 0U);
        g_vblank_cb = 0;
        return;
    }

    if ((intr & 1U) && g_vblank_cb) {
        g_vblank_cb();
    }
}

static void nvidia_g7x_wait_vblank_pub(void) {
    if (!g_active || !g_regs) return;
    if (g_is_pascal_nvdisplay) {
        /* GP106 PDISP Head 0 RG_DPCA VBlank/scanline poll */
        volatile unsigned char* pdisp = g_regs + NV_PDISP_BASE;
        uint32_t init = pdv_mmio_read32(pdisp + 0x00001100U);
        if (init == 0xFFFFFFFFU) return;
        for (int t = 0; t < 200000; t++) {
            uint32_t cur = pdv_mmio_read32(pdisp + 0x00001100U);
            if ((cur >> 16) != (init >> 16)) break;
        }
        return;
    }
    nvidia_wait_vblank();
}

static int nvidia_g7x_enable_vblank_irq(void (*cb)(void)) {
    if (!g_active || !g_regs || g_is_pascal_nvdisplay || !g_pci) return 0;
    if (g_is_nv4x) {
        pdv_mmio_write32(g_regs + 0x00000140U, 0U);
        nvidia_pcrtc_write(NV_PCRTC_INTR_EN_0, 0U);
        g_vblank_cb = 0;
        return 0;
    }
    g_vblank_cb = cb;
    if (cb) {
        if (!g_vblank_irq_registered && g_kapi->pci_register_irq) {
            if (g_kapi->pci_register_irq(g_pci, nvidia_g7x_irq_handler, 0) == 0) {
                g_vblank_irq_registered = 1;
            }
        }
        if (g_vblank_irq_registered) {
            pdv_mmio_write32(g_regs + 0x00000140U, 0U); /* PMC_INTR_EN_0 = 0 */
            pdv_mmio_write32(g_regs + 0x00001140U, 0U); /* PBUS_INTR_EN = 0 */
            pdv_mmio_write32(g_regs + 0x00002140U, 0U); /* PFIFO_INTR_EN_0 = 0 */
            pdv_mmio_write32(g_regs + 0x00009140U, 0U); /* PTIMER_INTR_EN_0 = 0 */
            pdv_mmio_write32(g_regs + 0x00400140U, 0U); /* PGRAPH_INTR_EN = 0 */
            pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_PCRTC_INTR_EN_0, 0U);
            pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_HEAD_STRIDE + NV_PCRTC_INTR_EN_0, 0U);
            pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_PCRTC_INTR_0, 0xFFFFFFFFU);
            pdv_mmio_write32(g_regs + NV_PCRTC_BASE + NV_HEAD_STRIDE + NV_PCRTC_INTR_0, 0xFFFFFFFFU);
            uint32_t pmc_pending = pdv_mmio_read32(g_regs + 0x00000100U);
            if (pmc_pending != 0U) {
                klog_info("[NV  ] vblank IRQ suppressed (PMC_INTR_0=0x%08x)", pmc_pending);
                g_vblank_cb = 0;
                return 0;
            }
            nvidia_pcrtc_write(NV_PCRTC_INTR_EN_0, 1U);
            pdv_mmio_write32(g_regs + 0x00000140U, 1U); /* PMC_INTR_EN_0 */
            return 1;
        }
    } else {
        pdv_mmio_write32(g_regs + 0x00000140U, 0U);
        nvidia_pcrtc_write(NV_PCRTC_INTR_EN_0, 0U);
    }
    return 0;
}

static int nvidia_g7x_flip_scanout(uint64_t fb_offset_bytes) {
    if (!g_active || !g_regs || g_is_pascal_nvdisplay) return 0;
    if (g_bar1_size > 0 && fb_offset_bytes >= g_bar1_size) return 0;
    nvidia_pcrtc_write(NV_CRTC_START_REG, (unsigned int)fb_offset_bytes);
    return 1;
}

static unsigned int nvidia_probe_hw_vram(uint64_t bar0_phys, int is_pascal,
                                         unsigned int default_vram) {
    if (!bar0_phys) return default_vram;
    volatile uint8_t* pfb =
        (volatile uint8_t*)g_kapi->mmio_map_uc(bar0_phys + 0x100000ULL, 0x1000);
    if (pfb) {
        uint32_t cstatus = mmio_read32(pfb + 0x20C);
        if (cstatus != 0 && cstatus != 0xFFFFFFFFU) {
            if (is_pascal) {
                /* GF100+/GP10x 0x10020C reports total VRAM in MB (bits 31:0 or >>20) */
                uint64_t mb = cstatus;
                if (mb > 32768ULL) mb = (cstatus >> 20);
                if (mb >= 512ULL && mb <= 32768ULL) {
                    uint64_t bytes = mb * 1024ULL * 1024ULL;
                    if (bytes > 0xFFF00000ULL) bytes = 0xC0000000ULL;
                    return (unsigned int)bytes;
                }
            } else {
                uint32_t bytes = cstatus & 0xFFF00000U;
                if (bytes >= (16U * 1024U * 1024U) &&
                    bytes <= (1024U * 1024U * 1024U)) {
                    return bytes;
                }
            }
        }
    }
    if (is_pascal) {
        volatile uint8_t* mc_ram =
            (volatile uint8_t*)g_kapi->mmio_map_uc(bar0_phys + 0x900000ULL, 0x1000);
        if (mc_ram) {
            uint32_t ram_amount = mmio_read32(mc_ram + 0x20C);
            if (ram_amount != 0 && ram_amount != 0xFFFFFFFFU) {
                uint64_t bytes = 0;
                if (ram_amount >= 512U && ram_amount <= 32768U) {
                    bytes = (uint64_t)ram_amount * 1024ULL * 1024ULL;
                } else {
                    uint64_t shifted = (uint64_t)ram_amount << 12;
                    if (shifted >= (512ULL * 1024ULL * 1024ULL) &&
                        shifted <= (4096ULL * 1024ULL * 1024ULL)) {
                        bytes = shifted;
                    }
                }
                if (bytes > 0xFFF00000ULL) bytes = 0xC0000000ULL;
                /* 0x90020C may reflect a single FBPA partition on 192-bit GP106;
                 * never downgrade below default_vram (e.g. 3072MB on GTX 1060 3GB). */
                if (bytes >= (uint64_t)default_vram) {
                    return (unsigned int)bytes;
                }
            }
        }
    }
    return default_vram;
}

static int nvidia_g7x_probe(void) {
    g_active = 0;
    g_is_pascal_nvdisplay = 0;
    g_is_nv4x = 0;
    g_chip_arch = 0;
    g_pci = 0;
    g_vram_size = 0;
    g_bar0 = 0;
    g_bar1 = 0;
    g_bar1_size = 0;
    g_regs = 0;
    g_head = 0;
    g_primary_7300gt = 0;
    g_status = "NVIDIA PCIe device not found";

    unsigned char* cur_lfb = g_kapi->fb_get_lfb();
    int sw = 0, sh = 0, sbpp = 0;
    g_kapi->fb_get_desktop_mode(&sw, &sh, &sbpp);
    pdv_fb_surface_t surf;
    g_kapi->fb_get_surface(&surf);

    for (int i = 0; i < g_kapi->pci_get_device_count(); i++) {
        const pdv_pci_device_t* dev = g_kapi->pci_get_device(i);
        if (!dev || dev->vendor != NVIDIA_VENDOR_ID) continue;

        unsigned int vram = 0;
        int is_pascal = 0;
        switch (dev->device) {
            case PCI_DEV_GTX1060_3GB:
                if (dev->subsystem_vendor == PCI_SUBVEN_COLORFUL) {
                    g_name = "NVIDIA GeForce GTX 1060 3GB (GP106 Colorful)";
                } else {
                    g_name = "NVIDIA GeForce GTX 1060 3GB (GP106)";
                }
                vram = 3072U * 1024U * 1024U;
                is_pascal = 1;
                break;
            case PCI_DEV_GTX1060_6GB:
            case PCI_DEV_GTX1060_5GB:
                g_name = "NVIDIA GeForce GTX 1060 (GP106)";
                vram = 3072U * 1024U * 1024U;
                is_pascal = 1;
                break;
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
                g_is_nv4x = 1;
                break;
            case PCI_DEV_6600LE_NV43_A:
                g_name = "NVIDIA GeForce 6600 LE (NV43)";
                vram = 128U * 1024U * 1024U;
                g_is_nv4x = 1;
                break;
            case PCI_DEV_6600LE_NV43_B:
                g_name = "NVIDIA GeForce 6600 LE (NV43 PCIe)";
                vram = 128U * 1024U * 1024U;
                g_is_nv4x = 1;
                break;
            default:
                if (dev->class_code == 0x03) {
                    if (dev->device >= 0x1B00U && dev->device <= 0x1D9FU) {
                        g_name = "NVIDIA GeForce GTX 10-Series (Pascal)";
                        vram = 3072U * 1024U * 1024U;
                        is_pascal = 1;
                    } else {
                        g_name = "NVIDIA GeForce PCIe Graphics";
                        vram = 256U * 1024U * 1024U;
                        g_primary_7300gt = 1;
                    }
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

        if (g_bar0 && dev->bar[0].size >= 0x00700000ULL) {
            g_regs = (volatile unsigned char*)g_kapi->mmio_map_uc(
                g_bar0, 0x00700000U);
        }
        if (g_regs) {
            uint32_t pmc_boot0 = pdv_mmio_read32(g_regs + NV_MC_BOOT_0_REG);
            g_chip_arch = (pmc_boot0 >> 20) & 0x1FFU;
            if (g_chip_arch >= 0x040U && g_chip_arch <= 0x045U) {
                g_is_nv4x = 1;
            }
            if (g_chip_arch >= 0x050U && g_chip_arch != 0x1FFU) {
                /* NV50/Tesla through GP10x/Pascal use NVDisplay/EVO rather than NV4x PCRTC */
                if (g_chip_arch >= 0x130U || is_pascal) {
                    is_pascal = 1;
                }
            }
        }

        g_is_pascal_nvdisplay = is_pascal;
        if (!g_regs && !g_is_pascal_nvdisplay) {
            g_status = "NVIDIA MMIO aperture unavailable";
            continue;
        }

        if (g_is_pascal_nvdisplay) {
            g_nvg7x_drv.requires_hw_mode_set = 0;
            uint32_t pmc_boot0 = g_regs ? pdv_mmio_read32(g_regs + NV_MC_BOOT_0_REG) : 0;
            uint32_t boot0_straps = g_regs ? pdv_mmio_read32(g_regs + 0x00101000U) : 0;
            g_crystal_khz = (boot0_straps & (1U << 22)) ? 27000U : 27000U;
            klog_info("[NV  ] Pascal GP106 detected: id=%04x:%04x subsys=%04x:%04x rev=%02x PMC_BOOT_0=0x%08x arch=0x%03x straps=0x%08x",
                      dev->vendor, dev->device, dev->subsystem_vendor,
                      dev->subsystem_device, dev->revision,
                      pmc_boot0, g_chip_arch, boot0_straps);
        } else {
            g_nvg7x_drv.requires_hw_mode_set = 1;
            if (g_bar1 && surf.framebuffer_phys >= g_bar1) {
                uint32_t start_offset =
                    (uint32_t)(surf.framebuffer_phys - g_bar1);
                uint32_t head0 = pdv_mmio_read32(
                    g_regs + NV_PCRTC_BASE + NV_CRTC_START_REG);
                uint32_t head1 = pdv_mmio_read32(
                    g_regs + NV_PCRTC_BASE + NV_HEAD_STRIDE +
                    NV_CRTC_START_REG);
                if (head1 == start_offset && head0 != start_offset) g_head = 1;
            }
            {
                nv_vga_write(0x1F, 0x57U);
                nv_vga_write(0x44, (unsigned char)(g_head ? 0x03U : 0x00U));
                nv_vga_write(0x1F, 0x57U);
                unsigned char cr28 = nv_vga_read(0x28);
                unsigned char cr33 = nv_vga_read(0x33);
                /* Limit single-link TMDS digital panels to 165 MHz; RAMDAC/Dual-Link up to 400 MHz */
                g_max_pclk_khz = ((cr28 & 0x80U) && (cr33 & 0x01U)) ? 165000U : 400000U;
                uint32_t boot0 = pdv_mmio_read32(g_regs + 0x00101000U);
                g_crystal_khz = (boot0 & 0x40U) ? 14318U : 13500U;
                if (boot0 & (1U << 22)) {
                    g_crystal_khz = 27000U;
                }
                uint32_t boot_vpll =
                    nvidia_ramdac0_read(g_head ? 0x0520U : 0x0508U);
                uint32_t boot_vpllb = nvidia_ramdac0_read(0x0578U);
                uint32_t boot_vpll2b = nvidia_ramdac0_read(0x057CU);
                uint32_t boot_pllsel =
                    nvidia_ramdac0_read(NV_RAMDAC_PLL_SEL_REG);
                uint32_t boot_0580 = nvidia_ramdac0_read(0x0580U);
                g_has_boot_vpll = 0;
                g_vpll_ref_khz = g_crystal_khz * 4U;
                int bw = (surf.width >= PDV_DISPLAY_MIN_WIDTH) ? (int)surf.width : sw;
                int bh = (surf.height >= PDV_DISPLAY_MIN_HEIGHT) ? (int)surf.height : sh;
                pdv_display_timing_t boot_timing;
                uint32_t bm = boot_vpll & 0xFFU;
                uint32_t bn = (boot_vpll >> 8) & 0xFFU;
                uint32_t bp = (boot_vpll >> 16) & 0x07U;
                if (bm >= 1U && bm <= 32U && bn >= 4U &&
                    g_kapi->display_get_mode_timing &&
                    g_kapi->display_get_mode_timing(bw, bh, &boot_timing) &&
                    boot_timing.pixel_clock_khz >= 20000U) {
                    uint64_t bvco = (uint64_t)boot_timing.pixel_clock_khz << bp;
                    uint32_t bref = (uint32_t)((bvco * bm + (bn / 2U)) / bn);
                    if (bref >= 12000U && bref <= 500000U) {
                        g_boot_vpll = boot_vpll;
                        g_boot_vpllb = boot_vpllb;
                        g_boot_vpll2b = boot_vpll2b;
                        g_boot_0580 = boot_0580;
                        g_boot_vco_khz = (uint32_t)bvco;
                        g_vpll_ref_khz = bref;
                        g_has_boot_vpll = 1;
                    }
                }
                klog_info("[NV  ] output CRTC head=%d slaved=%d tv=%d straps=0x%08x xtal=%ukHz ref=%ukHz vpll=0x%08x vpllb=0x%08x pllsel=0x%08x ctrl=0x%08x",
                          g_head, (cr28 & 0x80U) ? 1 : 0,
                          (cr28 & 0x80U) && !(cr33 & 0x01U) ? 1 : 0,
                          boot0, g_crystal_khz, g_vpll_ref_khz, boot_vpll,
                          g_head ? boot_vpll2b : boot_vpllb,
                          boot_pllsel, boot_0580);
            }
        }

        unsigned int hw_vram = nvidia_probe_hw_vram(g_bar0, g_is_pascal_nvdisplay, vram);
        g_vram_size = hw_vram ? hw_vram : (vram ? vram : (unsigned int)bar_size);
        g_boot_w = (surf.width >= PDV_DISPLAY_MIN_WIDTH) ? (int)surf.width : sw;
        g_boot_h = (surf.height >= PDV_DISPLAY_MIN_HEIGHT) ? (int)surf.height : sh;
        if (g_boot_w < PDV_DISPLAY_MIN_WIDTH) g_boot_w = PDV_DISPLAY_MIN_WIDTH;
        if (g_boot_h < PDV_DISPLAY_MIN_HEIGHT) g_boot_h = PDV_DISPLAY_MIN_HEIGHT;
        g_boot_bpp = (surf.bpp == 16 || surf.bpp == 24 || surf.bpp == 32)
                         ? (int)surf.bpp : 32;
        g_cur_w = g_boot_w;
        g_cur_h = g_boot_h;
        g_cur_bpp = g_boot_bpp;
        g_cur_pitch = (int)surf.pitch;
        g_active = 1;
        nvidia_g7x_build_modes();

        if (g_is_pascal_nvdisplay) {
            if (g_bar1 && nvidia_framebuffer_in_bar(
                              surf.framebuffer_phys, bar_size)) {
                g_status = "GP106 Pascal UEFI GOP/VBE BAR1 scanout active";
            } else {
                g_status = "GP106 Pascal UEFI GOP/VBE scanout active";
            }
        } else if (g_bar1 && nvidia_framebuffer_in_bar(
                          surf.framebuffer_phys, bar_size)) {
            g_status = g_is_nv4x ? "NV4x CRTC/VPLL hardware modeset ready (BAR1)"
                                 : "G7x CRTC/VPLL hardware modeset ready (BAR1)";
        } else {
            g_status = g_is_nv4x ? "NV4x CRTC/VPLL hardware modeset ready"
                                 : "G7x CRTC/VPLL hardware modeset ready";
        }
        klog_info("[NV  ] active %s id=%04x:%04x subsys=%04x:%04x rev=%02x arch=0x%03x pcie=%02x head=%d BAR0=0x%llx BAR1=0x%llx(size=%lluMB) lfb=0x%llx vram=%uMB modes=%d",
                  g_name, dev->vendor, dev->device, dev->subsystem_vendor,
                  dev->subsystem_device, dev->revision, g_chip_arch, pcie,
                  g_head, (unsigned long long)g_bar0, (unsigned long long)g_bar1,
                  (unsigned long long)(g_bar1_size / (1024ULL * 1024ULL)),
                  (unsigned long long)surf.framebuffer_phys,
                  g_vram_size / (1024U * 1024U), g_nv_mode_count);
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

static pdv_display_driver_t g_nvg7x_drv = {
    .name                   = "NVIDIA G7x PCIe",
    .priority               = 80,
    .requires_hw_mode_set   = 1,
    .probe                  = nvidia_g7x_probe,
    .start                  = 0,
    .stop                   = 0,
    .get_name               = nvidia_g7x_get_name,
    .get_status             = nvidia_g7x_get_status,
    .is_active              = nvidia_g7x_is_active,
    .is_2d_active           = nvidia_g7x_is_2d_active,
    .get_vram_size          = nvidia_g7x_get_vram_size,
    .can_set_mode           = nvidia_g7x_supports_bpp,
    .supports_bpp           = nvidia_g7x_supports_bpp,
    .set_mode               = nvidia_g7x_set_mode,
    .fill_rect              = nvidia_g7x_fill_rect,
    .screen_blt             = nvidia_g7x_screen_blt,
    .present_rect           = nvidia_g7x_present_rect,
    .sync                   = nvidia_g7x_sync,
    .has_hw_cursor          = 0,
    .set_cursor             = 0,
    .get_mode_count         = nvidia_g7x_get_mode_count,
    .get_mode               = nvidia_g7x_get_mode,
    .get_max_width          = nvidia_g7x_get_max_width,
    .get_max_height         = nvidia_g7x_get_max_height,
    .can_set_mode_ex        = nvidia_g7x_supports_mode_ex,
    .set_mode_ex            = nvidia_g7x_set_mode_ex,
    .get_current_refresh_hz = nvidia_g7x_get_current_refresh_hz,
    .wait_vblank            = nvidia_g7x_wait_vblank_pub,
    .enable_vblank_irq      = nvidia_g7x_enable_vblank_irq,
    .flip_scanout           = nvidia_g7x_flip_scanout,
};

int pdv_driver_entry(const pdv_kernel_api_t* kapi) {
    if (!kapi || kapi->ddk_version < PDV_DDK_VERSION_1_0) return -1;
    g_kapi = kapi;
    return kapi->register_display_driver(&g_nvg7x_drv);
}
