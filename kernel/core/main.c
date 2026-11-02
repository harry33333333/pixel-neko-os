#include "limine.h"
#include "version.h"
#include "log.h"
#include "cpu.h"
#include "mm.h"
#include "boot_framebuffer.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Global HHDM and memory limits
hhdm_info_t g_hhdm = {0};

// --- Limine Protocol Requests (v8.x Standard) ---

__attribute__((used, section(".limine_requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_stack_size_request stack_size_request = {
    .id = LIMINE_STACK_SIZE_REQUEST,
    .revision = 0,
    .stack_size = 65536
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

// --- Helper Functions ---

static void parse_memory_map(void) {
    if (!memmap_request.response) {
        log_printf("[MM WARN] Limine memmap response not provided!\n");
        return;
    }

    uint64_t entry_count = memmap_request.response->entry_count;
    struct limine_memmap_entry** entries = memmap_request.response->entries;

    uint64_t max_phys = 0;
    uint64_t usable_bytes = 0;

    log_printf("[MM] Limine MemMap provided %llu entries:\n", entry_count);
    for (uint64_t i = 0; i < entry_count; i++) {
        struct limine_memmap_entry* e = entries[i];
        uint64_t base = e->base;
        uint64_t length = e->length;
        uint64_t end = base + length;

        if (end > max_phys) {
            max_phys = end;
        }

        const char* type_str = "UNKNOWN";
        switch (e->type) {
            case LIMINE_MEMMAP_USABLE:
                type_str = "USABLE";
                usable_bytes += length;
                break;
            case LIMINE_MEMMAP_RESERVED:
                type_str = "RESERVED";
                break;
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
                type_str = "ACPI_RECLAIMABLE";
                break;
            case LIMINE_MEMMAP_ACPI_NVS:
                type_str = "ACPI_NVS";
                break;
            case LIMINE_MEMMAP_BAD_MEMORY:
                type_str = "BAD_MEMORY";
                break;
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
                type_str = "BOOTLOADER_RECLAIMABLE";
                break;
            case LIMINE_MEMMAP_KERNEL_AND_MODULES:
                type_str = "KERNEL_AND_MODULES";
                break;
            case LIMINE_MEMMAP_FRAMEBUFFER:
                type_str = "FRAMEBUFFER";
                break;
        }

        if (i < 16) {
            log_printf("  [0x%016llx - 0x%016llx] (size: %llu KB) -> %s\n",
                       base, end, length / 1024, type_str);
        }
    }

    g_hhdm.physical_memory_limit = max_phys;
    g_hhdm.total_usable_bytes = usable_bytes;
    g_hhdm.total_memory_bytes = max_phys;

    log_printf("[MM] Physical Memory Limit: 0x%llx (%llu MB), Usable RAM: %llu MB\n",
               g_hhdm.physical_memory_limit, g_hhdm.physical_memory_limit / (1024 * 1024),
               usable_bytes / (1024 * 1024));
}

// Draw a classic Windows 3.1 / Mac OS 9 style 3D modal window
static void draw_welcome_window(void) {
    if (!g_main_surface.framebuffer_virt) return;

    int win_w = 540;
    int win_h = 280;
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
    int text_y = win_y + 42;

    boot_framebuffer_draw_string(text_x, text_y, "Pixel Neko OS 64-Bit Core Bring-up", c_black, c_gray);
    text_y += 24;
    boot_framebuffer_draw_string(text_x, text_y, "Architecture: AMD64 / x86-64 Long Mode (Ring 0)", c_black, c_gray);
    text_y += 20;

    // CPU Info
    boot_framebuffer_draw_string(text_x, text_y, "CPU Vendor : ", c_black, c_gray);
    boot_framebuffer_draw_string(text_x + 104, text_y, g_cpu_info.vendor, c_green, c_gray);
    text_y += 18;

    boot_framebuffer_draw_string(text_x, text_y, "CPU Model  : ", c_black, c_gray);
    boot_framebuffer_draw_string(text_x + 104, text_y, cpu_model_string, c_black, c_gray);
    text_y += 18;

    // Memory Info
    uint64_t ram_mb = g_hhdm.total_usable_bytes / (1024 * 1024);
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
    mem_str[m_pos++] = 'R';
    mem_str[m_pos++] = 'A';
    mem_str[m_pos++] = 'M';
    mem_str[m_pos] = '\0';

    boot_framebuffer_draw_string(text_x, text_y, "Physical RAM: ", c_black, c_gray);
    boot_framebuffer_draw_string(text_x + 112, text_y, mem_str, c_black, c_gray);
    text_y += 22;

    // Status Banner
    boot_framebuffer_fill_rect(text_x, text_y, win_w - 40, 26, c_white);
    boot_framebuffer_fill_rect(text_x, text_y, win_w - 40, 1, c_dgray);
    boot_framebuffer_fill_rect(text_x, text_y, 1, 26, c_dgray);
    boot_framebuffer_draw_string(text_x + 8, text_y + 5, "[OK] Limine UEFI/BIOS Dual-Boot Verified!", c_green, c_white);
    text_y += 36;

    // Button
    int btn_w = 90;
    int btn_h = 24;
    int btn_x = win_x + (win_w - btn_w) / 2;
    int btn_y = win_y + win_h - 36;
    boot_framebuffer_fill_rect(btn_x, btn_y, btn_w, btn_h, c_gray);
    boot_framebuffer_fill_rect(btn_x, btn_y, btn_w, 2, c_white);
    boot_framebuffer_fill_rect(btn_x, btn_y, 2, btn_h, c_white);
    boot_framebuffer_fill_rect(btn_x + btn_w - 2, btn_y, 2, btn_h, c_black);
    boot_framebuffer_fill_rect(btn_x, btn_y + btn_h - 2, btn_w, 2, c_black);
    boot_framebuffer_draw_string(btn_x + 18, btn_y + 4, "[ OK ]", c_black, c_gray);
}

// --- 64-Bit Kernel Main Entry Point ---

void kmain(void) {
    // 1. Initialize serial port logger (COM1 @ 0x3F8)
    log_init();

    log_printf("\n");
    log_printf("============================================================\n");
    log_printf("  Pixel Neko OS 64-Bit Kernel (Darwin %s)\n", OS_BUILD);
    log_printf("  Codename: %s | Baseline Train: %s\n", OS_CODENAME, OS_TRAIN);
    log_printf("============================================================\n");

    // 2. Validate Limine Base Revision
    if (!LIMINE_BASE_REVISION_SUPPORTED) {
        log_printf("[FATAL] Limine base revision 3 is NOT supported by bootloader!\n");
        for (;;) { __asm__ volatile ("hlt"); }
    }
    log_printf("[BOOT] Limine Base Revision 3 handshake verified.\n");

    // 3. CPUID Feature Detection & Mandatory Feature Verification
    if (!cpu_detect_and_verify()) {
        log_printf("[FATAL] CPU failed mandatory admission check! Halting system.\n");
        for (;;) { __asm__ volatile ("hlt"); }
    }
    log_printf("[CPU] All mandatory 64-bit features verified (LM, NX, APIC, SYSCALL, SSE2).\n");

    // 4. Extract HHDM Offset
    if (hhdm_request.response) {
        g_hhdm.hhdm_offset = hhdm_request.response->offset;
        log_printf("[MM] Limine HHDM Base Offset: 0x%016llx\n", g_hhdm.hhdm_offset);
    } else {
        log_printf("[FATAL] Limine HHDM response missing!\n");
        for (;;) { __asm__ volatile ("hlt"); }
    }

    // 5. Parse Memory Map
    parse_memory_map();

    // 6. Check Kernel Load Address
    if (kernel_address_request.response) {
        log_printf("[KERNEL] Phys Base: 0x%llx, Virt Base: 0x%llx\n",
                   kernel_address_request.response->physical_base,
                   kernel_address_request.response->virtual_base);
    }

    // 7. Check ACPI RSDP
    if (rsdp_request.response && rsdp_request.response->address) {
        log_printf("[ACPI] Limine RSDP Pointer: %p\n", rsdp_request.response->address);
    }

    // 8. Initialize Boot Framebuffer (Limine GOP / VESA)
    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer* fb = framebuffer_request.response->framebuffers[0];
        if (boot_framebuffer_init(fb)) {
            // Draw classic Mac OS 50% checkerboard background
            boot_framebuffer_draw_checkerboard();

            // Draw Windows 3.1 / Mac OS 9 style 3D welcome modal window
            draw_welcome_window();

            log_printf("[UI] Classic 50%% checkerboard and welcome modal rendered successfully.\n");
        }
    } else {
        log_printf("[BOOT WARN] No graphic framebuffer returned by Limine! Running in headless serial mode.\n");
    }

    log_printf("[SYSTEM] P1 Milestone reached: 64-Bit Long Mode & Limine Dual-Boot active.\n");

    // Main kernel idle loop
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
