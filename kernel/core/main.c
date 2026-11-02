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
volatile struct limine_rsdp_request rsdp_request = {
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

    if (max_phys < 0x100000000ULL) {
        max_phys = 0x100000000ULL;
    }

    g_hhdm.physical_memory_limit = max_phys;
    g_hhdm.total_usable_bytes = usable_bytes;
    g_hhdm.total_memory_bytes = max_phys;

    log_printf("[MM] Physical Memory Limit: 0x%llx (%llu MB), Usable RAM: %llu MB\n",
               g_hhdm.physical_memory_limit, g_hhdm.physical_memory_limit / (1024 * 1024),
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

// Draw a classic Windows 3.1 / Mac OS 9 style 3D modal window
static void draw_welcome_window(void) {
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
            vmm_remap_framebuffer_wc();

            // Draw classic Mac OS 50% checkerboard background early
            boot_framebuffer_draw_checkerboard();
            log_printf("[UI] Early Boot Framebuffer active & 50%% checkerboard rendered.\n");
        }
    } else {
        log_printf("[BOOT WARN] No graphic framebuffer returned by Limine! Running in headless serial mode.\n");
    }

    // 11. P2.2: Initialize Kernel Heap
    kheap_init();

    // 12. P2.5: Initialize ACPI Subsystem (RSDP, RSDT/XSDT, MADT, MCFG)
    acpi_init();

    // 13. P2.6: Initialize Modern Interrupt Subsystem (IMCR, Mask 8259, Local APIC, IOAPIC)
    apic_init();

    // 14. P2.7: Initialize Unified 64-bit DMA Subsystem (ZONE_DMA / DMA32 / NORMAL)
    dma_init();

    // 15. P4a.1/P4a.2: Build the bus/device/driver tree and enumerate PCIe
    device_model_init();
    bus_register("root", BUS_TYPE_ROOT);
    pci_scan_all();

    // 16. P4a.3: Initialize the unified input queue and PS/2 fallback devices
    input_manager_init();
    ps2_input_init();
    int bound_drivers = driver_probe_all();
    device_tree_dump();
    log_printf("[P4A] PCI backend=%s, device(s)=%d, input drivers bound=%d.\n",
               pci_get_backend_name(), pci_get_device_count(), bound_drivers);

    // 17. P4b: Probe the full USB host-controller matrix and enumerate devices
    usb_init();
    log_printf("[P4B] USB controllers=%u devices=%u summary=%s HID(K=%d M=%d)\n",
               usb_get_hci_count(), usb_get_device_count(),
               usb_host_controller_summary(),
               usb_hid_keyboard_count(), usb_hid_mouse_count());

    // 18. Check Kernel Load Address
    if (kernel_address_request.response) {
        log_printf("[KERNEL] Phys Base: 0x%llx, Virt Base: 0x%llx\n",
                   kernel_address_request.response->physical_base,
                   kernel_address_request.response->virtual_base);
    }

    // 19. P3.2: Enable native FPU/SSE state and initialize scheduler state
    fpu_init();
    scheduler_init();
    process_subsystem_init();

    // 20. P3.3: Configure SYSCALL/SYSRET MSRs and GS-based per-CPU state
    syscall_init();

    // 21. P3.5: Create the message queue and shared-memory test objects
    ipc_init();

    // 22. P3.1: Create Idle and two independent Ring 3 ELF processes
    if (!process_create_idle()) {
        log_printf("[P3 FATAL] Failed to create Idle thread.\n");
        for (;;) __asm__ volatile ("pause");
    }

    const uint8_t* user_elf = _binary_bin_init_elf_start;
    size_t user_elf_size =
        (size_t)(_binary_bin_init_elf_end - _binary_bin_init_elf_start);
    process_t* init_process =
        process_create_user(user_elf, user_elf_size, "init", "init", NULL, NULL);
    if (!init_process) {
        log_printf("[P3 FATAL] Failed to load init ELF.\n");
        for (;;) __asm__ volatile ("pause");
    }

    process_t* echo_process =
        process_create_user(user_elf, user_elf_size, "ipc-echo", "init", "echo",
                            init_process);
    if (!echo_process) {
        log_printf("[P3 FATAL] Failed to load IPC echo ELF.\n");
        for (;;) __asm__ volatile ("pause");
    }

    log_printf("[P3] Loaded Ring 3 processes: init PID=%u, echo PID=%u.\n",
               init_process->pid, echo_process->pid);

    // 23. Draw Windows 3.1 / Mac OS 9 style welcome window with P4b metrics
    if (g_main_surface.framebuffer_virt) {
        draw_welcome_window();
        log_printf("[UI] Welcome modal rendered successfully with full P4b status.\n");
    }

    // 24. Play classic Mac / Happy Neko ascending boot chord on PC Speaker
    speaker_boot_chord();

    log_printf("[SYSTEM] P4b milestone reached: full USB host-controller matrix active.\n");

    // 25. Hardware IRQs are enabled by the initial IRET to the selected thread.
    scheduler_start();
}

