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

#define SVGA_CAP_RECT_FILL       0x00000001U
#define SVGA_CAP_RECT_COPY       0x00000002U
#define SVGA_CAP_ALPHA_CURSOR    0x00000010U
#define SVGA_CAP_8BIT_EMULATION  0x00000100U
#define SVGA_CAP_3D              0x00000200U
#define SVGA_CAP_EXTENDED_FIFO   0x00000400U
#define SVGA_CAP_GMR             0x00100000U
#define SVGA_CAP_GMR2            0x00400000U
#define SVGA_CAP_SCREEN_OBJECT_2 0x00800000U
#define SVGA_CAP_GBOBJECTS       0x08000000U

#define SVGA_ID_0 0x90000000U
#define SVGA_ID_1 0x90000001U
#define SVGA_ID_2 0x90000002U

#define SVGA_REG_ID             0
#define SVGA_REG_ENABLE         1
#define SVGA_REG_WIDTH          2
#define SVGA_REG_HEIGHT         3
#define SVGA_REG_MAX_WIDTH      4
#define SVGA_REG_MAX_HEIGHT     5
#define SVGA_REG_DEPTH          6
#define SVGA_REG_BPP            7
#define SVGA_REG_PSEUDOCOLOR    8
#define SVGA_REG_RED_MASK       9
#define SVGA_REG_GREEN_MASK     10
#define SVGA_REG_BLUE_MASK      11
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START       13
#define SVGA_REG_FB_OFFSET      14
#define SVGA_REG_VRAM_SIZE      15
#define SVGA_REG_FB_SIZE        16
#define SVGA_REG_CAPABILITIES   17
#define SVGA_REG_MEM_SIZE       19
#define SVGA_REG_CONFIG_DONE    20
#define SVGA_REG_SYNC           21
#define SVGA_REG_BUSY           22
#define SVGA_REG_GUEST_ID       23
#define SVGA_REG_CURSOR_ID      24
#define SVGA_REG_MEMORY_SIZE    47
#define GUEST_OS_OTHER          0x5010U
#define SVGA_REG_CURSOR_X       25
#define SVGA_REG_CURSOR_Y       26
#define SVGA_REG_CURSOR_ON      27
#define SVGA_PALETTE_BASE       1024

#define SVGA_MAX_MODES          32

#define SVGA_FIFO_MIN       0
#define SVGA_FIFO_MAX       1
#define SVGA_FIFO_NEXT_CMD  2
#define SVGA_FIFO_STOP      3
#define SVGA_FIFO_COMMIT    4

#define SVGA_CMD_UPDATE        1
#define SVGA_CMD_RECT_FILL     2
#define SVGA_CMD_RECT_COPY     3
#define SVGA_CMD_DEFINE_CURSOR 19
#define SVGA_CMD_MOVE_CURSOR   20

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
static int g_reg_mmio;
static unsigned int g_io_index;
static unsigned int g_io_data;
static volatile unsigned char* g_reg_mmio_base;
static uint64_t g_fb_phys;
static volatile unsigned char* g_fb_virt;
static uint64_t g_fifo_phys;
static unsigned int g_fifo_size;
static unsigned int g_fifo_min;
static unsigned int g_fifo_max;
static unsigned int g_vram_size;
static unsigned int g_raw_vram_size;
static unsigned int g_memory_size;
static unsigned int g_fb_size;
static uint64_t g_bar1_size;
static unsigned int g_capabilities;
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
            svga_fifo_write(SVGA_FIFO_COMMIT, cursor);
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
    g_fifo = (volatile unsigned int*)g_kapi->mmio_map_uc(g_fifo_phys, map_bytes);
    if (!g_fifo) return 0;

    g_fifo_min = SVGA_FIFO_MIN_BYTES;
    if (g_fifo_min + 8192U >= map_bytes) g_fifo_min = 0;
    g_fifo_max = map_bytes;

    svga_fifo_write(SVGA_FIFO_MIN, g_fifo_min);
    svga_fifo_write(SVGA_FIFO_MAX, g_fifo_max);
    svga_fifo_write(SVGA_FIFO_NEXT_CMD, g_fifo_min);
    svga_fifo_write(SVGA_FIFO_STOP, g_fifo_min);
    svga_fifo_write(SVGA_FIFO_COMMIT, g_fifo_min);
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

static uint64_t svga_effective_fb_limit(void) {
    uint64_t limit = 0;
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
    g_pci = 0;
    g_fifo = 0;
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
    g_memory_size = 0;
    if ((g_capabilities & (SVGA_CAP_GMR | SVGA_CAP_GMR2 | SVGA_CAP_GBOBJECTS)) != 0U) {
        unsigned int mem_sz = svga_read_reg(SVGA_REG_MEMORY_SIZE);
        if (mem_sz != 0xFFFFFFFFU) {
            g_memory_size = mem_sz;
        }
    }
    g_vram_size = g_raw_vram_size;
    if (g_memory_size > g_raw_vram_size && g_bar1_size > (uint64_t)g_raw_vram_size) {
        uint64_t usable = (uint64_t)g_memory_size;
        if (g_bar1_size > 0 && g_bar1_size < usable) {
            usable = g_bar1_size;
        }
        g_vram_size = (usable > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (unsigned int)usable;
    } else if (g_vram_size == 0 && g_bar1_size > 0) {
        g_vram_size = (g_bar1_size > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (unsigned int)g_bar1_size;
    }
    g_raw_max_width = svga_read_reg(SVGA_REG_MAX_WIDTH);
    g_raw_max_height = svga_read_reg(SVGA_REG_MAX_HEIGHT);
    g_max_width = g_raw_max_width;
    g_max_height = g_raw_max_height;
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

    if (g_pci->bar[2].valid && !g_pci->bar[2].is_io) {
        g_fifo_phys = g_pci->bar[2].address;
    }

    g_probed = 1;
    svga_rebuild_mode_table();
    g_status = "Detected, mode not configured";
    klog_info("[SVGA] %s id=0x%X caps=0x%X raw_vram=%uKB mem=%uKB eff_vram=%uKB fb=%uKB bar1=%uKB raw_max=%ux%u eff_max=%ux%u modes=%d",
              g_name, g_interface_id, g_capabilities,
              g_raw_vram_size / 1024U, g_memory_size / 1024U,
              g_vram_size / 1024U, g_fb_size / 1024U,
              (unsigned int)(g_bar1_size / 1024ULL),
              g_raw_max_width, g_raw_max_height,
              g_max_width, g_max_height, g_svga_mode_count);
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

    for (int t = 0; t < try_cnt; t++) {
        int cand_bpp = try_hw[t];
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

    if (hw_bpp == 0) {
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
        unsigned int gm = svga_read_reg(SVGA_REG_GREEN_MASK);
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
    g_kapi->fb_set_surface(&surf, (unsigned char*)fb_virt);

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

static void vmware_svga_fill_rect(int x, int y, int width, int height,
                                  unsigned int color) {
    int original_x = x;
    int original_y = y;
    int original_width = width;
    int original_height = height;
    if (!g_active || !svga_normalize_rect(&x, &y, &width, &height)) return;

    int accelerated = 0;
    if (g_fifo_ready && (g_capabilities & SVGA_CAP_RECT_FILL)) {
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

    if (g_fifo_ready && (g_capabilities & SVGA_CAP_RECT_COPY)) {
        unsigned int command[7];
        command[0] = SVGA_CMD_RECT_COPY;
        command[1] = (unsigned int)sx;
        command[2] = (unsigned int)sy;
        command[3] = (unsigned int)dx;
        command[4] = (unsigned int)dy;
        command[5] = (unsigned int)width;
        command[6] = (unsigned int)height;
        (void)svga_fifo_emit(command, 7);
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
