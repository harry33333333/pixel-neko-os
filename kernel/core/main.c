#include "limine.h"
#include "version.h"
#include "log.h"
#include "cpu.h"
#include "mm.h"
#include "boot_framebuffer.h"
#include "port.h"
#include "speaker.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "acpi.h"
#include "apic.h"
#include "dma.h"
#include "panic.h"
#include "fpu.h"
#include "scheduler.h"
#include "process.h"
#include "ipc.h"
#include "syscall.h"
#include "device.h"
#include "pci.h"
#include "input.h"
#include "usb_core.h"
#include "usb_hid.h"
#include "storage.h"
#include "part.h"
#include "vfs.h"
#include "pdv.h"
#include "p5_test.h"
#include "splash.h"
#include "ui_compat.h"
#include "settings.h"
#include "wm.h"
#include "power.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Global HHDM and memory limits
hhdm_info_t g_hhdm = {0};

// Boot splash mode: false = Windows XP-style progress bar; true = Classic verbose badge
bool g_verbose_boot = false;
volatile bool g_xp_boot_active = false;
static int g_xp_step_frame = 0;
static bool g_acpi_fault_test = false;
bool g_acpi_shutdown_test_requested = false;

// --- Limine Protocol Requests (v8.x Standard) ---

__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
volatile struct limine_stack_size_request stack_size_request = {
    .id = LIMINE_STACK_SIZE_REQUEST,
    .revision = 0,
    .stack_size = 65536
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_kernel_file_request kernel_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

// --- Helper Functions ---

static bool cmdline_contains(const char* needle) {
    if (!kernel_file_request.response ||
        !kernel_file_request.response->kernel_file ||
        !kernel_file_request.response->kernel_file->cmdline ||
        !needle) {
        return false;
    }
    const char* haystack = kernel_file_request.response->kernel_file->cmdline;
    size_t nlen = strlen(needle);
    for (size_t i = 0; haystack[i]; i++) {
        if (strncmp(haystack + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static void parse_memory_map(void) {
    if (!memmap_request.response) {
        log_printf("[MM WARN] Limine memmap response not provided!\n");
        return;
    }

    uint64_t entry_count = memmap_request.response->entry_count;
    struct limine_memmap_entry** entries = memmap_request.response->entries;

    uint64_t max_phys = 0;
    uint64_t usable_bytes = 0;
    uint64_t high_ram_bytes = 0;

    log_printf("[MM] Limine MemMap provided %llu entries:\n", entry_count);
    for (uint64_t i = 0; i < entry_count; i++) {
        struct limine_memmap_entry* e = entries[i];
        uint64_t base = e->base;
        uint64_t length = e->length;
        uint64_t end = base + length;

        if (end > max_phys) {
            max_phys = end;
        }

        bool is_ram = false;
        const char* type_str = "UNKNOWN";
        switch (e->type) {
            case LIMINE_MEMMAP_USABLE:
                type_str = "USABLE";
                usable_bytes += length;
                is_ram = true;
                break;
            case LIMINE_MEMMAP_RESERVED:
                type_str = "RESERVED";
                break;
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
                type_str = "ACPI_RECLAIMABLE";
                is_ram = true;
                break;
            case LIMINE_MEMMAP_ACPI_NVS:
                type_str = "ACPI_NVS";
                is_ram = true;
                break;
            case LIMINE_MEMMAP_BAD_MEMORY:
                type_str = "BAD_MEMORY";
                break;
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
                type_str = "BOOTLOADER_RECLAIMABLE";
                is_ram = true;
                break;
            case LIMINE_MEMMAP_KERNEL_AND_MODULES:
                type_str = "KERNEL_AND_MODULES";
                is_ram = true;
                break;
            case LIMINE_MEMMAP_FRAMEBUFFER:
                type_str = "FRAMEBUFFER";
                break;
        }

        if (is_ram && end > 0x100000ULL) {
            uint64_t high_start = (base < 0x100000ULL) ? 0x100000ULL : base;
            high_ram_bytes += (end - high_start);
        }

        if (i < 16) {
            log_printf("  [0x%016llx - 0x%016llx] (size: %llu KB) -> %s\n",
                       base, end, length / 1024, type_str);
        }
    }

    if (max_phys < 0x100000000ULL) {
        max_phys = 0x100000000ULL;
    }

    uint64_t total_ram_bytes = 0x100000ULL + high_ram_bytes;
    if (total_ram_bytes < usable_bytes) {
        total_ram_bytes = usable_bytes;
    }

    g_hhdm.physical_memory_limit = max_phys;
    g_hhdm.total_usable_bytes = usable_bytes;
    g_hhdm.total_memory_bytes = total_ram_bytes;

    log_printf("[MM] Physical Memory Limit: 0x%llx (%llu MB), Installed RAM: %llu MB, Usable RAM: %llu MB\n",
               g_hhdm.physical_memory_limit, g_hhdm.physical_memory_limit / (1024 * 1024),
               (total_ram_bytes + 0x7FFFFULL) / (1024 * 1024),
               usable_bytes / (1024 * 1024));
}

// Global UI interaction bounds
static int g_btn_x = 0, g_btn_y = 0, g_btn_w = 90, g_btn_h = 24;
static int g_bar_x = 0, g_bar_y = 0, g_bar_w = 0, g_bar_h = 16;

extern const uint8_t _binary_bin_init_elf_start[];
extern const uint8_t _binary_bin_init_elf_end[];

static void draw_button(int x, int y, int w, int h, bool pressed) {
    uint32_t c_white = boot_framebuffer_make_color(255, 255, 255);
    uint32_t c_gray  = boot_framebuffer_make_color(192, 192, 192);
    uint32_t c_black = boot_framebuffer_make_color(0, 0, 0);

    boot_framebuffer_fill_rect(x, y, w, h, c_gray);
    if (!pressed) {
        boot_framebuffer_fill_rect(x, y, w, 2, c_white);
        boot_framebuffer_fill_rect(x, y, 2, h, c_white);
        boot_framebuffer_fill_rect(x + w - 2, y, 2, h, c_black);
        boot_framebuffer_fill_rect(x, y + h - 2, w, 2, c_black);
        boot_framebuffer_draw_string(x + 18, y + 4, "[ OK ]", c_black, c_gray);
    } else {
        boot_framebuffer_fill_rect(x, y, w, 2, c_black);
        boot_framebuffer_fill_rect(x, y, 2, h, c_black);
        boot_framebuffer_fill_rect(x + w - 2, y, 2, h, c_white);
        boot_framebuffer_fill_rect(x, y + h - 2, w, 2, c_white);
        boot_framebuffer_draw_string(x + 19, y + 5, "[ OK ]", c_black, c_gray);
    }
}

// Draw animated 45-degree diagonal striped Barber-Pole progress bar (Classic Mac / Win3.1 style)
static void draw_barber_pole(int x, int y, int w, int h, int frame) {
    if (!g_main_surface.framebuffer_virt || w <= 0 || h <= 0) return;

    uint32_t c_white  = boot_framebuffer_make_color(255, 255, 255);
    uint32_t c_dgray  = boot_framebuffer_make_color(128, 128, 128);
    uint32_t c_stripe = boot_framebuffer_make_color(0, 0, 160); // Classic Navy Blue

    // 1-pixel Inset 3D border
    boot_framebuffer_fill_rect(x, y, w, 1, c_dgray);
    boot_framebuffer_fill_rect(x, y, 1, h, c_dgray);
    boot_framebuffer_fill_rect(x, y + h - 1, w, 1, c_white);
    boot_framebuffer_fill_rect(x + w - 1, y, 1, h, c_white);

    int inner_x = x + 1;
    int inner_y = y + 1;
    int inner_w = w - 2;
    int inner_h = h - 2;

    for (int cy = inner_y; cy < inner_y + inner_h; cy++) {
        for (int cx = 0; cx < inner_w; cx++) {
            int pattern = (cx + (cy - inner_y) + frame) % 16;
            uint32_t color = (pattern < 8) ? c_stripe : c_white;
            boot_framebuffer_putpixel(inner_x + cx, cy, color);
        }
    }
}

// Initialize modern Windows XP-style boot screen
static void boot_screen_xp_init(void) {
    if (!g_main_surface.framebuffer_virt) return;

    // Pitch black background
    uint32_t c_black = boot_framebuffer_make_color(0, 0, 0);
    boot_framebuffer_clear(c_black);

    int cx = (int)g_main_surface.width / 2;
    int cy = (int)g_main_surface.height / 2;

    // 1. Draw iconic TBMK emblem
    int lx = cx - 20;
    int ly = cy - 92;

    uint32_t c_navy    = boot_framebuffer_make_color(0, 0, 160);     // 1: Dark Blue bar
    uint32_t c_hiblue  = boot_framebuffer_make_color(50, 120, 240);  // 9: Light Blue highlight
    uint32_t c_red     = boot_framebuffer_make_color(160, 20, 20);   // 4: Red body
    uint32_t c_hired   = boot_framebuffer_make_color(240, 50, 50);   // 12: Red highlight
    uint32_t c_green   = boot_framebuffer_make_color(20, 160, 20);   // 2: Green body
    uint32_t c_higreen = boot_framebuffer_make_color(50, 220, 50);   // 10: Green highlight
    uint32_t c_cyan    = boot_framebuffer_make_color(20, 160, 160);  // 3: Cyan body
    uint32_t c_hicyan  = boot_framebuffer_make_color(50, 220, 220);  // 11: Cyan highlight
    uint32_t c_mag     = boot_framebuffer_make_color(160, 20, 160);  // 5: Magenta body
    uint32_t c_himag   = boot_framebuffer_make_color(230, 60, 230);  // 13: Magenta highlight
    uint32_t c_dgray   = boot_framebuffer_make_color(60, 64, 72);
    uint32_t c_white   = boot_framebuffer_make_color(255, 255, 255);

    // Dark blue bar (55x15)
    for (int y = ly + 3; y < ly + 18; y++) {
        for (int x = lx - 7; x < lx + 48; x++) {
            boot_framebuffer_putpixel(x, y, c_navy);
        }
    }
    // Top highlight line on blue bar
    for (int x = lx - 4; x < lx + 45; x++) {
        boot_framebuffer_putpixel(x, ly + 3, c_hiblue);
    }

    // Red block (12x12)
    for (int y = ly; y < ly + 12; y++) {
        for (int x = lx; x < lx + 12; x++) {
            boot_framebuffer_putpixel(x, y, (y == ly || x == lx) ? c_hired : c_red);
        }
    }

    // Green block (12x12)
    for (int y = ly; y < ly + 12; y++) {
        for (int x = lx + 10; x < lx + 22; x++) {
            boot_framebuffer_putpixel(x, y, (y == ly || x == lx + 10) ? c_higreen : c_green);
        }
    }

    // Cyan block (12x12)
    for (int y = ly + 7; y < ly + 19; y++) {
        for (int x = lx + 19; x < lx + 31; x++) {
            boot_framebuffer_putpixel(x, y, (y == ly + 7 || x == lx + 19) ? c_hicyan : c_cyan);
        }
    }

    // Magenta block (12x12)
    for (int y = ly + 7; y < ly + 19; y++) {
        for (int x = lx + 29; x < lx + 41; x++) {
            boot_framebuffer_putpixel(x, y, (y == ly + 7 || x == lx + 29) ? c_himag : c_mag);
        }
    }

    // "TBMK" text in signature 4 colors
    uint32_t c_txt_t = boot_framebuffer_make_color(60, 130, 255); // Blue
    uint32_t c_txt_b = boot_framebuffer_make_color(50, 210, 50);  // Green
    uint32_t c_txt_m = boot_framebuffer_make_color(40, 210, 220); // Cyan
    uint32_t c_txt_k = boot_framebuffer_make_color(220, 50, 220); // Magenta

    boot_framebuffer_draw_char(lx + 2,  ly + 24, 'T', c_txt_t, c_black);
    boot_framebuffer_draw_char(lx + 10, ly + 24, 'B', c_txt_b, c_black);
    boot_framebuffer_draw_char(lx + 18, ly + 24, 'M', c_txt_m, c_black);
    boot_framebuffer_draw_char(lx + 26, ly + 24, 'K', c_txt_k, c_black);

    // 2. Title & Edition Typography
    const char* title = "Pixel Neko OS";
    int title_len = (int)strlen(title);
    int title_x = cx - (title_len * 8) / 2;
    boot_framebuffer_draw_string(title_x + 1, cy - 40 + 1, title, c_dgray, c_black);
    boot_framebuffer_draw_string(title_x, cy - 40, title, c_white, c_black);

    const char* sub = "Snow Neko  ::  64-Bit Edition";
    int sub_len = (int)strlen(sub);
    int sub_x = cx - (sub_len * 8) / 2;
    uint32_t c_sub = boot_framebuffer_make_color(150, 180, 220);
    boot_framebuffer_draw_string(sub_x, cy - 20, sub, c_sub, c_black);

    // 3. Progress Bar Track (Width 200, Height 16)
    int bar_w = 200;
    int bar_h = 16;
    int bar_x = cx - bar_w / 2;
    int bar_y = cy + 24;

    uint32_t c_border = boot_framebuffer_make_color(75, 80, 92);
    uint32_t c_dark   = boot_framebuffer_make_color(22, 26, 34);

    // Outer border
    boot_framebuffer_fill_rect(bar_x, bar_y, bar_w, 1, c_border);
    boot_framebuffer_fill_rect(bar_x, bar_y, 1, bar_h, c_border);
    boot_framebuffer_fill_rect(bar_x + bar_w - 1, bar_y, 1, bar_h, c_border);
    boot_framebuffer_fill_rect(bar_x, bar_y + bar_h - 1, bar_w, 1, c_border);

    // Inner shadow
    boot_framebuffer_fill_rect(bar_x + 1, bar_y + 1, bar_w - 2, 1, c_dark);
    boot_framebuffer_fill_rect(bar_x + 1, bar_y + 1, 1, bar_h - 2, c_dark);
    boot_framebuffer_fill_rect(bar_x + bar_w - 2, bar_y + 1, 1, bar_h - 2, c_dark);
    boot_framebuffer_fill_rect(bar_x + 1, bar_y + bar_h - 2, bar_w - 2, 1, c_dark);

    // Track interior (196 x 12)
    boot_framebuffer_fill_rect(bar_x + 2, bar_y + 2, bar_w - 4, bar_h - 4, c_black);

    // 4. Subtle copyright footer
    const char* footer = "Copyright (C) 2026 Tairitsu_tty / XNZM. All rights reserved.";
    int footer_len = (int)strlen(footer);
    int footer_x = cx - (footer_len * 8) / 2;
    int footer_y = (int)g_main_surface.height - 28;
    if (footer_y > cy + 80) {
        uint32_t c_dim = boot_framebuffer_make_color(85, 90, 100);
        boot_framebuffer_draw_string(footer_x, footer_y, footer, c_dim, c_black);
    }

    g_xp_step_frame = 0;
    g_xp_boot_active = true;
}

// Draw the iconic 3-segment XP glowing blue marquee progress bar
static void draw_xp_progress_frame(int frame) {
    if (!g_main_surface.framebuffer_virt) return;

    int cx = (int)g_main_surface.width / 2;
    int cy = (int)g_main_surface.height / 2;

    int bar_w = 200;
    int bar_h = 16;
    int bar_x = cx - bar_w / 2;
    int bar_y = cy + 24;

    int inner_x = bar_x + 2;
    int inner_y = bar_y + 2;
    int inner_w = bar_w - 4; // 196 px
    int inner_h = bar_h - 4; // 12 px

    uint32_t c_black = boot_framebuffer_make_color(0, 0, 0);
    // Clear track interior
    boot_framebuffer_fill_rect(inner_x, inner_y, inner_w, inner_h, c_black);

    // 3 segmented glowing blue pill blocks
    int seg_w = 9;
    int seg_gap = 2;
    int cluster_w = 3 * seg_w + 2 * seg_gap; // 31 px
    int total_range = inner_w + cluster_w;

    int cluster_x = (frame * 6) % total_range - cluster_w;

    // Windows XP blue glowing palette
    uint32_t c_hi    = boot_framebuffer_make_color(170, 225, 255); // Top highlight
    uint32_t c_body1 = boot_framebuffer_make_color(70, 165, 255);  // Upper gradient
    uint32_t c_body2 = boot_framebuffer_make_color(30, 115, 245);  // Core vibrant XP blue
    uint32_t c_body3 = boot_framebuffer_make_color(15, 75, 205);   // Lower gradient
    uint32_t c_bot   = boot_framebuffer_make_color(10, 45, 150);   // Bottom shadow

    for (int s = 0; s < 3; s++) {
        int sx = cluster_x + s * (seg_w + seg_gap);
        for (int px = 0; px < seg_w; px++) {
            int target_x = sx + px;
            if (target_x >= 0 && target_x < inner_w) {
                for (int py = 0; py < inner_h; py++) {
                    // Rounded corners: skip 4 extreme corner pixels
                    if ((px == 0 || px == seg_w - 1) && (py == 0 || py == inner_h - 1)) {
                        continue;
                    }
                    uint32_t col;
                    if (py == 0) col = c_hi;
                    else if (py < 3) col = c_body1;
                    else if (py < 8) col = c_body2;
                    else if (py < 11) col = c_body3;
                    else col = c_bot;

                    boot_framebuffer_putpixel(inner_x + target_x, inner_y + py, col);
                }
            }
        }
    }
}

// Tick the Windows XP marquee progress bar by 1 frame
void boot_screen_tick(void) {
    if (!g_xp_boot_active || g_verbose_boot) return;
    if (!g_main_surface.framebuffer_virt) return;

    g_xp_step_frame++;
    draw_xp_progress_frame(g_xp_step_frame);
    boot_framebuffer_flush();
}

// Render real-time bring-up progress badge at screen bottom
static void boot_progress_notify(const char* step_msg) {
    if (!g_main_surface.framebuffer_virt) return;

    if (g_verbose_boot) {
        // Classic Diagnostic Boot: Render step badge at bottom of 50% checkerboard screen
        uint32_t c_white = boot_framebuffer_make_color(255, 255, 255);
        uint32_t c_dgray = boot_framebuffer_make_color(128, 128, 128);
        uint32_t c_blue  = boot_framebuffer_make_color(0, 0, 128);

        int badge_w = 480;
        int badge_h = 24;
        int badge_x = (int)(g_main_surface.width - badge_w) / 2;
        int badge_y = (int)g_main_surface.height - 36;
        if (badge_y < 10) badge_y = 10;

        boot_framebuffer_fill_rect(badge_x, badge_y, badge_w, badge_h, c_white);
        boot_framebuffer_fill_rect(badge_x, badge_y, badge_w, 1, c_dgray);
        boot_framebuffer_fill_rect(badge_x, badge_y, 1, badge_h, c_dgray);
        boot_framebuffer_draw_string(badge_x + 12, badge_y + 4, step_msg, c_blue, c_white);
        boot_framebuffer_flush();
    } else {
        // Windows XP Boot Mode: Smoothly advance the 3 glowing blue blocks
        (void)step_msg;
        boot_screen_tick();
    }
}

// Draw a classic Windows 3.1 / Mac OS 9 style 3D modal window
static void __attribute__((unused)) draw_welcome_window(void) {
    if (!g_main_surface.framebuffer_virt) return;

    int win_w = 560;
    int win_h = 360;
    int win_x = (int)(g_main_surface.width - win_w) / 2;
    int win_y = (int)(g_main_surface.height - win_h) / 2;

    uint32_t c_white = boot_framebuffer_make_color(255, 255, 255);
    uint32_t c_gray  = boot_framebuffer_make_color(192, 192, 192);
    uint32_t c_dgray = boot_framebuffer_make_color(128, 128, 128);
    uint32_t c_black = boot_framebuffer_make_color(0, 0, 0);
    uint32_t c_title = boot_framebuffer_make_color(0, 0, 128);       // Classic Win3.1 Navy Blue
    uint32_t c_green = boot_framebuffer_make_color(0, 128, 0);

    // 1. Drop shadow (Mac/Win3.1 style)
    boot_framebuffer_fill_rect(win_x + 6, win_y + 6, win_w, win_h, c_dgray);

    // 2. Window Body
    boot_framebuffer_fill_rect(win_x, win_y, win_w, win_h, c_gray);

    // 3. 3D Outer Bevel
    // Top & Left white highlight
    boot_framebuffer_fill_rect(win_x, win_y, win_w, 2, c_white);
    boot_framebuffer_fill_rect(win_x, win_y, 2, win_h, c_white);
    // Bottom & Right dark border
    boot_framebuffer_fill_rect(win_x + win_w - 2, win_y, 2, win_h, c_black);
    boot_framebuffer_fill_rect(win_x, win_y + win_h - 2, win_w, 2, c_black);

    // 4. Title Bar
    int title_h = 24;
    boot_framebuffer_fill_rect(win_x + 4, win_y + 4, win_w - 8, title_h, c_title);
    boot_framebuffer_draw_string(win_x + 12, win_y + 8, OS_FULL_STRING, c_white, c_title);

    // 5. Content text lines
    int text_x = win_x + 20;
    int text_y = win_y + 36;

    boot_framebuffer_draw_string(text_x, text_y, "Pixel Neko OS 64-Bit Core Bring-up (P4b Complete)", c_title, c_gray);
    text_y += 20;
    boot_framebuffer_draw_string(text_x, text_y, "Architecture: AMD64, Ring 0 / Ring 3, IST1 #DF, IST2 NMI", c_black, c_gray);
    text_y += 18;

    // CPU Info
    boot_framebuffer_draw_string(text_x, text_y, "CPU Detected: ", c_black, c_gray);
    boot_framebuffer_draw_string(text_x + 104, text_y, g_cpu_info.vendor, c_green, c_gray);
    boot_framebuffer_draw_string(text_x + 210, text_y, cpu_model_string, c_black, c_gray);
    text_y += 18;

    // Memory Info
    uint64_t ram_mb = (pmm_get_total_page_count() * 4096) / (1024 * 1024);
    uint64_t free_mb = (pmm_get_free_page_count() * 4096) / (1024 * 1024);
    char mem_str[64];
    int m_pos = 0;
    uint64_t temp = ram_mb;
    if (temp == 0) mem_str[m_pos++] = '0';
    char digits[16];
    int d_cnt = 0;
    while (temp > 0) {
        digits[d_cnt++] = '0' + (temp % 10);
        temp /= 10;
    }
    while (d_cnt > 0) mem_str[m_pos++] = digits[--d_cnt];
    mem_str[m_pos++] = ' ';
    mem_str[m_pos++] = 'M';
    mem_str[m_pos++] = 'B';
    mem_str[m_pos++] = ' ';
    mem_str[m_pos++] = '(';
    temp = free_mb;
    if (temp == 0) mem_str[m_pos++] = '0';
    d_cnt = 0;
    while (temp > 0) {
        digits[d_cnt++] = '0' + (temp % 10);
        temp /= 10;
    }
    while (d_cnt > 0) mem_str[m_pos++] = digits[--d_cnt];
    mem_str[m_pos++] = ' ';
    mem_str[m_pos++] = 'M';
    mem_str[m_pos++] = 'B';
    mem_str[m_pos++] = ' ';
    mem_str[m_pos++] = 'F';
    mem_str[m_pos++] = 'r';
    mem_str[m_pos++] = 'e';
    mem_str[m_pos++] = 'e';
    mem_str[m_pos++] = ')';
    mem_str[m_pos] = '\0';

    boot_framebuffer_draw_string(text_x, text_y, "PMM Memory  : ", c_black, c_gray);
    boot_framebuffer_draw_string(text_x + 104, text_y, mem_str, c_black, c_gray);
    text_y += 18;

    // VMM & Graphics Info
    boot_framebuffer_draw_string(text_x, text_y, "VMM / Video : 4-Level Paging, PAT Write-Combining Active", c_black, c_gray);
    text_y += 18;

    // ACPI & APIC Info
    boot_framebuffer_draw_string(text_x, text_y, "Interrupts  : Modern APIC (LAPIC Timer Vector 32, IOAPIC Active)", c_black, c_gray);
    text_y += 18;

    // DMA Info
    boot_framebuffer_draw_string(text_x, text_y, "P4b USB     : UHCI/OHCI/EHCI/xHCI, Hub, HID Hotplug", c_black, c_gray);
    text_y += 22;

    // Status Banner
    boot_framebuffer_fill_rect(text_x, text_y, win_w - 40, 26, c_white);
    boot_framebuffer_fill_rect(text_x, text_y, win_w - 40, 1, c_dgray);
    boot_framebuffer_fill_rect(text_x, text_y, 1, 26, c_dgray);
    boot_framebuffer_draw_string(text_x + 8, text_y + 5, "[OK] P4b Full-Stack USB Subsystem Operational!", c_green, c_white);
    text_y += 34;

    // Barber-Pole Progress Bar
    g_bar_x = text_x;
    g_bar_y = text_y;
    g_bar_w = win_w - 40;
    g_bar_h = 16;
    draw_barber_pole(g_bar_x, g_bar_y, g_bar_w, g_bar_h, 0);
    text_y += 22;

    // Hint text
    boot_framebuffer_draw_string(text_x, text_y, "Launching PID 1 ELF process and IPC echo service...", c_dgray, c_gray);

    // Button
    g_btn_w = 90;
    g_btn_h = 24;
    g_btn_x = win_x + (win_w - g_btn_w) / 2;
    g_btn_y = win_y + win_h - 32;
    draw_button(g_btn_x, g_btn_y, g_btn_w, g_btn_h, false);
}

// --- 64-Bit Kernel Main Entry Point ---

void kmain(void) {
    // 1. Initialize serial port logger (COM1 @ 0x3F8)
    log_init();

    if (cmdline_contains("verbose_boot=1") || cmdline_contains("boot_splash=classic")) {
        g_verbose_boot = true;
    }
    if (cmdline_contains("mode=install")) {
        ui_compat_set_install_mode(true);
        settings_set_boot_mode(SETTINGS_BOOT_INSTALLER);
    } else if (cmdline_contains("mode=local")) {
        settings_set_boot_mode(SETTINGS_BOOT_LOCAL);
    } else {
        settings_set_boot_mode(SETTINGS_BOOT_LIVE);
    }
    g_acpi_shutdown_test_requested = cmdline_contains("shutdown_test=1");
    g_acpi_fault_test = cmdline_contains("acpi_selftest=1") ||
                        g_acpi_shutdown_test_requested;
    acpi_set_uacpi_enabled(!cmdline_contains("disable_uacpi=1"));

    log_printf("\n");
    log_printf("============================================================\n");
    log_printf("  Pixel Neko OS 64-Bit Kernel (Darwin %s)\n", OS_BUILD);
    log_printf("  Codename: %s | Baseline Train: %s\n", OS_CODENAME, OS_TRAIN);
    log_printf("============================================================\n");

    // 2. Validate Limine Base Revision
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        log_printf("[FATAL] Limine base revision 3 is NOT supported by bootloader!\n");
        for (;;) { __asm__ volatile ("pause"); }
    }
    log_printf("[BOOT] Limine Base Revision 3 handshake verified.\n");

    // 3. CPUID Feature Detection & Mandatory Feature Verification
    if (!cpu_detect_and_verify()) {
        log_printf("[FATAL] CPU failed mandatory admission check! Halting system.\n");
        for (;;) { __asm__ volatile ("pause"); }
    }
    log_printf("[CPU] All mandatory 64-bit features verified (LM, NX, APIC, SYSCALL, SSE2).\n");

    // 4. Extract HHDM Offset
    if (hhdm_request.response) {
        g_hhdm.hhdm_offset = hhdm_request.response->offset;
        log_printf("[MM] Limine HHDM Base Offset: 0x%016llx\n", g_hhdm.hhdm_offset);
    } else {
        log_printf("[FATAL] Limine HHDM response missing!\n");
        for (;;) { __asm__ volatile ("pause"); }
    }

    // 5. Parse Memory Map
    parse_memory_map();

    // 6. P2.3: Initialize 64-bit GDT, Per-CPU TSS, and IST1 Double Fault Stack
    gdt_init();

    // 7. P2.4: Initialize 64-bit IDT (256 vectors, CPU exception stubs, Mac Bomb Panic)
    idt_init();

    // 8. P2.1: Initialize 64-bit Physical Memory Manager (Bitmap Allocator > 4GB)
    pmm_init();

    // 9. P2.2: Initialize 64-bit Virtual Memory Manager (4-Level Paging, PAT Write-Combining)
    vmm_init();

    // 10. Initialize Boot Framebuffer early so any fatal error/panic is immediately visible on screen!
    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
        if (boot_framebuffer_init(fb)) {
            // Accelerate Boot Framebuffer writes with PAT Write-Combining (WC)
            if (!vmm_remap_framebuffer_wc()) {
                log_printf("[BOOT WARN] Dedicated WC framebuffer mapping unavailable; using Limine mapping.\n");
            }

            if (g_verbose_boot) {
                // Classic Diagnostic Boot: Draw classic Mac OS 50% checkerboard background early
                boot_framebuffer_draw_checkerboard();
                boot_framebuffer_flush();
                boot_progress_notify("Pixel Neko OS :: Framebuffer Ready");
                log_printf("[UI] Early Boot Framebuffer active & 50%% checkerboard rendered.\n");
            } else {
                // Windows XP Boot Mode: Render clean black background, Pixel Neko emblem & XP progress track
                boot_screen_xp_init();
                boot_framebuffer_flush();
                log_printf("[UI] Windows XP-style boot screen active.\n");
            }
        }
    } else {
        log_printf("[BOOT WARN] No graphic framebuffer returned by Limine! Running in headless serial mode.\n");
    }

    // 11. P2.2: Initialize Kernel Heap
    boot_progress_notify("Pixel Neko OS :: [11/23] Initializing Heap...");
    kheap_init();

    // 12. P2.5: Initialize ACPI Subsystem (RSDP, RSDT/XSDT, MADT, MCFG)
    boot_progress_notify("Pixel Neko OS :: [12/23] Parsing ACPI Tables...");
    acpi_init();
    if (g_acpi_fault_test && !acpi_run_gas_self_test()) {
        log_printf("[ACPI TEST] FADT GAS self-test FAILED\n");
    }

    // 13. P2.6: Initialize Modern Interrupt Subsystem (IMCR, Mask 8259, Local APIC, IOAPIC)
    boot_progress_notify("Pixel Neko OS :: [13/23] Configuring APIC & Interrupts...");
    apic_init();

    // 14. P2.7: Initialize Unified 64-bit DMA Subsystem (ZONE_DMA / DMA32 / NORMAL)
    boot_progress_notify("Pixel Neko OS :: [14/23] Initializing DMA Subsystem...");
    dma_init();

    // 15. P4a.1/P4a.2: Build the bus/device/driver tree and enumerate PCIe
    boot_progress_notify("Pixel Neko OS :: [15/23] Enumerating PCI Bus...");
    device_model_init();
    bus_register("root", BUS_TYPE_ROOT);
    pci_scan_all();

    // 16. P4a.3: Initialize the unified input queue and PS/2 fallback devices
    boot_progress_notify("Pixel Neko OS :: [16/23] Probing PS/2 & Input Devices...");
    input_manager_init();
    ps2_input_init();
    int bound_drivers = driver_probe_all();
    device_tree_dump();
    log_printf("[P4A] PCI backend=%s, device(s)=%d, input drivers bound=%d.\n",
               pci_get_backend_name(), pci_get_device_count(), bound_drivers);

    // 17. P4b: Probe the full USB host-controller matrix and enumerate devices
    boot_progress_notify("Pixel Neko OS :: [17/23] Initializing USB Controllers...");
    blkdev_init();
    boot_screen_tick();
    usb_init();
    boot_screen_tick();
    log_printf("[P4B] USB controllers=%u devices=%u summary=%s HID(K=%d M=%d)\n",
               usb_get_hci_count(), usb_get_device_count(),
               usb_host_controller_summary(),
               usb_hid_keyboard_count(), usb_hid_mouse_count());

    boot_progress_notify("Pixel Neko OS :: [18/23] Probing Storage & Volumes...");
    storage_init();
    boot_screen_tick();
    vfs_init();
    boot_screen_tick();
    (void)p5_run_storage_self_test();
    boot_screen_tick();
    log_printf("[P5 ] Storage block devices=%d partitions=%d\n",
               blkdev_count(), g_partition_count);
    settings_init();

    boot_progress_notify("Pixel Neko OS :: [18b/23] Loading Hardware Drivers (PDEVICE)...");
    (void)pdv_autoload_boot_drivers(cmdline_contains("safe_mode=1"),
                                    cmdline_contains("nopdv=1"));
    vfs_mount_initial_floppies();
    boot_screen_tick();

    if (g_main_surface.framebuffer_virt) {
        g_xp_boot_active = false;
        ui_compat_init();
        splash_run();
        (void)settings_load();
    }

    // 18. Check Kernel Load Address
    if (kernel_address_request.response) {
        log_printf("[KERNEL] Phys Base: 0x%llx, Virt Base: 0x%llx\n",
                   kernel_address_request.response->physical_base,
                   kernel_address_request.response->virtual_base);
    }

    // 19. P3.2: Enable native FPU/SSE state and initialize scheduler state
    boot_progress_notify("Pixel Neko OS :: [19/23] Initializing Scheduler...");
    fpu_init();
    scheduler_init();
    process_subsystem_init();

    // 20. P3.3: Configure SYSCALL/SYSRET MSRs and GS-based per-CPU state
    boot_progress_notify("Pixel Neko OS :: [20/23] Configuring SYSCALL & MSRs...");
    syscall_init();

    // 21. P3.5: Create the message queue and shared-memory test objects
    boot_progress_notify("Pixel Neko OS :: [21/23] Initializing IPC Subsystem...");
    ipc_init();

    // 22. P3.1: Create Idle and two independent Ring 3 ELF processes
    boot_progress_notify("Pixel Neko OS :: [22/23] Spawning User ELF Processes...");
    if (!process_create_idle()) {
        boot_progress_notify("FATAL: Failed to create Idle thread!");
        log_printf("[P3 FATAL] Failed to create Idle thread.\n");
        for (;;) __asm__ volatile ("pause");
    }

    const uint8_t* user_elf = _binary_bin_init_elf_start;
    size_t user_elf_size =
        (size_t)(_binary_bin_init_elf_end - _binary_bin_init_elf_start);
    process_t* init_process =
        process_create_user(user_elf, user_elf_size, "init", "init", NULL, NULL);
    if (!init_process) {
        boot_progress_notify("FATAL: Failed to load init ELF!");
        log_printf("[P3 FATAL] Failed to load init ELF.\n");
        for (;;) __asm__ volatile ("pause");
    }

    process_t* echo_process =
        process_create_user(user_elf, user_elf_size, "ipc-echo", "init", "echo",
                            init_process);
    if (!echo_process) {
        boot_progress_notify("FATAL: Failed to load IPC echo ELF!");
        log_printf("[P3 FATAL] Failed to load IPC echo ELF.\n");
        for (;;) __asm__ volatile ("pause");
    }

    log_printf("[P3] Loaded Ring 3 processes: init PID=%u, echo PID=%u.\n",
               init_process->pid, echo_process->pid);

    if (g_main_surface.framebuffer_virt) {
        ui_desktop_start();
        if (ui_install_mode_requested()) {
            wm_installer_activate();
            log_printf("[P6 ] Graphical installer mode active.\n");
        }
        log_printf("[P6 ] Full desktop, splash and nine applications ready.\n");
    }

    log_printf("[SYSTEM] P4b milestone reached: full USB host-controller matrix active.\n");

    // 25. Hardware IRQs are enabled by the initial IRET to the selected thread.
    scheduler_start();
}
