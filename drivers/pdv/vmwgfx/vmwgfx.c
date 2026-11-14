#include <pdv/ddk.h>

static const pdv_kernel_api_t* g_kapi;

#define klog_info(fmt, ...) g_kapi->log_printf(fmt "\n", ##__VA_ARGS__)
#define klog_warn(fmt, ...) g_kapi->log_printf(fmt "\n", ##__VA_ARGS__)
#define mmio_read32         pdv_mmio_read32
#define mmio_write32        pdv_mmio_write32
#define mmio_barrier        pdv_mmio_barrier
#define outb                pdv_outb
#define inl                 pdv_inl
#define outl                pdv_outl

#define VMWARE_PCI_VENDOR_ID     0x15AD
#define VMWARE_SVGA_II_ID        0x0405
#define VMWARE_SVGA_II_FUSION_ID 0x0406
#define VMWARE_SVGA_ADAPTER_ID   0x0710

#define SVGA_CAP_RECT_FILL         0x00000001U
#define SVGA_CAP_RECT_COPY         0x00000002U
#define SVGA_CAP_8BIT_EMULATION    0x00000100U
#define SVGA_CAP_ALPHA_CURSOR      0x00000200U
#define SVGA_CAP_3D                0x00004000U
#define SVGA_CAP_EXTENDED_FIFO     0x00008000U
#define SVGA_CAP_PITCHLOCK         0x00020000U
#define SVGA_CAP_GMR               0x00100000U
#define SVGA_CAP_GMR2              0x00400000U
#define SVGA_CAP_SCREEN_OBJECT_2   0x00800000U
#define SVGA_CAP_GBOBJECTS         0x08000000U
#define SVGA_CAP_CAP2_REGISTER     0x80000000U

#define SVGA_CAP2_GB_MEMSIZE_2     0x00000008U

#define SVGA_ID_0 0x90000000U
#define SVGA_ID_1 0x90000001U
#define SVGA_ID_2 0x90000002U

#define SVGA_REG_ID                              0
#define SVGA_REG_ENABLE                          1
#define SVGA_REG_WIDTH                           2
#define SVGA_REG_HEIGHT                          3
#define SVGA_REG_MAX_WIDTH                       4
#define SVGA_REG_MAX_HEIGHT                      5
#define SVGA_REG_DEPTH                           6
#define SVGA_REG_BPP                             7
#define SVGA_REG_PSEUDOCOLOR                     8
#define SVGA_REG_RED_MASK                        9
#define SVGA_REG_GREEN_MASK                      10
#define SVGA_REG_BLUE_MASK                       11
#define SVGA_REG_BYTES_PER_LINE                  12
#define SVGA_REG_FB_START                        13
#define SVGA_REG_FB_OFFSET                       14
#define SVGA_REG_VRAM_SIZE                       15
#define SVGA_REG_FB_SIZE                         16
#define SVGA_REG_CAPABILITIES                    17
#define SVGA_REG_MEM_SIZE                        19
#define SVGA_REG_CONFIG_DONE                     20
#define SVGA_REG_SYNC                            21
#define SVGA_REG_BUSY                            22
#define SVGA_REG_GUEST_ID                        23
#define SVGA_REG_CURSOR_ID                       24
#define SVGA_REG_CURSOR_X                        25
#define SVGA_REG_CURSOR_Y                        26
#define SVGA_REG_CURSOR_ON                       27
#define SVGA_REG_MEM_REGS                        30
#define SVGA_REG_GMR_MAX_IDS                     43
#define SVGA_REG_TRACES                          45
#define SVGA_REG_GMRS_MAX_PAGES                  46
#define SVGA_REG_MEMORY_SIZE                     47
#define SVGA_REG_MAX_PRIMARY_MEM                 50
#define SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB  51
#define SVGA_REG_SCREENTARGET_MAX_WIDTH          55
#define SVGA_REG_SCREENTARGET_MAX_HEIGHT         56
#define SVGA_REG_CAP2                            59
#define SVGA_REG_GBOBJECT_MEM_SIZE_KB            76
#define GUEST_OS_OTHER                           0x5010U
#define SVGA_PALETTE_BASE                        1024

#define SVGA_REG_ENABLE_DISABLE 0U
#define SVGA_REG_ENABLE_ENABLE  (1U << 0)
#define SVGA_REG_ENABLE_HIDE    (1U << 1)

#define SVGA_MAX_MODES          32
#define SVGA_MAX_VRAM_CAP_BYTES 0xFFF00000ULL

#define SVGA_FIFO_MIN           0
#define SVGA_FIFO_MAX           1
#define SVGA_FIFO_NEXT_CMD      2
#define SVGA_FIFO_STOP          3
#define SVGA_FIFO_CAPABILITIES  4
#define SVGA_FIFO_FLAGS         5
#define SVGA_FIFO_BUSY          290

#define SVGA_FIFO_CAP_GMR2            (1U << 8)
#define SVGA_FIFO_CAP_SCREEN_OBJECT_2 (1U << 9)

#define SVGA_CMD_UPDATE                 1
#define SVGA_CMD_RECT_FILL              2
#define SVGA_CMD_RECT_COPY              3
#define SVGA_CMD_DEFINE_CURSOR          19
#define SVGA_CMD_MOVE_CURSOR            20
#define SVGA_CMD_DEFINE_SCREEN          34
#define SVGA_CMD_DESTROY_SCREEN         35
#define SVGA_CMD_DEFINE_GMRFB           36
#define SVGA_CMD_BLIT_GMRFB_TO_SCREEN   37
#define SVGA_CMD_DEFINE_GMR2            41
#define SVGA_CMD_REMAP_GMR2             42

#define SVGA_3D_CMD_SET_OTABLE_BASE         1091U
#define SVGA_3D_CMD_DESTROY_GB_MOB          1094U
#define SVGA_3D_CMD_DEFINE_GB_SURFACE       1097U
#define SVGA_3D_CMD_DESTROY_GB_SURFACE      1098U
#define SVGA_3D_CMD_BIND_GB_SURFACE         1099U
#define SVGA_3D_CMD_UPDATE_GB_IMAGE         1101U
#define SVGA_3D_CMD_SET_OTABLE_BASE64       1115U
#define SVGA_3D_CMD_DEFINE_GB_SCREENTARGET  1124U
#define SVGA_3D_CMD_DESTROY_GB_SCREENTARGET 1125U
#define SVGA_3D_CMD_BIND_GB_SCREENTARGET    1126U
#define SVGA_3D_CMD_UPDATE_GB_SCREENTARGET  1127U
#define SVGA_3D_CMD_DEFINE_GB_MOB64         1135U

#define SVGA_OTABLE_COUNT           5U
#define SVGA_OTABLE_PAGE_SIZE       4096U
#define SVGA3D_MOBFMT_PT64_0        4U
#define SVGA3D_MOBFMT_PT64_1        5U
#define SVGA3D_MOBFMT_PT64_2        6U
#define SVGA3D_MOBFMT_INVALID       0xFFFFFFFFU
#define SVGA3D_SURFACE_SCREENTARGET (1U << 16)
#define SVGA3D_X8R8G8B8             1U
#define SVGA3D_R5G6B5               3U
#define SVGA_STFLAG_PRIMARY         (1U << 0)
#define SVGA3D_INVALID_ID           0xFFFFFFFFU

#define SVGA_SCREEN_MUST_BE_SET (1U << 0)
#define SVGA_SCREEN_IS_PRIMARY  (1U << 1)
#define SVGA_REMAP_GMR2_PPN32   0U
#define SVGA_REMAP_GMR2_PPN64   (1U << 1)
#define SVGA_SCREEN_OBJ_GMR_ID  0U

#define SVGA_CURSOR_HIDE 0
#define SVGA_CURSOR_SHOW 1

#define SVGA_FIFO_MIN_BYTES 4096U
#define SVGA_FIFO_MIN_SIZE  16384U
#define SVGA_WAIT_LOOPS     500000

static const pdv_pci_device_t* g_pci;
static int g_probed;
static int g_active;
static int g_fifo_ready;
static int g_cursor_ready;
static int g_screen_obj_active;
static int g_stdu_active;
static int g_reg_mmio;
static unsigned int g_io_index;
static unsigned int g_io_data;
static volatile unsigned char* g_reg_mmio_base;
static uint64_t g_fb_phys;
static volatile unsigned char* g_fb_virt;
static pdv_dma_buffer_t g_gmr_dma;
static pdv_dma_buffer_t g_otable_dma;
static pdv_dma_buffer_t g_mob_pt_dma;
static uint64_t g_fifo_phys;
static unsigned int g_fifo_size;
static unsigned int g_fifo_min;
static unsigned int g_fifo_max;
static unsigned int g_vram_size;
static unsigned int g_raw_vram_size;
static unsigned int g_memory_size;
static unsigned int g_fb_size;
static uint64_t g_gb_mem_bytes;
static uint64_t g_max_primary_mem;
static unsigned int g_gmr_max_ids;
static unsigned int g_gmrs_max_pages;
static unsigned int g_screen_max_width;
static unsigned int g_screen_max_height;
static uint64_t g_bar1_size;
static unsigned int g_capabilities;
static unsigned int g_capabilities2;
static unsigned int g_interface_id;
static unsigned int g_max_width;
static unsigned int g_max_height;
static unsigned int g_raw_max_width;
static unsigned int g_raw_max_height;
static int g_boot_width;
static int g_boot_height;
static int g_width;
static int g_height;
static int g_bpp;
static int g_pitch;
static const char* g_name = "VMware SVGA II";
static const char* g_status = "Not detected";
static volatile unsigned int* g_fifo;

static pdv_display_mode_t g_svga_modes[SVGA_MAX_MODES];
static int g_svga_mode_count;

static unsigned int svga_read_reg(unsigned int reg) {
    if (g_reg_mmio && g_reg_mmio_base) {
        mmio_write32((volatile void*)(g_reg_mmio_base), reg);
        return mmio_read32((volatile const void*)(g_reg_mmio_base + 4));
    }
    outl((unsigned short)g_io_index, reg);
    return inl((unsigned short)g_io_data);
}

static void svga_write_reg(unsigned int reg, unsigned int value) {
    if (g_reg_mmio && g_reg_mmio_base) {
        mmio_write32((volatile void*)(g_reg_mmio_base), reg);
        mmio_write32((volatile void*)(g_reg_mmio_base + 4), value);
        return;
    }
    outl((unsigned short)g_io_index, reg);
    outl((unsigned short)g_io_data, value);
}

static void svga_fifo_write(unsigned int index, unsigned int value) {
    mmio_write32((volatile void*)(g_fifo + index), value);
}

static unsigned int svga_fifo_read(unsigned int index) {
    return mmio_read32((volatile const void*)(g_fifo + index));
}

static unsigned int svga_bytes_per_pixel(int bpp) {
    if (bpp <= 8) return 1;
    if (bpp <= 16) return 2;
    if (bpp <= 24) return 3;
    return 4;
}

static void vmware_svga_sync(void) {
    if (!g_active) return;
    svga_write_reg(SVGA_REG_SYNC, 1);
    for (int i = 0; i < SVGA_WAIT_LOOPS; i++) {
        if (!(svga_read_reg(SVGA_REG_BUSY) & 1U)) return;
        outb(0x80, 0);
    }
    g_fifo_ready = 0;
    g_status = "FIFO timeout (linear framebuffer fallback)";
    klog_warn("[SVGA] FIFO sync timeout (busy=%u)",
              svga_read_reg(SVGA_REG_BUSY));
}

static int svga_fifo_emit(const unsigned int* words, unsigned int count) {
    if (!g_fifo_ready || !words || count == 0 || count > 160) return 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        unsigned int next = svga_fifo_read(SVGA_FIFO_NEXT_CMD);
        unsigned int stop = svga_fifo_read(SVGA_FIFO_STOP);

        if (next < g_fifo_min || next > g_fifo_max) next = g_fifo_min;
        if (stop < g_fifo_min || stop > g_fifo_max) stop = g_fifo_min;

        unsigned int free_bytes;
        if (next >= stop) {
            free_bytes = (g_fifo_max - next) + (stop - g_fifo_min);
        } else {
            free_bytes = stop - next;
        }

        if (free_bytes >= count * 4U + 4U) {
            unsigned int cursor = next;
            for (unsigned int i = 0; i < count; i++) {
                mmio_write32((volatile void*)((volatile unsigned char*)g_fifo + cursor),
                             words[i]);
                cursor += 4U;
                if (cursor == g_fifo_max) cursor = g_fifo_min;
            }
            mmio_barrier();
            svga_fifo_write(SVGA_FIFO_NEXT_CMD, cursor);
            mmio_barrier();
            if ((g_capabilities & SVGA_CAP_EXTENDED_FIFO) &&
                g_fifo_min >= (SVGA_FIFO_BUSY + 1U) * 4U &&
                svga_fifo_read(SVGA_FIFO_BUSY) == 0U) {
                svga_fifo_write(SVGA_FIFO_BUSY, 1U);
                svga_write_reg(SVGA_REG_SYNC, 1U);
            }
            return 1;
        }

        vmware_svga_sync();
        if (!g_fifo_ready) return 0;
    }
    return 0;
}

static int svga_normalize_rect(int* x, int* y, int* width, int* height) {
    if (*x < 0) {
        *width += *x;
        *x = 0;
    }
    if (*y < 0) {
        *height += *y;
        *y = 0;
    }
    if (*x + *width > g_width) *width = g_width - *x;
    if (*y + *height > g_height) *height = g_height - *y;
    return *width > 0 && *height > 0 && *x >= 0 && *y >= 0;
}

static int svga_init_fifo(void) {
    g_fifo_ready = 0;
    if (!g_fifo_phys) return 0;

    g_fifo_size = svga_read_reg(SVGA_REG_MEM_SIZE);
    if (g_fifo_size < SVGA_FIFO_MIN_SIZE) {
        klog_warn("[SVGA] FIFO unavailable or too small: %u bytes",
                  g_fifo_size);
        return 0;
    }

    unsigned int map_bytes = g_fifo_size;
    if (map_bytes > 4U * 1024U * 1024U) map_bytes = 4U * 1024U * 1024U;
    if (!g_fifo) {
        g_fifo = (volatile unsigned int*)g_kapi->mmio_map_uc(g_fifo_phys, map_bytes);
        if (!g_fifo) return 0;
    }

    g_fifo_min = SVGA_FIFO_MIN_BYTES;
    if (g_capabilities & SVGA_CAP_EXTENDED_FIFO) {
        unsigned int mem_regs = svga_read_reg(SVGA_REG_MEM_REGS);
        if (mem_regs != 0xFFFFFFFFU && mem_regs > 0U && mem_regs * 4U > g_fifo_min &&
            mem_regs * 4U + 8192U < map_bytes) {
            g_fifo_min = mem_regs * 4U;
        }
    }
    if (g_fifo_min + 8192U >= map_bytes) g_fifo_min = 0;
    g_fifo_max = map_bytes;

    for (unsigned int i = 0; i < g_fifo_min / 4U; i++) {
        svga_fifo_write(i, 0U);
    }
    svga_fifo_write(SVGA_FIFO_MIN, g_fifo_min);
    svga_fifo_write(SVGA_FIFO_MAX, g_fifo_max);
    svga_fifo_write(SVGA_FIFO_NEXT_CMD, g_fifo_min);
    svga_fifo_write(SVGA_FIFO_STOP, g_fifo_min);
    svga_fifo_write(SVGA_FIFO_FLAGS, 0);
    if (g_fifo_min >= (SVGA_FIFO_BUSY + 1U) * 4U) {
        svga_fifo_write(SVGA_FIFO_BUSY, 0U);
    }
    mmio_barrier();
    g_fifo_ready = 1;
    return 1;
}

static void svga_upload_default_palette(void) {
    static const uint8_t vga16[16][3] = {
        {0, 0, 0}, {0, 0, 128}, {0, 128, 0}, {0, 128, 128},
        {128, 0, 0}, {128, 0, 128}, {128, 64, 0}, {192, 192, 192},
        {128, 128, 128}, {0, 0, 255}, {0, 255, 0}, {0, 255, 255},
        {255, 0, 0}, {255, 0, 255}, {255, 255, 0}, {255, 255, 255}
    };
    for (unsigned int i = 0; i < 256U; i++) {
        unsigned int r, g, b;
        if (i < 16U) {
            r = vga16[i][0];
            g = vga16[i][1];
            b = vga16[i][2];
        } else if (i < 32U) {
            r = g = b = (i - 16U) * 17U;
        } else {
            unsigned int val = i - 32U;
            r = (val % 6U) * 51U;
            g = ((val / 6U) % 6U) * 51U;
            b = ((val / 36U) % 6U) * 51U;
        }
        svga_write_reg(SVGA_PALETTE_BASE + i * 3U + 0U, r);
        svga_write_reg(SVGA_PALETTE_BASE + i * 3U + 1U, g);
        svga_write_reg(SVGA_PALETTE_BASE + i * 3U + 2U, b);
    }
}

static unsigned int svga_read_opt_reg(unsigned int reg) {
    unsigned int val = svga_read_reg(reg);
    return (val == 0xFFFFFFFFU) ? 0U : val;
}

static int svga_has_stdu(void) {
    return ((g_capabilities & SVGA_CAP_GBOBJECTS) != 0U) &&
           ((g_capabilities & SVGA_CAP_3D) != 0U) &&
           (g_fifo_phys != 0);
}

static int svga_has_screen_object2(void) {
    return ((g_capabilities & SVGA_CAP_SCREEN_OBJECT_2) != 0U) &&
           ((g_capabilities & SVGA_CAP_GMR2) != 0U) &&
           (g_fifo_phys != 0);
}

static uint64_t svga_effective_fb_limit(void) {
    uint64_t limit = 0;
    if (svga_has_stdu() || svga_has_screen_object2()) {
        if (g_vram_size > 0) limit = (uint64_t)g_vram_size;
        if (g_max_primary_mem >= 4ULL * 1024ULL * 1024ULL &&
            (limit == 0 || g_max_primary_mem < limit)) {
            limit = g_max_primary_mem;
        }
        if (!svga_has_stdu() && g_gmrs_max_pages > 0) {
            uint64_t gmr_bytes = (uint64_t)g_gmrs_max_pages * 4096ULL;
            if (gmr_bytes >= 4ULL * 1024ULL * 1024ULL &&
                (limit == 0 || gmr_bytes < limit)) {
                limit = gmr_bytes;
            }
        }
        return limit;
    }
    if (g_vram_size > 0) limit = (uint64_t)g_vram_size;
    if (g_bar1_size > 0 && (limit == 0 || g_bar1_size < limit)) {
        limit = g_bar1_size;
    }
    return limit;
}

static int vmware_svga_supports_mode(int width, int height, int bpp);

static void svga_rebuild_mode_table(void) {
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
    int preset_cnt = (int)(sizeof(k_preset_w) / sizeof(k_preset_w[0]));
    int cnt = 0;

    for (int i = 0; i < preset_cnt && cnt < SVGA_MAX_MODES; i++) {
        int w = k_preset_w[i];
        int h = k_preset_h[i];
        uint8_t mask = 0;
        for (int b = 0; b < 4; b++) {
            if (vmware_svga_supports_mode(w, h, k_bpps[b])) {
                mask |= pdv_bpp_to_mask(k_bpps[b]);
            }
        }
        if (mask != 0) {
            g_svga_modes[cnt].width = (uint16_t)w;
            g_svga_modes[cnt].height = (uint16_t)h;
            g_svga_modes[cnt].bpp_mask = mask;
            g_svga_modes[cnt].preferred = (w == g_boot_width && h == g_boot_height) ? 1U : 0U;
            g_svga_modes[cnt].reserved = 0U;
            cnt++;
        }
    }

    int extra_w[2] = { g_boot_width, (int)g_max_width };
    int extra_h[2] = { g_boot_height, (int)g_max_height };
    for (int e = 0; e < 2 && cnt < SVGA_MAX_MODES; e++) {
        int ew = extra_w[e];
        int eh = extra_h[e];
        if (ew < PDV_DISPLAY_MIN_WIDTH || eh < PDV_DISPLAY_MIN_HEIGHT) continue;
        if (ew > 16384 || eh > 16384) continue;
        int dup = 0;
        for (int j = 0; j < cnt; j++) {
            if ((int)g_svga_modes[j].width == ew && (int)g_svga_modes[j].height == eh) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;
        uint8_t mask = 0;
        for (int b = 0; b < 4; b++) {
            if (vmware_svga_supports_mode(ew, eh, k_bpps[b])) {
                mask |= pdv_bpp_to_mask(k_bpps[b]);
            }
        }
        if (mask != 0) {
            g_svga_modes[cnt].width = (uint16_t)ew;
            g_svga_modes[cnt].height = (uint16_t)eh;
            g_svga_modes[cnt].bpp_mask = mask;
            g_svga_modes[cnt].preferred = (e == 0) ? 1U : 0U;
            g_svga_modes[cnt].reserved = 0U;
            cnt++;
        }
    }

    g_svga_mode_count = cnt;
}

static void svga_restore_previous_mode(unsigned int enabled,
                                       unsigned int config_done,
                                       unsigned int width,
                                       unsigned int height,
                                       unsigned int bpp) {
    if (!enabled || width == 0 || height == 0 || bpp == 0) {
        svga_write_reg(SVGA_REG_ENABLE, 0);
        return;
    }
    svga_write_reg(SVGA_REG_WIDTH, width);
    svga_write_reg(SVGA_REG_HEIGHT, height);
    svga_write_reg(SVGA_REG_DEPTH, (bpp == 32) ? 24U : bpp);
    svga_write_reg(SVGA_REG_BPP, bpp);
    svga_write_reg(SVGA_REG_ENABLE, 1);
    svga_write_reg(SVGA_REG_CONFIG_DONE, config_done);
}

static int vmware_svga_probe_hw(void) {
    g_probed = 1;
    g_active = 0;
    g_fifo_ready = 0;
    g_cursor_ready = 0;
    g_screen_obj_active = 0;
    g_stdu_active = 0;
    g_pci = 0;
    g_fifo = 0;
    g_capabilities2 = 0;
    g_gb_mem_bytes = 0;
    g_max_primary_mem = 0;
    g_gmr_max_ids = 0;
    g_gmrs_max_pages = 0;
    g_screen_max_width = 0;
    g_screen_max_height = 0;
    g_status = "SVGA device not found";

    g_pci = g_kapi->pci_find_by_id(VMWARE_PCI_VENDOR_ID, VMWARE_SVGA_II_ID);
    if (!g_pci) {
        g_pci = g_kapi->pci_find_by_id(VMWARE_PCI_VENDOR_ID, VMWARE_SVGA_II_FUSION_ID);
    }
    if (!g_pci) {
        g_pci = g_kapi->pci_find_by_id(VMWARE_PCI_VENDOR_ID, VMWARE_SVGA_ADAPTER_ID);
    }
    if (!g_pci) return 0;

    if (g_pci->device == VMWARE_SVGA_II_FUSION_ID) {
        g_name = "VMware SVGA II (Fusion)";
    } else if (g_pci->device == VMWARE_SVGA_ADAPTER_ID) {
        g_name = "VMware SVGA Adapter";
    } else {
        g_name = "VMware SVGA II";
    }

    g_reg_mmio = 0;
    g_io_index = 0;
    g_io_data = 0;
    g_reg_mmio_base = 0;
    if (g_pci->bar[0].valid && g_pci->bar[0].is_io) {
        g_io_index = (unsigned int)g_pci->bar[0].address;
        g_io_data = g_io_index + 1;
    } else if (g_pci->bar[0].valid && !g_pci->bar[0].is_io) {
        g_reg_mmio = 1;
        g_reg_mmio_base = (volatile unsigned char*)g_kapi->mmio_map_uc(
            g_pci->bar[0].address, 0x1000);
        if (!g_reg_mmio_base) return 0;
    } else {
        g_status = "SVGA BAR0 invalid";
        return 0;
    }

    if (!g_pci->bar[1].valid || g_pci->bar[1].is_io) {
        g_status = "SVGA BAR1 framebuffer invalid";
        return 0;
    }

    g_kapi->pci_enable_io(g_pci);
    g_kapi->pci_enable_memory(g_pci);
    g_kapi->pci_enable_bus_master(g_pci);

    svga_write_reg(SVGA_REG_ID, SVGA_ID_2);
    g_interface_id = svga_read_reg(SVGA_REG_ID);
    if ((g_interface_id & 0xFFFF0000U) != SVGA_ID_0) {
        svga_write_reg(SVGA_REG_ID, SVGA_ID_1);
        g_interface_id = svga_read_reg(SVGA_REG_ID);
    }
    if ((g_interface_id & 0xFFFF0000U) != SVGA_ID_0) {
        klog_warn("[SVGA] unsupported interface ID 0x%X", g_interface_id);
        g_status = "Unsupported SVGA interface";
        return 0;
    }

    svga_write_reg(SVGA_REG_GUEST_ID, GUEST_OS_OTHER);
    g_capabilities = svga_read_reg(SVGA_REG_CAPABILITIES);
    g_raw_vram_size = svga_read_reg(SVGA_REG_VRAM_SIZE);
    g_fb_size = svga_read_reg(SVGA_REG_FB_SIZE);
    g_bar1_size = g_pci->bar[1].size;
    if (g_pci->bar[2].valid && !g_pci->bar[2].is_io) {
        g_fifo_phys = g_pci->bar[2].address;
    }

    if (g_capabilities & SVGA_CAP_CAP2_REGISTER) {
        g_capabilities2 = svga_read_opt_reg(SVGA_REG_CAP2);
    }
    if ((g_capabilities & SVGA_CAP_CAP2_REGISTER) &&
        (g_capabilities2 & SVGA_CAP2_GB_MEMSIZE_2)) {
        unsigned int kb = svga_read_opt_reg(SVGA_REG_GBOBJECT_MEM_SIZE_KB);
        if (kb > 0) g_gb_mem_bytes = (uint64_t)kb * 1024ULL;
    } else if (g_capabilities & SVGA_CAP_GBOBJECTS) {
        unsigned int kb = svga_read_opt_reg(SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB);
        if (kb > 0) g_gb_mem_bytes = (uint64_t)kb * 1024ULL;
    }

    g_memory_size = 0;
    if ((g_capabilities & (SVGA_CAP_GMR | SVGA_CAP_GMR2 |
                           SVGA_CAP_SCREEN_OBJECT_2 | SVGA_CAP_GBOBJECTS)) != 0U) {
        g_memory_size = svga_read_opt_reg(SVGA_REG_MEMORY_SIZE);
        g_max_primary_mem = (uint64_t)svga_read_opt_reg(SVGA_REG_MAX_PRIMARY_MEM);
    }
    if (g_capabilities & (SVGA_CAP_GMR | SVGA_CAP_GMR2)) {
        g_gmr_max_ids = svga_read_opt_reg(SVGA_REG_GMR_MAX_IDS);
        g_gmrs_max_pages = svga_read_opt_reg(SVGA_REG_GMRS_MAX_PAGES);
    }
    if (g_capabilities & (SVGA_CAP_SCREEN_OBJECT_2 | SVGA_CAP_GBOBJECTS)) {
        g_screen_max_width = svga_read_opt_reg(SVGA_REG_SCREENTARGET_MAX_WIDTH);
        g_screen_max_height = svga_read_opt_reg(SVGA_REG_SCREENTARGET_MAX_HEIGHT);
    }

    /*
     * In VMware 3D / GBObjects mode, SVGA_REG_VRAM_SIZE often stays fixed at
     * 4 MB (legacy GFB compatibility window) while configured 3D VRAM is
     * exposed via GBOBJECT_MEM_SIZE_KB / MAX_PRIMARY_MEM / BAR1.
     */
    g_vram_size = g_raw_vram_size;
    if (g_capabilities & (SVGA_CAP_3D | SVGA_CAP_GBOBJECTS)) {
        uint64_t usable = (uint64_t)g_raw_vram_size;
        if (g_gb_mem_bytes > usable) usable = g_gb_mem_bytes;
        if (g_max_primary_mem > usable) usable = g_max_primary_mem;
        if ((uint64_t)g_memory_size > usable) usable = (uint64_t)g_memory_size;
        if (g_bar1_size > usable) usable = g_bar1_size;
        if (usable > SVGA_MAX_VRAM_CAP_BYTES) usable = SVGA_MAX_VRAM_CAP_BYTES;
        g_vram_size = (unsigned int)usable;
    } else if (g_memory_size > g_raw_vram_size && g_bar1_size > (uint64_t)g_raw_vram_size) {
        uint64_t usable = (uint64_t)g_memory_size;
        if (g_bar1_size > 0 && g_bar1_size < usable) {
            usable = g_bar1_size;
        }
        if (usable > SVGA_MAX_VRAM_CAP_BYTES) usable = SVGA_MAX_VRAM_CAP_BYTES;
        g_vram_size = (unsigned int)usable;
    } else if (g_vram_size == 0 && g_bar1_size > 0) {
        uint64_t usable = g_bar1_size;
        if (usable > SVGA_MAX_VRAM_CAP_BYTES) usable = SVGA_MAX_VRAM_CAP_BYTES;
        g_vram_size = (unsigned int)usable;
    }

    g_raw_max_width = svga_read_reg(SVGA_REG_MAX_WIDTH);
    g_raw_max_height = svga_read_reg(SVGA_REG_MAX_HEIGHT);
    g_max_width = g_raw_max_width;
    g_max_height = g_raw_max_height;
    if (svga_has_stdu() || svga_has_screen_object2()) {
        if (g_screen_max_width > g_max_width && g_screen_max_width <= 16384U) {
            g_max_width = g_screen_max_width;
        }
        if (g_screen_max_height > g_max_height && g_screen_max_height <= 16384U) {
            g_max_height = g_screen_max_height;
        }
    }
    if (g_max_width < PDV_DISPLAY_MIN_WIDTH) g_max_width = 1920;
    if (g_max_height < PDV_DISPLAY_MIN_HEIGHT) g_max_height = 1200;
    if (g_vram_size >= 32U * 1024U * 1024U) {
        if (g_max_width < 3840U) g_max_width = 3840U;
        if (g_max_height < 2160U) g_max_height = 2160U;
    } else if (g_vram_size >= 16U * 1024U * 1024U) {
        if (g_max_width < 2560U) g_max_width = 2560U;
        if (g_max_height < 1600U) g_max_height = 1600U;
    } else if (g_vram_size >= 4U * 1024U * 1024U) {
        if (g_max_width < 1920U) g_max_width = 1920U;
        if (g_max_height < 1200U) g_max_height = 1200U;
    }

    pdv_fb_surface_t boot_surf;
    g_kapi->fb_get_surface(&boot_surf);
    g_boot_width = (int)boot_surf.width;
    g_boot_height = (int)boot_surf.height;
    if (g_boot_width > (int)g_max_width && g_boot_width <= 16384) {
        g_max_width = (unsigned int)g_boot_width;
    }
    if (g_boot_height > (int)g_max_height && g_boot_height <= 16384) {
        g_max_height = (unsigned int)g_boot_height;
    }

    g_probed = 1;
    svga_rebuild_mode_table();
    g_status = "Detected, mode not configured";
    klog_info("[SVGA] %s id=0x%X caps=0x%X cap2=0x%X raw_vram=%uKB gb=%lluKB prim=%lluKB eff_vram=%uKB bar1=%uKB raw_max=%ux%u eff_max=%ux%u modes=%d",
              g_name, g_interface_id, g_capabilities, g_capabilities2,
              g_raw_vram_size / 1024U,
              (unsigned long long)(g_gb_mem_bytes / 1024ULL),
              (unsigned long long)(g_max_primary_mem / 1024ULL),
              g_vram_size / 1024U,
              (unsigned int)(g_bar1_size / 1024ULL),
              g_raw_max_width, g_raw_max_height,
              g_max_width, g_max_height, g_svga_mode_count);
    return 1;
}

static void svga_apply_surface(int width, int height, int hw_bpp,
                               unsigned int pitch, uint64_t fb_phys,
                               volatile unsigned char* fb_virt,
                               int check_green_mask) {
    pdv_fb_surface_t surf;
    g_kapi->fb_get_surface(&surf);
    surf.framebuffer_phys = fb_phys;
    surf.framebuffer_virt = (void*)(uintptr_t)fb_virt;
    surf.width = (uint32_t)width;
    surf.height = (uint32_t)height;
    surf.pitch = (uint32_t)pitch;
    surf.bpp = (uint32_t)hw_bpp;
    if (hw_bpp == 8) {
        surf.red_mask_size = 0;
        surf.red_mask_shift = 0;
        surf.green_mask_size = 0;
        surf.green_mask_shift = 0;
        surf.blue_mask_size = 0;
        surf.blue_mask_shift = 0;
        surf.format = PDV_PIXEL_FORMAT_UNKNOWN;
    } else if (hw_bpp == 16) {
        unsigned int gm = check_green_mask ? svga_read_reg(SVGA_REG_GREEN_MASK) : 0x07E0U;
        if (gm == 0x03E0U) {
            surf.red_mask_size = 5;
            surf.red_mask_shift = 10;
            surf.green_mask_size = 5;
            surf.green_mask_shift = 5;
            surf.blue_mask_size = 5;
            surf.blue_mask_shift = 0;
            surf.format = PDV_PIXEL_FORMAT_UNKNOWN;
        } else {
            surf.red_mask_size = 5;
            surf.red_mask_shift = 11;
            surf.green_mask_size = 6;
            surf.green_mask_shift = 5;
            surf.blue_mask_size = 5;
            surf.blue_mask_shift = 0;
            surf.format = PDV_PIXEL_FORMAT_RGB565;
        }
    } else {
        surf.red_mask_size = 8;
        surf.red_mask_shift = 16;
        surf.green_mask_size = 8;
        surf.green_mask_shift = 8;
        surf.blue_mask_size = 8;
        surf.blue_mask_shift = 0;
        surf.format = PDV_PIXEL_FORMAT_RGB888;
    }
    g_kapi->fb_set_surface(&surf, (unsigned char*)(uintptr_t)fb_virt);
}

static void svga_teardown_screen_object(void) {
    if (!g_screen_obj_active || !g_fifo_ready) return;
    unsigned int destroy_cmd[2];
    destroy_cmd[0] = SVGA_CMD_DESTROY_SCREEN;
    destroy_cmd[1] = 0U;
    (void)svga_fifo_emit(destroy_cmd, 2U);

    unsigned int undef_gmr[3];
    undef_gmr[0] = SVGA_CMD_DEFINE_GMR2;
    undef_gmr[1] = SVGA_SCREEN_OBJ_GMR_ID;
    undef_gmr[2] = 0U;
    (void)svga_fifo_emit(undef_gmr, 3U);

    g_active = 1;
    vmware_svga_sync();
    g_active = 0;
    g_screen_obj_active = 0;
}

static void svga_zero_dma(const pdv_dma_buffer_t* dma, size_t bytes) {
    if (!dma || !pdv_dma_is_valid(dma) || !dma->virt || bytes == 0U) return;
    volatile uint64_t* q = (volatile uint64_t*)dma->virt;
    size_t qcnt = bytes / 8U;
    for (size_t i = 0U; i < qcnt; i++) {
        q[i] = 0ULL;
    }
    volatile unsigned char* b = (volatile unsigned char*)dma->virt;
    for (size_t i = qcnt * 8U; i < bytes; i++) {
        b[i] = 0U;
    }
}

static void svga_teardown_stdu(void) {
    if (g_stdu_active && g_fifo_ready) {
        unsigned int unbind_st[6];
        unbind_st[0] = SVGA_3D_CMD_BIND_GB_SCREENTARGET;
        unbind_st[1] = 16U;
        unbind_st[2] = 0U;
        unbind_st[3] = SVGA3D_INVALID_ID;
        unbind_st[4] = 0U;
        unbind_st[5] = 0U;
        (void)svga_fifo_emit(unbind_st, 6U);

        unsigned int destroy_st[3];
        destroy_st[0] = SVGA_3D_CMD_DESTROY_GB_SCREENTARGET;
        destroy_st[1] = 4U;
        destroy_st[2] = 0U;
        (void)svga_fifo_emit(destroy_st, 3U);

        unsigned int destroy_surf[3];
        destroy_surf[0] = SVGA_3D_CMD_DESTROY_GB_SURFACE;
        destroy_surf[1] = 4U;
        destroy_surf[2] = 0U;
        (void)svga_fifo_emit(destroy_surf, 3U);

        unsigned int destroy_mob[3];
        destroy_mob[0] = SVGA_3D_CMD_DESTROY_GB_MOB;
        destroy_mob[1] = 4U;
        destroy_mob[2] = SVGA_SCREEN_OBJ_GMR_ID;
        (void)svga_fifo_emit(destroy_mob, 3U);

        for (unsigned int t = 0U; t < SVGA_OTABLE_COUNT; t++) {
            unsigned int clear_ot[7];
            clear_ot[0] = SVGA_3D_CMD_SET_OTABLE_BASE;
            clear_ot[1] = 20U;
            clear_ot[2] = t;
            clear_ot[3] = 0U;
            clear_ot[4] = 0U;
            clear_ot[5] = 0U;
            clear_ot[6] = SVGA3D_MOBFMT_INVALID;
            (void)svga_fifo_emit(clear_ot, 7U);
        }

        g_active = 1;
        vmware_svga_sync();
        g_active = 0;
    }
    if (pdv_dma_is_valid(&g_mob_pt_dma)) {
        g_kapi->dma_free(&g_mob_pt_dma);
    }
    if (pdv_dma_is_valid(&g_otable_dma)) {
        g_kapi->dma_free(&g_otable_dma);
    }
    g_stdu_active = 0;
}

static int svga_set_mode_stdu(int width, int height, int bpp,
                              const int* try_hw, int try_cnt) {
    if (!svga_has_stdu() || try_cnt <= 0) return 0;

    int cand_bpp = 0;
    for (int i = 0; i < try_cnt; i++) {
        if (try_hw[i] == 32 || try_hw[i] == 16) {
            cand_bpp = try_hw[i];
            break;
        }
    }
    if (cand_bpp == 0) cand_bpp = 32;

    unsigned int pitch = (unsigned int)width * svga_bytes_per_pixel(cand_bpp);
    size_t fb_bytes = (size_t)pitch * (size_t)(unsigned int)height;
    unsigned int num_pages = (unsigned int)((fb_bytes + 4095U) / 4096U);
    if (num_pages == 0U) return 0;

    unsigned int pt_pages = 0U;
    unsigned int pt_depth = SVGA3D_MOBFMT_PT64_0;
    unsigned int num_l1 = 0U;
    if (num_pages > 1U) {
        if (num_pages <= 512U) {
            pt_pages = 1U;
            pt_depth = SVGA3D_MOBFMT_PT64_1;
        } else {
            num_l1 = (num_pages + 511U) / 512U;
            if (num_l1 > 512U) return 0;
            pt_pages = num_l1 + 1U;
            pt_depth = SVGA3D_MOBFMT_PT64_2;
        }
    }

    pdv_dma_buffer_t new_dma = {0};
    if (g_kapi->dma_alloc((size_t)num_pages * 4096U, PDV_DMA_BIT_MASK(64),
                          PDV_PAGE_SIZE, &new_dma) != 0 ||
        !pdv_dma_is_valid(&new_dma)) {
        return 0;
    }

    pdv_dma_buffer_t otable_dma = {0};
    size_t otable_bytes = (size_t)SVGA_OTABLE_COUNT * SVGA_OTABLE_PAGE_SIZE;
    if (g_kapi->dma_alloc(otable_bytes, PDV_DMA_BIT_MASK(64),
                          PDV_PAGE_SIZE, &otable_dma) != 0 ||
        !pdv_dma_is_valid(&otable_dma)) {
        g_kapi->dma_free(&new_dma);
        return 0;
    }

    pdv_dma_buffer_t pt_dma = {0};
    if (pt_pages > 0U) {
        if (g_kapi->dma_alloc((size_t)pt_pages * 4096U, PDV_DMA_BIT_MASK(64),
                              PDV_PAGE_SIZE, &pt_dma) != 0 ||
            !pdv_dma_is_valid(&pt_dma)) {
            g_kapi->dma_free(&otable_dma);
            g_kapi->dma_free(&new_dma);
            return 0;
        }
        svga_zero_dma(&pt_dma, (size_t)pt_pages * 4096U);
    }

    svga_zero_dma(&new_dma, (size_t)num_pages * 4096U);
    svga_zero_dma(&otable_dma, otable_bytes);

    uint64_t fb_ppn_base = (uint64_t)(new_dma.phys >> 12);
    uint64_t pt_root_ppn = fb_ppn_base;
    if (pt_pages > 0U) {
        volatile uint64_t* pt_entries = (volatile uint64_t*)pt_dma.virt;
        for (unsigned int p = 0U; p < num_pages; p++) {
            pt_entries[p] = fb_ppn_base + (uint64_t)p;
        }
        uint64_t pt_ppn_base = (uint64_t)(pt_dma.phys >> 12);
        if (pt_depth == SVGA3D_MOBFMT_PT64_1) {
            pt_root_ppn = pt_ppn_base;
        } else {
            size_t l0_off = (size_t)num_l1 * 512U;
            for (unsigned int k = 0U; k < num_l1; k++) {
                pt_entries[l0_off + k] = pt_ppn_base + (uint64_t)k;
            }
            pt_root_ppn = pt_ppn_base + (uint64_t)num_l1;
        }
        g_kapi->dma_sync_for_device(&pt_dma);
    }
    g_kapi->dma_sync_for_device(&otable_dma);

    /* Switch legacy SVGA scanout off cleanly before bringing up STDU + OTables. */
    unsigned int safe_w = (g_raw_max_width >= 640U && g_raw_max_width < 800U) ? g_raw_max_width : 640U;
    unsigned int safe_h = (g_raw_max_height >= 480U && g_raw_max_height < 600U) ? g_raw_max_height : 480U;
    svga_write_reg(SVGA_REG_CONFIG_DONE, 0U);
    svga_write_reg(SVGA_REG_ENABLE, 0U);
    svga_write_reg(SVGA_REG_WIDTH, safe_w);
    svga_write_reg(SVGA_REG_HEIGHT, safe_h);
    svga_write_reg(SVGA_REG_DEPTH, 24U);
    svga_write_reg(SVGA_REG_BPP, 32U);
    svga_write_reg(SVGA_REG_TRACES, 0U);
    svga_write_reg(SVGA_REG_ENABLE, SVGA_REG_ENABLE_ENABLE | SVGA_REG_ENABLE_HIDE);

    if (!svga_read_reg(SVGA_REG_ENABLE) || !svga_init_fifo()) {
        svga_write_reg(SVGA_REG_ENABLE, 0U);
        if (pdv_dma_is_valid(&pt_dma)) g_kapi->dma_free(&pt_dma);
        g_kapi->dma_free(&otable_dma);
        g_kapi->dma_free(&new_dma);
        return 0;
    }

    svga_write_reg(SVGA_REG_CONFIG_DONE, 1U);
    g_active = 1;

    int ok = 1;
    uint64_t ot_ppn_base = (uint64_t)(otable_dma.phys >> 12);
    for (unsigned int t = 0U; ok && t < SVGA_OTABLE_COUNT; t++) {
        uint64_t ot_ppn = ot_ppn_base + (uint64_t)t;
        unsigned int set_ot[8];
        set_ot[0] = SVGA_3D_CMD_SET_OTABLE_BASE64;
        set_ot[1] = 24U;
        set_ot[2] = t;
        set_ot[3] = (unsigned int)(ot_ppn & 0xFFFFFFFFULL);
        set_ot[4] = (unsigned int)(ot_ppn >> 32);
        set_ot[5] = SVGA_OTABLE_PAGE_SIZE;
        set_ot[6] = 0U;
        set_ot[7] = SVGA3D_MOBFMT_PT64_0;
        ok = svga_fifo_emit(set_ot, 8U);
    }

    if (ok) {
        unsigned int def_mob[7];
        def_mob[0] = SVGA_3D_CMD_DEFINE_GB_MOB64;
        def_mob[1] = 20U;
        def_mob[2] = SVGA_SCREEN_OBJ_GMR_ID;
        def_mob[3] = pt_depth;
        def_mob[4] = (unsigned int)(pt_root_ppn & 0xFFFFFFFFULL);
        def_mob[5] = (unsigned int)(pt_root_ppn >> 32);
        def_mob[6] = num_pages * 4096U;
        ok = svga_fifo_emit(def_mob, 7U);
    }

    if (ok) {
        unsigned int fmt = (cand_bpp == 16) ? SVGA3D_R5G6B5 : SVGA3D_X8R8G8B8;
        unsigned int def_surf[11];
        def_surf[0]  = SVGA_3D_CMD_DEFINE_GB_SURFACE;
        def_surf[1]  = 36U;
        def_surf[2]  = 0U; /* sid */
        def_surf[3]  = SVGA3D_SURFACE_SCREENTARGET;
        def_surf[4]  = fmt;
        def_surf[5]  = 1U; /* numMipLevels */
        def_surf[6]  = 0U; /* multisampleCount */
        def_surf[7]  = 0U; /* autogenFilter */
        def_surf[8]  = (unsigned int)width;
        def_surf[9]  = (unsigned int)height;
        def_surf[10] = 1U; /* depth */
        ok = svga_fifo_emit(def_surf, 11U);
    }

    if (ok) {
        unsigned int bind_surf[4];
        bind_surf[0] = SVGA_3D_CMD_BIND_GB_SURFACE;
        bind_surf[1] = 8U;
        bind_surf[2] = 0U; /* sid */
        bind_surf[3] = SVGA_SCREEN_OBJ_GMR_ID;
        ok = svga_fifo_emit(bind_surf, 4U);
    }

    if (ok) {
        unsigned int def_st[9];
        def_st[0] = SVGA_3D_CMD_DEFINE_GB_SCREENTARGET;
        def_st[1] = 28U;
        def_st[2] = 0U; /* stid */
        def_st[3] = (unsigned int)width;
        def_st[4] = (unsigned int)height;
        def_st[5] = 0U; /* xRoot */
        def_st[6] = 0U; /* yRoot */
        def_st[7] = SVGA_STFLAG_PRIMARY;
        def_st[8] = 0U; /* dpi */
        ok = svga_fifo_emit(def_st, 9U);
    }

    if (ok) {
        unsigned int bind_st[6];
        bind_st[0] = SVGA_3D_CMD_BIND_GB_SCREENTARGET;
        bind_st[1] = 16U;
        bind_st[2] = 0U; /* stid */
        bind_st[3] = 0U; /* sid */
        bind_st[4] = 0U; /* face */
        bind_st[5] = 0U; /* mipmap */
        ok = svga_fifo_emit(bind_st, 6U);
    }

    if (ok) {
        g_kapi->dma_sync_for_device(&new_dma);
        unsigned int upd[18];
        upd[0]  = SVGA_3D_CMD_UPDATE_GB_IMAGE;
        upd[1]  = 36U;
        upd[2]  = 0U;
        upd[3]  = 0U;
        upd[4]  = 0U;
        upd[5]  = 0U;
        upd[6]  = 0U;
        upd[7]  = 0U;
        upd[8]  = (unsigned int)width;
        upd[9]  = (unsigned int)height;
        upd[10] = 1U;

        upd[11] = SVGA_3D_CMD_UPDATE_GB_SCREENTARGET;
        upd[12] = 20U;
        upd[13] = 0U;
        upd[14] = 0U;
        upd[15] = 0U;
        upd[16] = (unsigned int)width;
        upd[17] = (unsigned int)height;
        ok = svga_fifo_emit(upd, 18U);
        vmware_svga_sync();
    }

    if (!ok || !g_fifo_ready) {
        g_stdu_active = 1;
        svga_teardown_stdu();
        g_active = 0;
        svga_write_reg(SVGA_REG_CONFIG_DONE, 0U);
        svga_write_reg(SVGA_REG_ENABLE, 0U);
        if (pdv_dma_is_valid(&pt_dma)) g_kapi->dma_free(&pt_dma);
        g_kapi->dma_free(&otable_dma);
        g_kapi->dma_free(&new_dma);
        return 0;
    }

    if (pdv_dma_is_valid(&g_mob_pt_dma)) g_kapi->dma_free(&g_mob_pt_dma);
    if (pdv_dma_is_valid(&g_otable_dma)) g_kapi->dma_free(&g_otable_dma);
    if (pdv_dma_is_valid(&g_gmr_dma)) g_kapi->dma_free(&g_gmr_dma);

    g_gmr_dma = new_dma;
    g_otable_dma = otable_dma;
    g_mob_pt_dma = pt_dma;
    g_stdu_active = 1;
    g_screen_obj_active = 0;
    g_fb_phys = new_dma.phys;
    g_fb_virt = (volatile unsigned char*)new_dma.virt;
    g_width = width;
    g_height = height;
    g_bpp = cand_bpp;
    g_pitch = (int)pitch;
    g_active = 1;

    svga_apply_surface(width, height, cand_bpp, pitch,
                       new_dma.phys, (volatile unsigned char*)new_dma.virt, 0);
    svga_write_reg(SVGA_REG_CURSOR_ON, SVGA_CURSOR_HIDE);
    g_status = "3D-capable profile: STDU + GBObjects active";

    klog_info("[SVGA] mode %dx%dx%d (STDU %dbpp) pitch=%u mob=0x%llx",
              width, height, bpp, cand_bpp, pitch,
              (unsigned long long)new_dma.phys);
    return 1;
}

static int svga_set_mode_screen_object(int width, int height, int bpp,
                                       const int* try_hw, int try_cnt) {
    if (!svga_has_screen_object2() || try_cnt <= 0) return 0;

    int cand_bpp = try_hw[0];
    if (cand_bpp == 8) {
        for (int i = 0; i < try_cnt; i++) {
            if (try_hw[i] == 32 || try_hw[i] == 16 || try_hw[i] == 24) {
                cand_bpp = try_hw[i];
                break;
            }
        }
    }

    unsigned int pitch = (unsigned int)width * svga_bytes_per_pixel(cand_bpp);
    size_t fb_bytes = (size_t)pitch * (size_t)(unsigned int)height;
    unsigned int num_pages = (unsigned int)((fb_bytes + 4095U) / 4096U);
    if (num_pages == 0U) return 0;
    if (g_gmr_max_ids > 0U && SVGA_SCREEN_OBJ_GMR_ID >= g_gmr_max_ids) return 0;
    if (g_gmrs_max_pages > 0U && num_pages > g_gmrs_max_pages) return 0;

    pdv_dma_buffer_t new_dma = {0};
    if (g_kapi->dma_alloc((size_t)num_pages * 4096U, PDV_DMA_BIT_MASK(32),
                          PDV_PAGE_SIZE, &new_dma) != 0 ||
        !pdv_dma_is_valid(&new_dma)) {
        if (g_kapi->dma_alloc((size_t)num_pages * 4096U, PDV_DMA_BIT_MASK(64),
                              PDV_PAGE_SIZE, &new_dma) != 0 ||
            !pdv_dma_is_valid(&new_dma)) {
            return 0;
        }
    }

    /* Reset mode registers to a small valid size so SVGA_REG_ENABLE = 1 holds. */
    unsigned int safe_w = (g_raw_max_width >= 640U && g_raw_max_width < 800U) ? g_raw_max_width : 640U;
    unsigned int safe_h = (g_raw_max_height >= 480U && g_raw_max_height < 600U) ? g_raw_max_height : 480U;
    svga_zero_dma(&new_dma, (size_t)num_pages * 4096U);
    svga_write_reg(SVGA_REG_CONFIG_DONE, 0);
    svga_write_reg(SVGA_REG_ENABLE, 0);
    svga_write_reg(SVGA_REG_WIDTH, safe_w);
    svga_write_reg(SVGA_REG_HEIGHT, safe_h);
    svga_write_reg(SVGA_REG_DEPTH, 24U);
    svga_write_reg(SVGA_REG_BPP, 32U);
    svga_write_reg(SVGA_REG_TRACES, 0U);
    svga_write_reg(SVGA_REG_ENABLE, SVGA_REG_ENABLE_ENABLE | SVGA_REG_ENABLE_HIDE);

    if (!svga_read_reg(SVGA_REG_ENABLE) || !svga_init_fifo()) {
        svga_write_reg(SVGA_REG_ENABLE, 0);
        g_kapi->dma_free(&new_dma);
        return 0;
    }

    svga_write_reg(SVGA_REG_CONFIG_DONE, 1U);
    unsigned int fifo_caps = svga_fifo_read(SVGA_FIFO_CAPABILITIES);
    if ((fifo_caps & (SVGA_FIFO_CAP_SCREEN_OBJECT_2 | SVGA_FIFO_CAP_GMR2)) !=
        (SVGA_FIFO_CAP_SCREEN_OBJECT_2 | SVGA_FIFO_CAP_GMR2)) {
        svga_write_reg(SVGA_REG_CONFIG_DONE, 0);
        svga_write_reg(SVGA_REG_ENABLE, 0);
        g_kapi->dma_free(&new_dma);
        return 0;
    }

    g_active = 1;

    unsigned int def_gmr[3];
    def_gmr[0] = SVGA_CMD_DEFINE_GMR2;
    def_gmr[1] = SVGA_SCREEN_OBJ_GMR_ID;
    def_gmr[2] = num_pages;
    int ok = svga_fifo_emit(def_gmr, 3U);

    uint64_t base_ppn = new_dma.phys >> 12;
    int use_ppn64 = ((base_ppn + (uint64_t)num_pages - 1ULL) > 0xFFFFFFFFULL);
    unsigned int remap_cmd[160];
    for (unsigned int off = 0; ok && off < num_pages;) {
        unsigned int chunk = num_pages - off;
        if (chunk > 64U) chunk = 64U;
        remap_cmd[0] = SVGA_CMD_REMAP_GMR2;
        remap_cmd[1] = SVGA_SCREEN_OBJ_GMR_ID;
        remap_cmd[2] = use_ppn64 ? SVGA_REMAP_GMR2_PPN64 : SVGA_REMAP_GMR2_PPN32;
        remap_cmd[3] = off;
        remap_cmd[4] = chunk;
        unsigned int w = 5U;
        for (unsigned int p = 0; p < chunk; p++) {
            uint64_t ppn = base_ppn + (uint64_t)off + (uint64_t)p;
            if (use_ppn64) {
                remap_cmd[w++] = (unsigned int)(ppn & 0xFFFFFFFFULL);
                remap_cmd[w++] = (unsigned int)(ppn >> 32);
            } else {
                remap_cmd[w++] = (unsigned int)ppn;
            }
        }
        ok = svga_fifo_emit(remap_cmd, w);
        off += chunk;
    }

    if (ok) {
        unsigned int def_scr[12];
        def_scr[0] = SVGA_CMD_DEFINE_SCREEN;
        def_scr[1] = 44U; /* sizeof(SVGAScreenObject) */
        def_scr[2] = 0U;  /* screenId */
        def_scr[3] = SVGA_SCREEN_MUST_BE_SET | SVGA_SCREEN_IS_PRIMARY;
        def_scr[4] = (unsigned int)width;
        def_scr[5] = (unsigned int)height;
        def_scr[6] = 0U;  /* root.x */
        def_scr[7] = 0U;  /* root.y */
        def_scr[8] = SVGA_SCREEN_OBJ_GMR_ID;
        def_scr[9] = 0U;  /* backingStore.ptr.offset */
        def_scr[10] = pitch;
        def_scr[11] = 0U; /* cloneCount */
        ok = svga_fifo_emit(def_scr, 12U);
    }

    if (ok) {
        unsigned int color_depth = (cand_bpp == 32) ? 24U : (unsigned int)cand_bpp;
        unsigned int def_gmrfb[5];
        def_gmrfb[0] = SVGA_CMD_DEFINE_GMRFB;
        def_gmrfb[1] = SVGA_SCREEN_OBJ_GMR_ID;
        def_gmrfb[2] = 0U;
        def_gmrfb[3] = pitch;
        def_gmrfb[4] = ((color_depth & 0xFFU) << 8) | ((unsigned int)cand_bpp & 0xFFU);
        ok = svga_fifo_emit(def_gmrfb, 5U);
    }

    if (ok) {
        g_kapi->dma_sync_for_device(&new_dma);
        unsigned int blit[8];
        blit[0] = SVGA_CMD_BLIT_GMRFB_TO_SCREEN;
        blit[1] = 0U;
        blit[2] = 0U;
        blit[3] = 0U;
        blit[4] = 0U;
        blit[5] = (unsigned int)width;
        blit[6] = (unsigned int)height;
        blit[7] = 0U;
        ok = svga_fifo_emit(blit, 8U);
        vmware_svga_sync();
    }

    if (!ok || !g_fifo_ready) {
        g_active = 0;
        svga_write_reg(SVGA_REG_CONFIG_DONE, 0);
        svga_write_reg(SVGA_REG_ENABLE, 0);
        g_kapi->dma_free(&new_dma);
        return 0;
    }

    if (pdv_dma_is_valid(&g_gmr_dma)) {
        g_kapi->dma_free(&g_gmr_dma);
    }
    g_gmr_dma = new_dma;
    g_screen_obj_active = 1;
    g_fb_phys = new_dma.phys;
    g_fb_virt = (volatile unsigned char*)new_dma.virt;
    g_width = width;
    g_height = height;
    g_bpp = cand_bpp;
    g_pitch = (int)pitch;
    g_active = 1;

    if (cand_bpp == 8) {
        svga_upload_default_palette();
    }
    svga_apply_surface(width, height, cand_bpp, pitch,
                       new_dma.phys, (volatile unsigned char*)new_dma.virt, 0);
    svga_write_reg(SVGA_REG_CURSOR_ON, SVGA_CURSOR_HIDE);
    g_status = "3D-capable profile: ScreenObject2 + GMR2 active";

    klog_info("[SVGA] mode %dx%dx%d (ScreenObject2 %dbpp) pitch=%u gmr=0x%llx",
              width, height, bpp, cand_bpp, pitch,
              (unsigned long long)new_dma.phys);
    return 1;
}

static int vmware_svga_set_mode(int width, int height, int bpp) {
    if (!g_probed && !vmware_svga_probe_hw()) return 0;
    if (!vmware_svga_supports_mode(width, height, bpp)) return 0;
    if (!g_reg_mmio && (!g_io_index || !g_io_data)) return 0;

    uint64_t pixels = (uint64_t)(unsigned int)width * (uint64_t)(unsigned int)height;
    uint64_t fb_limit = svga_effective_fb_limit();
    int try_hw[3] = { 0, 0, 0 };
    int try_cnt = 0;

    if (bpp == 32) {
        if (fb_limit == 0 || pixels * 4ULL <= fb_limit) try_hw[try_cnt++] = 32;
    } else if (bpp == 24) {
        if (fb_limit == 0 || pixels * 4ULL <= fb_limit) try_hw[try_cnt++] = 32;
        if (fb_limit == 0 || pixels * 3ULL <= fb_limit) try_hw[try_cnt++] = 24;
    } else if (bpp == 16) {
        if ((g_capabilities & SVGA_CAP_8BIT_EMULATION) &&
            (fb_limit == 0 || pixels * 2ULL <= fb_limit)) {
            try_hw[try_cnt++] = 16;
        }
        if (fb_limit == 0 || pixels * 4ULL <= fb_limit) try_hw[try_cnt++] = 32;
        if (!(g_capabilities & SVGA_CAP_8BIT_EMULATION) &&
            (fb_limit == 0 || pixels * 2ULL <= fb_limit)) {
            try_hw[try_cnt++] = 16;
        }
    } else {
        if ((g_capabilities & SVGA_CAP_8BIT_EMULATION) &&
            (fb_limit == 0 || pixels <= fb_limit)) {
            try_hw[try_cnt++] = 8;
        }
        if (fb_limit == 0 || pixels * 4ULL <= fb_limit) try_hw[try_cnt++] = 32;
        if (fb_limit == 0 || pixels * 2ULL <= fb_limit) try_hw[try_cnt++] = 16;
    }
    if (try_cnt == 0) return 0;

    if (g_active && g_fifo_ready) {
        vmware_svga_sync();
    }

    int was_active = g_active;
    unsigned int old_enabled = svga_read_reg(SVGA_REG_ENABLE);
    unsigned int old_config_done = svga_read_reg(SVGA_REG_CONFIG_DONE);
    unsigned int old_width = svga_read_reg(SVGA_REG_WIDTH);
    unsigned int old_height = svga_read_reg(SVGA_REG_HEIGHT);
    unsigned int old_bpp = svga_read_reg(SVGA_REG_BPP);

    if (g_stdu_active) {
        svga_teardown_stdu();
    }
    if (g_screen_obj_active) {
        svga_teardown_screen_object();
    }

    g_active = 0;
    g_cursor_ready = 0;
    svga_write_reg(SVGA_REG_CONFIG_DONE, 0);
    svga_write_reg(SVGA_REG_ENABLE, 0);

    svga_write_reg(SVGA_REG_ID, SVGA_ID_2);
    unsigned int negotiated_id = svga_read_reg(SVGA_REG_ID);
    if ((negotiated_id & 0xFFFF0000U) != SVGA_ID_0) {
        svga_restore_previous_mode(old_enabled, old_config_done,
                                   old_width, old_height, old_bpp);
        g_active = was_active;
        g_status = "SVGA ID negotiation failed";
        return 0;
    }
    g_interface_id = negotiated_id;

    int hw_bpp = 0;
    unsigned int pitch = 0;
    unsigned int framebuffer = 0;
    unsigned int fb_offset = 0;

    int legacy_dims_ok =
        (g_raw_max_width == 0U || (unsigned int)width <= g_raw_max_width) &&
        (g_raw_max_height == 0U || (unsigned int)height <= g_raw_max_height);

    if ((!svga_has_stdu() && !svga_has_screen_object2()) || legacy_dims_ok) {
        for (int t = 0; t < try_cnt; t++) {
            int cand_bpp = try_hw[t];
            uint64_t min_need = pixels * (uint64_t)svga_bytes_per_pixel(cand_bpp);
            if ((svga_has_stdu() || svga_has_screen_object2()) && g_raw_vram_size > 0 &&
                min_need > (uint64_t)g_raw_vram_size) {
                continue;
            }
            unsigned int cand_depth = (cand_bpp == 32) ? 24U : (unsigned int)cand_bpp;

            svga_write_reg(SVGA_REG_WIDTH, (unsigned int)width);
            svga_write_reg(SVGA_REG_HEIGHT, (unsigned int)height);
            svga_write_reg(SVGA_REG_DEPTH, cand_depth);
            svga_write_reg(SVGA_REG_BPP, (unsigned int)cand_bpp);
            svga_write_reg(SVGA_REG_ENABLE, 1);

            unsigned int active_enable = svga_read_reg(SVGA_REG_ENABLE);
            unsigned int active_width = svga_read_reg(SVGA_REG_WIDTH);
            unsigned int active_height = svga_read_reg(SVGA_REG_HEIGHT);
            unsigned int active_bpp = svga_read_reg(SVGA_REG_BPP);
            unsigned int active_fb_size = svga_read_reg(SVGA_REG_FB_SIZE);
            unsigned int cand_pitch = svga_read_reg(SVGA_REG_BYTES_PER_LINE);
            unsigned int cand_fb = svga_read_reg(SVGA_REG_FB_START);
            unsigned int cand_off = svga_read_reg(SVGA_REG_FB_OFFSET);
            unsigned int minimum_pitch = (unsigned int)width * svga_bytes_per_pixel(cand_bpp);
            uint64_t fb_need = (uint64_t)cand_pitch * (uint64_t)(unsigned int)height;

            if (active_enable != 0 &&
                active_width == (unsigned int)width &&
                active_height == (unsigned int)height &&
                (active_bpp == 0 || active_bpp == (unsigned int)cand_bpp) &&
                cand_fb != 0 && cand_pitch >= minimum_pitch &&
                (active_fb_size == 0 || (uint64_t)active_fb_size >= fb_need) &&
                (fb_limit == 0 || (uint64_t)cand_off + fb_need <= fb_limit) &&
                (g_bar1_size == 0 || (uint64_t)cand_off + fb_need <= g_bar1_size)) {
                if (active_fb_size > 0) g_fb_size = active_fb_size;
                hw_bpp = cand_bpp;
                pitch = cand_pitch;
                framebuffer = cand_fb;
                fb_offset = cand_off;
                break;
            }
            svga_write_reg(SVGA_REG_ENABLE, 0);
        }
    }

    if (hw_bpp == 0) {
        if (svga_has_stdu() &&
            svga_set_mode_stdu(width, height, bpp, try_hw, try_cnt)) {
            return 1;
        }
        if (svga_has_screen_object2() &&
            svga_set_mode_screen_object(width, height, bpp, try_hw, try_cnt)) {
            return 1;
        }
        svga_restore_previous_mode(old_enabled, old_config_done,
                                   old_width, old_height, old_bpp);
        g_active = was_active;
        g_status = "Display mode was not accepted";
        return 0;
    }

    uint64_t fb_phys = (uint64_t)framebuffer + (uint64_t)fb_offset;
    uint64_t map_size = (uint64_t)pitch * (uint64_t)(unsigned int)height;
    if (map_size < 4ULL * 1024ULL * 1024ULL) {
        map_size = 4ULL * 1024ULL * 1024ULL;
    }
    if (g_bar1_size > 0 && map_size > g_bar1_size) {
        map_size = g_bar1_size;
    }
    uint64_t min_map = (uint64_t)pitch * (uint64_t)(unsigned int)height;
    if (map_size < min_map) map_size = min_map;

    volatile unsigned char* fb_virt =
        (volatile unsigned char*)g_kapi->mmio_map_wc(fb_phys, (size_t)map_size);
    if (!fb_virt) {
        svga_restore_previous_mode(old_enabled, old_config_done,
                                   old_width, old_height, old_bpp);
        g_active = was_active;
        g_status = "Framebuffer mapping failed";
        return 0;
    }

    if (pdv_dma_is_valid(&g_mob_pt_dma)) {
        g_kapi->dma_free(&g_mob_pt_dma);
    }
    if (pdv_dma_is_valid(&g_otable_dma)) {
        g_kapi->dma_free(&g_otable_dma);
    }
    if (pdv_dma_is_valid(&g_gmr_dma)) {
        g_kapi->dma_free(&g_gmr_dma);
    }
    g_stdu_active = 0;
    g_screen_obj_active = 0;

    if (hw_bpp == 8) {
        svga_upload_default_palette();
    }

    g_fb_phys = fb_phys;
    g_fb_virt = fb_virt;
    g_width = width;
    g_height = height;
    g_bpp = hw_bpp;
    g_pitch = (int)pitch;
    g_active = 1;

    svga_apply_surface(width, height, hw_bpp, pitch, fb_phys, fb_virt, 1);

    svga_write_reg(SVGA_REG_TRACES, 1U);
    svga_init_fifo();
    svga_write_reg(SVGA_REG_CONFIG_DONE, 1);
    svga_write_reg(SVGA_REG_CURSOR_ON, SVGA_CURSOR_HIDE);

    if (g_fifo_ready) {
        if (g_capabilities & SVGA_CAP_3D) {
            g_status = "3D-capable profile: FIFO 2D active";
        } else {
            g_status = "FIFO 2D active (WC framebuffer)";
        }
    } else {
        g_status = "Linear WC framebuffer active";
    }

    klog_info("[SVGA] mode %dx%dx%d (hw %dbpp) pitch=%u fb=0x%llx fifo=%s",
              width, height, bpp, hw_bpp, pitch, (unsigned long long)fb_phys,
              g_fifo_ready ? "ready" : "fallback");
    return 1;
}

static int vmware_svga_probe_entry(void) {
    if (!vmware_svga_probe_hw()) return 0;
    int sw = 0, sh = 0, sbpp = 0;
    g_kapi->fb_get_desktop_mode(&sw, &sh, &sbpp);
    pdv_fb_surface_t surf;
    g_kapi->fb_get_surface(&surf);
    int w = sw > 0 ? sw : (int)surf.width;
    int h = sh > 0 ? sh : (int)surf.height;
    int bpp = (sbpp == 8 || sbpp == 16 || sbpp == 24 || sbpp == 32) ? sbpp : 32;
    if (w < PDV_DISPLAY_MIN_WIDTH) w = PDV_DISPLAY_MIN_WIDTH;
    if (h < PDV_DISPLAY_MIN_HEIGHT) h = PDV_DISPLAY_MIN_HEIGHT;
    if ((unsigned int)w > g_max_width) w = (int)g_max_width;
    if ((unsigned int)h > g_max_height) h = (int)g_max_height;

    if (vmware_svga_set_mode(w, h, bpp)) return 1;
    if (bpp != 16 && vmware_svga_set_mode(w, h, 16)) return 1;
    if (vmware_svga_set_mode(w, h, 8)) return 1;

    for (int i = g_svga_mode_count - 1; i >= 0; i--) {
        static const int k_try[4] = { 32, 24, 16, 8 };
        for (int b = 0; b < 4; b++) {
            if (g_svga_modes[i].bpp_mask & pdv_bpp_to_mask(k_try[b])) {
                if (vmware_svga_set_mode((int)g_svga_modes[i].width,
                                         (int)g_svga_modes[i].height,
                                         k_try[b])) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int vmware_svga_is_active(void) {
    return g_active;
}

static int vmware_svga_is_2d_active(void) {
    return g_active;
}

static int vmware_svga_supports_mode(int width, int height, int bpp) {
    if (!g_probed) return 0;
    if (width < PDV_DISPLAY_MIN_WIDTH || height < PDV_DISPLAY_MIN_HEIGHT) return 0;
    if ((unsigned int)width > g_max_width || (unsigned int)height > g_max_height) return 0;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;

    uint64_t pixels = (uint64_t)(unsigned int)width * (uint64_t)(unsigned int)height;
    uint64_t fb_limit = svga_effective_fb_limit();
    if (fb_limit == 0) return 1;

    if (bpp == 32) return (pixels * 4ULL <= fb_limit) ? 1 : 0;
    if (bpp == 24) return (pixels * 3ULL <= fb_limit) ? 1 : 0;
    if (bpp == 16) return (pixels * 2ULL <= fb_limit) ? 1 : 0;
    /* bpp == 8 */
    if ((g_capabilities & SVGA_CAP_8BIT_EMULATION) && pixels <= fb_limit) return 1;
    return (pixels * 2ULL <= fb_limit) ? 1 : 0;
}

static int vmware_svga_get_mode_count(void) {
    if (!g_probed) return 0;
    return g_svga_mode_count;
}

static int vmware_svga_get_mode(int index, pdv_display_mode_t* out_mode) {
    if (!g_probed || !out_mode || index < 0 || index >= g_svga_mode_count) {
        return 0;
    }
    *out_mode = g_svga_modes[index];
    return 1;
}

static int vmware_svga_get_max_width(void) {
    if (!g_probed) return 0;
    return (int)g_max_width;
}

static int vmware_svga_get_max_height(void) {
    if (!g_probed) return 0;
    return (int)g_max_height;
}

static const char* vmware_svga_get_name(void) {
    return g_name;
}

static const char* vmware_svga_get_status(void) {
    return g_status;
}

static unsigned int vmware_svga_get_vram_size(void) {
    return g_vram_size;
}

static void vmware_svga_present_rect(int x, int y, int width, int height) {
    if (!g_active || !g_fifo_ready ||
        !svga_normalize_rect(&x, &y, &width, &height)) {
        return;
    }

    if (g_stdu_active) {
        g_kapi->dma_sync_for_device(&g_gmr_dma);
        unsigned int command[18];
        command[0]  = SVGA_3D_CMD_UPDATE_GB_IMAGE;
        command[1]  = 36U;
        command[2]  = 0U;
        command[3]  = 0U;
        command[4]  = 0U;
        command[5]  = (unsigned int)x;
        command[6]  = (unsigned int)y;
        command[7]  = 0U;
        command[8]  = (unsigned int)width;
        command[9]  = (unsigned int)height;
        command[10] = 1U;

        command[11] = SVGA_3D_CMD_UPDATE_GB_SCREENTARGET;
        command[12] = 20U;
        command[13] = 0U;
        command[14] = (unsigned int)x;
        command[15] = (unsigned int)y;
        command[16] = (unsigned int)width;
        command[17] = (unsigned int)height;
        if (svga_fifo_emit(command, 18U)) {
            vmware_svga_sync();
        } else {
            g_fifo_ready = 0;
            g_status = "FIFO full (STDU update stalled)";
        }
        return;
    }

    if (g_screen_obj_active) {
        g_kapi->dma_sync_for_device(&g_gmr_dma);
        unsigned int command[8];
        command[0] = SVGA_CMD_BLIT_GMRFB_TO_SCREEN;
        command[1] = (unsigned int)x;
        command[2] = (unsigned int)y;
        command[3] = (unsigned int)x;
        command[4] = (unsigned int)y;
        command[5] = (unsigned int)(x + width);
        command[6] = (unsigned int)(y + height);
        command[7] = 0U;
        if (svga_fifo_emit(command, 8U)) {
            vmware_svga_sync();
        } else {
            g_fifo_ready = 0;
            g_status = "FIFO full (ScreenObject2 blit stalled)";
        }
        return;
    }

    unsigned int command[5];
    command[0] = SVGA_CMD_UPDATE;
    command[1] = (unsigned int)x;
    command[2] = (unsigned int)y;
    command[3] = (unsigned int)width;
    command[4] = (unsigned int)height;
    if (svga_fifo_emit(command, 5)) {
        vmware_svga_sync();
    } else {
        g_fifo_ready = 0;
        g_status = "FIFO full (linear framebuffer fallback)";
    }
}

static void svga_software_fill(int x, int y, int width, int height,
                               unsigned int color) {
    if (!g_fb_virt) return;
    unsigned int bytes = svga_bytes_per_pixel(g_bpp);
    volatile unsigned char* framebuffer = g_fb_virt;
    for (int row = y; row < y + height; row++) {
        volatile unsigned char* pixel =
            framebuffer + row * g_pitch + x * bytes;
        if (bytes == 4) {
            volatile unsigned int* p = (volatile unsigned int*)pixel;
            for (int column = 0; column < width; column++) p[column] = color;
        } else if (bytes == 2) {
            unsigned short value = (unsigned short)color;
            volatile unsigned short* p = (volatile unsigned short*)pixel;
            for (int column = 0; column < width; column++) p[column] = value;
        } else if (bytes == 1) {
            unsigned char value = (unsigned char)color;
            for (int column = 0; column < width; column++) pixel[column] = value;
        }
    }
}

static void svga_software_blt(int sx, int sy, int dx, int dy,
                              int width, int height) {
    if (!g_fb_virt) return;
    unsigned int bytes = svga_bytes_per_pixel(g_bpp);
    unsigned int row_bytes = (unsigned int)width * bytes;
    volatile unsigned char* fb = g_fb_virt;

    if (dy > sy) {
        for (int r = height - 1; r >= 0; r--) {
            volatile unsigned char* dst = fb + (dy + r) * g_pitch + dx * bytes;
            volatile unsigned char* src = fb + (sy + r) * g_pitch + sx * bytes;
            for (unsigned int c = 0; c < row_bytes; c++) dst[c] = src[c];
        }
    } else if (dy == sy && dx > sx) {
        for (int r = 0; r < height; r++) {
            volatile unsigned char* dst = fb + (dy + r) * g_pitch + dx * bytes;
            volatile unsigned char* src = fb + (sy + r) * g_pitch + sx * bytes;
            for (unsigned int c = row_bytes; c > 0; c--) dst[c - 1] = src[c - 1];
        }
    } else {
        for (int r = 0; r < height; r++) {
            volatile unsigned char* dst = fb + (dy + r) * g_pitch + dx * bytes;
            volatile unsigned char* src = fb + (sy + r) * g_pitch + sx * bytes;
            for (unsigned int c = 0; c < row_bytes; c++) dst[c] = src[c];
        }
    }
}

static void vmware_svga_fill_rect(int x, int y, int width, int height,
                                  unsigned int color) {
    int original_x = x;
    int original_y = y;
    int original_width = width;
    int original_height = height;
    if (!g_active || !svga_normalize_rect(&x, &y, &width, &height)) return;

    int accelerated = 0;
    if (!g_screen_obj_active && !g_stdu_active && g_fifo_ready && (g_capabilities & SVGA_CAP_RECT_FILL)) {
        unsigned int command[6];
        command[0] = SVGA_CMD_RECT_FILL;
        command[1] = color;
        command[2] = (unsigned int)x;
        command[3] = (unsigned int)y;
        command[4] = (unsigned int)width;
        command[5] = (unsigned int)height;
        accelerated = svga_fifo_emit(command, 6);
    }

    if (!accelerated) {
        svga_software_fill(x, y, width, height, color);
    }
    vmware_svga_present_rect(original_x, original_y,
                             original_width, original_height);
}

static void vmware_svga_screen_blt(int sx, int sy, int dx, int dy,
                                   int width, int height) {
    if (!g_active || !g_fb_virt) return;
    if (sx < 0) { width += sx; dx -= sx; sx = 0; }
    if (sy < 0) { height += sy; dy -= sy; sy = 0; }
    if (dx < 0) { width += dx; sx -= dx; dx = 0; }
    if (dy < 0) { height += dy; sy -= dy; dy = 0; }
    if (sx + width > g_width) width = g_width - sx;
    if (sy + height > g_height) height = g_height - sy;
    if (dx + width > g_width) width = g_width - dx;
    if (dy + height > g_height) height = g_height - dy;
    if (width <= 0 || height <= 0) return;

    int accelerated = 0;
    if (!g_screen_obj_active && !g_stdu_active && g_fifo_ready && (g_capabilities & SVGA_CAP_RECT_COPY)) {
        unsigned int command[7];
        command[0] = SVGA_CMD_RECT_COPY;
        command[1] = (unsigned int)sx;
        command[2] = (unsigned int)sy;
        command[3] = (unsigned int)dx;
        command[4] = (unsigned int)dy;
        command[5] = (unsigned int)width;
        command[6] = (unsigned int)height;
        accelerated = svga_fifo_emit(command, 7);
    }
    if (!accelerated) {
        svga_software_blt(sx, sy, dx, dy, width, height);
    }
    vmware_svga_present_rect(dx, dy, width, height);
}

static const pdv_display_driver_t g_vmwgfx_drv = {
    .name                 = "VMware SVGA II",
    .priority             = 100,
    .requires_hw_mode_set = 1,
    .probe                = vmware_svga_probe_entry,
    .start                = 0,
    .stop                 = 0,
    .get_name             = vmware_svga_get_name,
    .get_status           = vmware_svga_get_status,
    .is_active            = vmware_svga_is_active,
    .is_2d_active         = vmware_svga_is_2d_active,
    .get_vram_size        = vmware_svga_get_vram_size,
    .can_set_mode         = vmware_svga_supports_mode,
    .supports_bpp         = vmware_svga_supports_mode,
    .set_mode             = vmware_svga_set_mode,
    .fill_rect            = vmware_svga_fill_rect,
    .screen_blt           = vmware_svga_screen_blt,
    .present_rect         = vmware_svga_present_rect,
    .sync                 = vmware_svga_sync,
    .has_hw_cursor        = 0,
    .set_cursor           = 0,
    .get_mode_count       = vmware_svga_get_mode_count,
    .get_mode             = vmware_svga_get_mode,
    .get_max_width        = vmware_svga_get_max_width,
    .get_max_height       = vmware_svga_get_max_height,
};

int pdv_driver_entry(const pdv_kernel_api_t* kapi) {
    if (!kapi || kapi->ddk_version < PDV_DDK_VERSION_1_0) return -1;
    g_kapi = kapi;
    return kapi->register_display_driver(&g_vmwgfx_drv);
}
