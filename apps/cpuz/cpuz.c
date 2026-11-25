#include <stdarg.h>
#include <stdbool.h>

#include <pxe/abi.h>
#include <pxe/fs.h>
#include <pxe/gfx.h>
#include <pxe/gui.h>
#include <pxe/ring0.h>
#include <pxe/str.h>
#include <pxe/sys.h>

#include "libcpuid.h"

#define APP_NAME        "Pixel CPU Detect"
#define APP_VERSION     "1.0.0"
#define LIBCPUID_VERSION "0.8.2"

#define TAB_CPU         0
#define TAB_CACHES      1
#define TAB_MAINBOARD   2
#define TAB_MEMORY      3
#define TAB_GRAPHICS    4
#define TAB_ABOUT       5
#define TAB_COUNT       6

#define MAX_PCI_DEVICES 64
#define MAX_PDV_DRIVERS 32
#define REPORT_CAPACITY 65536
#define SAVE_PATH_MAX   192

#define TAB_STRIP_Y     6
#define TAB_STRIP_H     26
#define CONTENT_X       12
#define CONTENT_Y       36
#define STATUS_H        22
#define SAVE_DIALOG_W   430
#define SAVE_DIALOG_H   146

typedef struct {
    int win_id;
    int tab;
    int dirty;

    pxe_basic_sysinfo_t basic;
    pxe_display_caps_t display;
    pxe_r0_mem_info_t memory;
    pxe_r0_pci_info_t pci[MAX_PCI_DEVICES];
    int pci_count;
    pxe_r0_pdv_info_t pdv[MAX_PDV_DRIVERS];
    int pdv_count;

    struct cpu_raw_data_t raw;
    struct cpu_id_t cpu;
    hypervisor_vendor_t hypervisor;
    int cpuid_ok;
    int identify_ok;
    char cpu_error[96];

    char feature_base[192];
    char feature_vector[192];
    char feature_security[192];

    int save_modal;
    char save_path[SAVE_PATH_MAX];
    int save_path_len;
    char save_error[128];

    char notice[128];
    int notice_is_error;
    int zh;
    int ime_phrases;
} cpuz_app_t;

static cpuz_app_t g_app;
static char g_report[REPORT_CAPACITY];
static size_t g_report_length;

static const char* const k_tab_names[TAB_COUNT] = {
    "CPU", "Caches", "Mainboard", "Memory", "Graphics", "About"
};
static const char* const k_tab_names_zh[TAB_COUNT] = {
    "处理器", "缓存", "主板", "内存", "显卡", "关于"
};

static const int k_tab_x[TAB_COUNT] = { 8, 68, 138, 220, 286, 360 };
static const int k_tab_w[TAB_COUNT] = { 56, 66, 78, 62, 70, 58 };

static void copy_text(char* target, size_t capacity, const char* source) {
    if (!target || !capacity) return;
    size_t used = 0;
    if (source) {
        while (source[used] && used + 1 < capacity) {
            target[used] = source[used];
            used++;
        }
    }
    target[used] = '\0';
}

static const char* text_or(const char* value, const char* fallback) {
    return value && value[0] ? value : fallback;
}

static char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static bool text_ends_with_ci(const char* text, const char* suffix) {
    if (!text || !suffix) return false;
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length) return false;
    const char* tail = text + text_length - suffix_length;
    for (size_t index = 0; index < suffix_length; index++) {
        if (ascii_lower(tail[index]) != ascii_lower(suffix[index])) {
            return false;
        }
    }
    return true;
}

static void append_text(char* target, size_t capacity, const char* text) {
    if (!target || !capacity || !text) return;
    size_t used = strlen(target);
    if (used >= capacity - 1) return;
    if (used && used + 1 < capacity) {
        target[used++] = ',';
        target[used++] = ' ';
        target[used] = '\0';
    }
    copy_text(target + used, capacity - used, text);
}

static void clipped_text(char* target, size_t capacity, const char* source,
                         int pixels) {
    if (!target || !capacity) return;
    if (!source) source = "";
    int max_chars = pixels / 6;
    if (max_chars < 1) max_chars = 1;
    size_t length = strlen(source);
    if ((int)length <= max_chars) {
        copy_text(target, capacity, source);
        return;
    }
    if (max_chars <= 3) {
        target[0] = '.';
        target[1] = '\0';
        return;
    }
    size_t keep = (size_t)(max_chars - 3);
    if (keep >= capacity) keep = capacity - 1;
    memcpy(target, source, keep);
    target[keep] = '\0';
    copy_text(target + keep, capacity - keep, "...");
}

static void draw_clipped(const pxe_win_surface_t* surf, int x, int y,
                         const char* text, uint32_t color, int pixels) {
    char clipped[192];
    clipped_text(clipped, sizeof(clipped), text, pixels);
    pxe_draw_text_small(surf, x, y, clipped, color);
}

static void draw_group(const pxe_win_surface_t* surf, int x, int y, int w,
                       int h, const char* title) {
    pxe_draw_bevel_box(surf, x, y, w, h, 0, PXE_COL_LIGHT_GRAY);
    pxe_draw_text_small(surf, x + 8, y + 4, title, PXE_COL_DARK_BLUE);
    pxe_draw_hline(surf, x + 6, y + 17, w - 12, PXE_COL_DARK_GRAY);
}

static void draw_value(const pxe_win_surface_t* surf, int x, int y, int width,
                       const char* text, uint32_t color) {
    draw_clipped(surf, x, y, text, color, width);
}

static void draw_kv(const pxe_win_surface_t* surf, int x, int y, int label_width,
                    int value_width, const char* label, const char* value,
                    uint32_t value_color) {
    pxe_draw_text_small(surf, x, y, label, PXE_COL_DARK_GRAY);
    draw_value(surf, x + label_width, y, value_width, value, value_color);
}

static const char* boot_mode_name(int mode) {
    if (mode == 2) return "Installed disk";
    if (mode == 1) return "Installer";
    return "Live media";
}

static const char* hypervisor_name(hypervisor_vendor_t hypervisor) {
    switch (hypervisor) {
        case HYPERVISOR_NONE:        return "Bare Metal";
        case HYPERVISOR_BHYVE:       return "bhyve";
        case HYPERVISOR_HYPERV:      return "Hyper-V";
        case HYPERVISOR_KVM:         return "KVM";
        case HYPERVISOR_PARALLELS:   return "Parallels";
        case HYPERVISOR_QEMU:        return "QEMU";
        case HYPERVISOR_VIRTUALBOX:  return "VirtualBox";
        case HYPERVISOR_VMWARE:      return "VMware";
        case HYPERVISOR_XEN:         return "Xen";
        default:                     return "Unknown";
    }
}

static const char* pci_vendor_name(uint16_t vendor) {
    switch (vendor) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x1002: return "ATI / AMD";
        case 0x10DE: return "NVIDIA";
        case 0x7377: return "Colorful";
        case 0x1458: return "Gigabyte";
        case 0x1043: return "ASUS";
        case 0x1462: return "MSI";
        case 0x3842: return "EVGA";
        case 0x19DA: return "Zotac";
        case 0x1569: return "Palit";
        case 0x10B0: return "Gainward";
        case 0x1106: return "VIA";
        case 0x10B9: return "ALi";
        case 0x1039: return "SiS";
        case 0x15AD: return "VMware";
        case 0x1234: return "QEMU";
        case 0x1AF4: return "Red Hat";
        case 0x1B21: return "ASMedia";
        default:     return "Unknown";
    }
}

static const char* pci_gpu_chip_summary(const pxe_r0_pci_info_t* pci) {
    if (!pci) return NULL;
    if (pci->vendor_id == 0x10DE) {
        switch (pci->device_id) {
            case 0x1C02:
                return "GP106-300-A1 (16nm Pascal, 1152 Shaders, 3072MB GDDR5 192-bit)";
            case 0x1C03:
                return "GP106-400-A1 (16nm Pascal, 1280 Shaders, 6144MB GDDR5 192-bit)";
            case 0x1C06:
                return "GP106-350-K1-A1 (16nm Pascal, 1280 Shaders, 5120MB GDDR5 160-bit)";
            case 0x0393:
            case 0x0395:
            case 0x02E2:
                return "G73 (90nm GeForce 7300 GT, 256MB DDR2/GDDR3 128-bit)";
            case 0x0391:
                return "G73 (90nm GeForce 7600 GT, 256MB GDDR3 128-bit)";
            case 0x0392:
                return "G73 (90nm GeForce 7600 GS, 256MB DDR2 128-bit)";
            case 0x01DF:
            case 0x01D1:
            case 0x01D3:
                return "G72 (90nm GeForce 7300 Series, 64-bit)";
            case 0x0142:
            case 0x00F4:
                return "NV43 (110nm GeForce 6600 LE, 4 Pixel/3 Vertex Shaders, 128MB GDDR3 128-bit)";
            case 0x0163:
                return "NV44 (110nm GeForce 6200 LE, 128MB 64-bit)";
            default:
                break;
        }
    } else if (pci->vendor_id == 0x1002 && pci->device_id == 0x4755) {
        return "Mach64 GT-B (3D Rage II+DVD, 64-bit SGRAM/EDO)";
    }
    return NULL;
}

static void format_bytes(uint64_t bytes, char* out, size_t capacity) {
    if (bytes >= 1073741824ull) {
        snprintf(out, capacity, "%u.%u GB",
                 (unsigned)(bytes / 1073741824ull),
                 (unsigned)(((bytes % 1073741824ull) * 10ull) /
                            1073741824ull));
    } else if (bytes >= 1048576ull) {
        snprintf(out, capacity, "%u.%u MB",
                 (unsigned)(bytes / 1048576ull),
                 (unsigned)(((bytes % 1048576ull) * 10ull) / 1048576ull));
    } else if (bytes >= 1024ull) {
        snprintf(out, capacity, "%u.%u KB",
                 (unsigned)(bytes / 1024ull),
                 (unsigned)(((bytes % 1024ull) * 10ull) / 1024ull));
    } else {
        snprintf(out, capacity, "%u B", (unsigned)bytes);
    }
}

static void format_cache(int size_kb, int instances, char* out,
                         size_t capacity) {
    if (size_kb < 0) {
        copy_text(out, capacity, "Not reported");
    } else if (size_kb == 0) {
        copy_text(out, capacity, "Not present");
    } else if (instances > 1) {
        snprintf(out, capacity, "%d x %d KB", instances, size_kb);
    } else {
        snprintf(out, capacity, "%d KB", size_kb);
    }
}

static void format_assoc(int assoc, char* out, size_t capacity) {
    if (assoc > 0) snprintf(out, capacity, "%d-way", assoc);
    else copy_text(out, capacity, "Unknown");
}

static void format_line(int line, char* out, size_t capacity) {
    if (line > 0) snprintf(out, capacity, "%d bytes", line);
    else copy_text(out, capacity, "Unknown");
}

static void format_instances(int instances, char* out, size_t capacity) {
    if (instances > 0) snprintf(out, capacity, "%d", instances);
    else copy_text(out, capacity, "Unknown");
}

static const pxe_r0_pci_info_t* find_pci(const cpuz_app_t* app, int class_code,
                                         int subclass) {
    for (int index = 0; index < app->pci_count; index++) {
        if ((class_code < 0 || app->pci[index].class_code == class_code) &&
            (subclass < 0 || app->pci[index].subclass == subclass)) {
            return &app->pci[index];
        }
    }
    return NULL;
}

static const pxe_r0_pci_info_t* find_host_bridge(const cpuz_app_t* app) {
    for (int index = 0; index < app->pci_count; index++) {
        const pxe_r0_pci_info_t* pci = &app->pci[index];
        if (pci->bus == 0 && pci->slot == 0 && pci->function == 0) return pci;
    }
    return find_pci(app, 6, 0);
}

static const pxe_r0_pci_info_t* find_pch(const cpuz_app_t* app) {
    return find_pci(app, 6, 1);
}

static const pxe_r0_pci_info_t* find_graphics(const cpuz_app_t* app) {
    return find_pci(app, 3, -1);
}

static const pxe_r0_pci_info_t* find_memory_controller(const cpuz_app_t* app) {
    return find_pci(app, 5, -1);
}

static void build_feature_lists(cpuz_app_t* app) {
    static const struct {
        cpu_feature_t feature;
        const char* label;
    } base_features[] = {
        { CPU_FEATURE_FPU, "FPU" },
        { CPU_FEATURE_MMX, "MMX" },
        { CPU_FEATURE_SSE, "SSE" },
        { CPU_FEATURE_SSE2, "SSE2" },
        { CPU_FEATURE_PNI, "SSE3" },
        { CPU_FEATURE_SSSE3, "SSSE3" },
        { CPU_FEATURE_SSE4_1, "SSE4.1" },
        { CPU_FEATURE_SSE4_2, "SSE4.2" },
        { CPU_FEATURE_SSE4A, "SSE4a" },
        { CPU_FEATURE_X2APIC, "x2APIC" }
    };
    static const struct {
        cpu_feature_t feature;
        const char* label;
    } vector_features[] = {
        { CPU_FEATURE_AVX, "AVX" },
        { CPU_FEATURE_AVX2, "AVX2" },
        { CPU_FEATURE_AVX512F, "AVX-512F" },
        { CPU_FEATURE_AVX512BW, "AVX-512BW" },
        { CPU_FEATURE_AVX512VL, "AVX-512VL" },
        { CPU_FEATURE_FMA3, "FMA3" },
        { CPU_FEATURE_BMI1, "BMI1" },
        { CPU_FEATURE_BMI2, "BMI2" },
        { CPU_FEATURE_F16C, "F16C" },
        { CPU_FEATURE_XSAVE, "XSAVE" }
    };
    static const struct {
        cpu_feature_t feature;
        const char* label;
    } security_features[] = {
        { CPU_FEATURE_AES, "AES" },
        { CPU_FEATURE_SHA_NI, "SHA" },
        { CPU_FEATURE_RDRAND, "RDRAND" },
        { CPU_FEATURE_NX, "NX" },
        { CPU_FEATURE_LM, "x86-64" },
        { CPU_FEATURE_VMX, "VMX" },
        { CPU_FEATURE_SVM, "SVM" },
        { CPU_FEATURE_HYPERVISOR, "HYPERVISOR" }
    };

    app->feature_base[0] = '\0';
    app->feature_vector[0] = '\0';
    app->feature_security[0] = '\0';

    for (size_t index = 0;
         index < sizeof(base_features) / sizeof(base_features[0]); index++) {
        if (app->cpu.flags[base_features[index].feature]) {
            append_text(app->feature_base, sizeof(app->feature_base),
                        base_features[index].label);
        }
    }
    for (size_t index = 0;
         index < sizeof(vector_features) / sizeof(vector_features[0]); index++) {
        if (app->cpu.flags[vector_features[index].feature]) {
            append_text(app->feature_vector, sizeof(app->feature_vector),
                        vector_features[index].label);
        }
    }
    for (size_t index = 0;
         index < sizeof(security_features) / sizeof(security_features[0]);
         index++) {
        if (app->cpu.flags[security_features[index].feature]) {
            append_text(app->feature_security, sizeof(app->feature_security),
                        security_features[index].label);
        }
    }

    if (!app->feature_base[0]) copy_text(app->feature_base,
                                         sizeof(app->feature_base), "Unavailable");
    if (!app->feature_vector[0]) copy_text(app->feature_vector,
                                           sizeof(app->feature_vector), "None reported");
    if (!app->feature_security[0]) copy_text(app->feature_security,
                                             sizeof(app->feature_security), "None reported");
}

static void refresh_cpu(cpuz_app_t* app) {
    memset(&app->raw, 0, sizeof(app->raw));
    memset(&app->cpu, 0, sizeof(app->cpu));
    app->cpuid_ok = 0;
    app->identify_ok = 0;
    app->hypervisor = HYPERVISOR_UNKNOWN;
    app->cpu_error[0] = '\0';

    int result = cpuid_get_raw_data(&app->raw);
    if (result != ERR_OK) {
        copy_text(app->cpu_error, sizeof(app->cpu_error), cpuid_error());
        build_feature_lists(app);
        return;
    }
    app->cpuid_ok = 1;

    result = cpu_identify(&app->raw, &app->cpu);
    if (result != ERR_OK) {
        copy_text(app->cpu_error, sizeof(app->cpu_error), cpuid_error());
    } else {
        app->identify_ok = 1;
    }

    app->hypervisor = cpuid_get_hypervisor(&app->raw, &app->cpu);
    build_feature_lists(app);
}

static void refresh_dynamic(cpuz_app_t* app) {
    pxe_sys_get_basic_info(&app->basic);
    pxe_r0_get_memory_summary(&app->memory);
}

static void refresh_all(cpuz_app_t* app) {
    refresh_cpu(app);
    memset(&app->basic, 0, sizeof(app->basic));
    pxe_sys_get_basic_info(&app->basic);

    memset(&app->display, 0, sizeof(app->display));
    (void)pxe_display_get_caps(&app->display);

    memset(&app->memory, 0, sizeof(app->memory));
    pxe_r0_get_memory_summary(&app->memory);

    int count = pxe_r0_enum_pci_devices(app->pci, MAX_PCI_DEVICES);
    app->pci_count = count > 0 ? count : 0;
    if (app->pci_count > MAX_PCI_DEVICES) app->pci_count = MAX_PCI_DEVICES;

    count = pxe_r0_enum_pdv_drivers(app->pdv, MAX_PDV_DRIVERS);
    app->pdv_count = count > 0 ? count : 0;
    if (app->pdv_count > MAX_PDV_DRIVERS) app->pdv_count = MAX_PDV_DRIVERS;
    app->ime_phrases = pxe_get_ime_phrase_count();
}

static void reportf(const char* format, ...) {
    if (!format || g_report_length >= REPORT_CAPACITY - 1) return;
    va_list arguments;
    va_start(arguments, format);
    size_t remaining = REPORT_CAPACITY - g_report_length;
    int written = vsnprintf(g_report + g_report_length, remaining, format,
                            arguments);
    va_end(arguments);
    if (written < 0) return;
    if ((size_t)written >= remaining) {
        g_report_length = REPORT_CAPACITY - 1;
    } else {
        g_report_length += (size_t)written;
    }
    g_report[g_report_length] = '\0';
}

static void report_pci(const pxe_r0_pci_info_t* pci) {
    if (!pci) {
        reportf("  N/A\n");
        return;
    }
    reportf("  PCI address: %02X:%02X.%X\n", (unsigned)pci->bus,
            (unsigned)pci->slot, (unsigned)pci->function);
    reportf("  Vendor / device: %04X:%04X (%s)\n",
            (unsigned)pci->vendor_id, (unsigned)pci->device_id,
            pci_vendor_name(pci->vendor_id));
    reportf("  Subsystem: %04X:%04X  Revision: %02X\n",
            (unsigned)pci->subsys_vendor, (unsigned)pci->subsys_device,
            (unsigned)pci->revision);
    reportf("  Class: %02X.%02X.%02X  IRQ: %u  BAR0: 0x%llX (%u KB)\n",
            (unsigned)pci->class_code, (unsigned)pci->subclass,
            (unsigned)pci->prog_if, (unsigned)pci->irq_line,
            (unsigned long long)pci->bar0_addr,
            (unsigned)(pci->bar0_size / 1024ull));
}

static void build_report(cpuz_app_t* app) {
    memset(g_report, 0, sizeof(g_report));
    g_report_length = 0;

    reportf("Pixel CPU Detect %s report\n", APP_VERSION);
    pxe_rtc_time_t now;
    memset(&now, 0, sizeof(now));
    if (pxe_get_rtc_time(&now) == 0) {
        reportf("Generated: %04u-%02u-%02u %02u:%02u:%02u\n",
                (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
                (unsigned)now.hour, (unsigned)now.minute,
                (unsigned)now.second);
    }
    reportf("System: %s\n", text_or(app->basic.os_name, "Pixel Neko OS"));
    reportf("OS build: %s  Codename: %s\n",
            text_or(app->basic.os_build, "Unknown"),
            text_or(app->basic.os_codename, "Unknown"));
    reportf("Boot mode: %s  System drive: %c:\n",
            boot_mode_name(app->basic.boot_mode),
            app->basic.system_drive ? app->basic.system_drive : '?');
    reportf("Execution: Ring 0 PXE application\n");
    reportf("CPUID library: libcpuid %s (BSD-2-Clause)\n\n", LIBCPUID_VERSION);

    reportf("[Processor]\n");
    if (app->cpuid_ok) {
        reportf("Name: %s\n", text_or(app->cpu.brand_str, "Unknown"));
        reportf("Vendor: %s (%s)\n", text_or(app->cpu.vendor_str, "Unknown"),
                text_or(cpu_architecture_str(app->cpu.architecture), "Unknown"));
        reportf("Code name: %s\n", text_or(app->cpu.cpu_codename, "Unknown"));
        reportf("Technology: %s\n", text_or(app->cpu.technology_node, "Unknown"));
        reportf("Family / model / stepping: %X / %X / %X\n",
                (unsigned)app->cpu.x86.family, (unsigned)app->cpu.x86.model,
                (unsigned)app->cpu.x86.stepping);
        reportf("Extended family / model: %X / %X\n",
                (unsigned)app->cpu.x86.ext_family,
                (unsigned)app->cpu.x86.ext_model);
        reportf("Purpose: %s\n", cpu_purpose_str(app->cpu.purpose));
        reportf("Feature level: %s\n",
                cpu_feature_level_str(app->cpu.feature_level));
        reportf("Cores / logical CPUs / total logical: %d / %d / %d\n",
                app->cpu.num_cores, app->cpu.num_logical_cpus,
                app->cpu.total_logical_cpus);
        reportf("Kernel reported clock: %u MHz\n", app->basic.cpu_freq_mhz);
        reportf("Hypervisor: %s\n", hypervisor_name(app->hypervisor));
    } else {
        reportf("CPUID status: %s\n",
                app->cpu_error[0] ? app->cpu_error : "Initialization failed");
    }

    reportf("Features:");
    int feature_count = 0;
    for (int feature = 0; feature < NUM_CPU_FEATURES; feature++) {
        if (!app->cpu.flags[feature]) continue;
        const char* name = cpu_feature_str((cpu_feature_t)feature);
        if (!name || !name[0]) continue;
        reportf("%s%s", feature_count ? ", " : " ", name);
        feature_count++;
    }
    if (!feature_count) reportf(" Not available");
    reportf("\n\n");

    reportf("[Caches]\n");
    reportf("L1 data: %d KB, assoc %d, line %d, instances %d\n",
            app->cpu.l1_data_cache, app->cpu.l1_data_assoc,
            app->cpu.l1_data_cacheline, app->cpu.l1_data_instances);
    reportf("L1 instruction: %d KB, assoc %d, line %d, instances %d\n",
            app->cpu.l1_instruction_cache, app->cpu.l1_instruction_assoc,
            app->cpu.l1_instruction_cacheline,
            app->cpu.l1_instruction_instances);
    reportf("L2: %d KB, assoc %d, line %d, instances %d\n",
            app->cpu.l2_cache, app->cpu.l2_assoc, app->cpu.l2_cacheline,
            app->cpu.l2_instances);
    reportf("L3: %d KB, assoc %d, line %d, instances %d\n",
            app->cpu.l3_cache, app->cpu.l3_assoc, app->cpu.l3_cacheline,
            app->cpu.l3_instances);
    reportf("L4: %d KB, assoc %d, line %d, instances %d\n\n",
            app->cpu.l4_cache, app->cpu.l4_assoc, app->cpu.l4_cacheline,
            app->cpu.l4_instances);

    reportf("[Raw CPUID leaves]\n");
    for (int leaf = 0; leaf < MAX_CPUID_LEVEL; leaf++) {
        reportf("basic_cpuid[%02d] = %08X %08X %08X %08X\n", leaf,
                (unsigned)app->raw.basic_cpuid[leaf][EAX],
                (unsigned)app->raw.basic_cpuid[leaf][EBX],
                (unsigned)app->raw.basic_cpuid[leaf][ECX],
                (unsigned)app->raw.basic_cpuid[leaf][EDX]);
    }
    for (int leaf = 0; leaf < MAX_EXT_CPUID_LEVEL; leaf++) {
        reportf("ext_cpuid[%02d]   = %08X %08X %08X %08X\n", leaf,
                (unsigned)app->raw.ext_cpuid[leaf][EAX],
                (unsigned)app->raw.ext_cpuid[leaf][EBX],
                (unsigned)app->raw.ext_cpuid[leaf][ECX],
                (unsigned)app->raw.ext_cpuid[leaf][EDX]);
    }
    for (int leaf = 0; leaf < MAX_INTELFN4_LEVEL; leaf++) {
        reportf("intel_fn4[%d]     = %08X %08X %08X %08X\n", leaf,
                (unsigned)app->raw.intel_fn4[leaf][EAX],
                (unsigned)app->raw.intel_fn4[leaf][EBX],
                (unsigned)app->raw.intel_fn4[leaf][ECX],
                (unsigned)app->raw.intel_fn4[leaf][EDX]);
    }
    for (int leaf = 0; leaf < MAX_INTELFN11_LEVEL; leaf++) {
        reportf("intel_fn11[%d]    = %08X %08X %08X %08X\n", leaf,
                (unsigned)app->raw.intel_fn11[leaf][EAX],
                (unsigned)app->raw.intel_fn11[leaf][EBX],
                (unsigned)app->raw.intel_fn11[leaf][ECX],
                (unsigned)app->raw.intel_fn11[leaf][EDX]);
    }
    for (int leaf = 0; leaf < MAX_INTELFN12H_LEVEL; leaf++) {
        reportf("intel_fn12h[%d]   = %08X %08X %08X %08X\n", leaf,
                (unsigned)app->raw.intel_fn12h[leaf][EAX],
                (unsigned)app->raw.intel_fn12h[leaf][EBX],
                (unsigned)app->raw.intel_fn12h[leaf][ECX],
                (unsigned)app->raw.intel_fn12h[leaf][EDX]);
    }
    for (int leaf = 0; leaf < MAX_INTELFN14H_LEVEL; leaf++) {
        reportf("intel_fn14h[%d]   = %08X %08X %08X %08X\n", leaf,
                (unsigned)app->raw.intel_fn14h[leaf][EAX],
                (unsigned)app->raw.intel_fn14h[leaf][EBX],
                (unsigned)app->raw.intel_fn14h[leaf][ECX],
                (unsigned)app->raw.intel_fn14h[leaf][EDX]);
    }
    for (int leaf = 0; leaf < MAX_AMDFN8000001DH_LEVEL; leaf++) {
        reportf("amd_fn8000001d[%d] = %08X %08X %08X %08X\n", leaf,
                (unsigned)app->raw.amd_fn8000001dh[leaf][EAX],
                (unsigned)app->raw.amd_fn8000001dh[leaf][EBX],
                (unsigned)app->raw.amd_fn8000001dh[leaf][ECX],
                (unsigned)app->raw.amd_fn8000001dh[leaf][EDX]);
    }
    for (int leaf = 0; leaf < MAX_AMDFN80000026H_LEVEL; leaf++) {
        reportf("amd_fn80000026h[%d] = %08X %08X %08X %08X\n", leaf,
                (unsigned)app->raw.amd_fn80000026h[leaf][EAX],
                (unsigned)app->raw.amd_fn80000026h[leaf][EBX],
                (unsigned)app->raw.amd_fn80000026h[leaf][ECX],
                (unsigned)app->raw.amd_fn80000026h[leaf][EDX]);
    }
    reportf("\n");

    reportf("[Mainboard / firmware visible through PCI]\n");
    report_pci(find_host_bridge(app));
    reportf("  PCH / ISA bridge:\n");
    report_pci(find_pch(app));
    reportf("  Firmware identity: current PXE ABI does not expose SMBIOS board strings.\n\n");

    reportf("[Memory]\n");
    char bytes[32];
    format_bytes(app->memory.total_bytes, bytes, sizeof(bytes));
    reportf("Physical memory: %s\n", bytes);
    format_bytes(app->memory.free_bytes, bytes, sizeof(bytes));
    reportf("Free memory: %s\n", bytes);
    reportf("Pages: %llu total / %llu free / %llu used\n",
            (unsigned long long)app->memory.total_pages,
            (unsigned long long)app->memory.free_pages,
            (unsigned long long)app->memory.used_pages);
    reportf("Kernel summary: %u MB total / %u MB usable / %u MB free\n",
            (unsigned)app->basic.total_ram_mb,
            (unsigned)app->basic.usable_ram_mb,
            (unsigned)app->basic.free_ram_mb);
    reportf("Memory controller PCI device:\n");
    report_pci(find_memory_controller(app));
    reportf("\n");

    reportf("[Graphics]\n");
    const pxe_r0_pci_info_t* gpu_pci = find_graphics(app);
    report_pci(gpu_pci);
    const char* gpu_spec = pci_gpu_chip_summary(gpu_pci);
    if (gpu_spec) {
        reportf("  GPU spec: %s\n", gpu_spec);
    }
    reportf("  Active driver: %s\n",
            text_or(app->display.driver_name, "Boot framebuffer"));
    reportf("  Driver status: %s\n",
            text_or(app->display.driver_status, "Unknown"));
    reportf("  Mode: %dx%d @ %d bpp\n", app->basic.screen_w,
            app->basic.screen_h, app->basic.screen_bpp);
    format_bytes(app->basic.vram_bytes, bytes, sizeof(bytes));
    reportf("  VRAM: %s\n", bytes);
    reportf("  Backend: %s\n\n",
            text_or(app->basic.gfx_backend, "Framebuffer"));

    reportf("[All PCI devices: %d]\n", app->pci_count);
    for (int index = 0; index < app->pci_count; index++) {
        const pxe_r0_pci_info_t* pci = &app->pci[index];
        reportf("%02X:%02X.%X %04X:%04X class=%02X.%02X.%02X %s\n",
                (unsigned)pci->bus, (unsigned)pci->slot,
                (unsigned)pci->function, (unsigned)pci->vendor_id,
                (unsigned)pci->device_id, (unsigned)pci->class_code,
                (unsigned)pci->subclass, (unsigned)pci->prog_if,
                text_or(pci->class_name, "PCI Device"));
    }

    reportf("\n[Loaded PDV drivers: %d]\n", app->pdv_count);
    for (int index = 0; index < app->pdv_count; index++) {
        const pxe_r0_pdv_info_t* pdv = &app->pdv[index];
        reportf("%s  %s  version=%s  class=%u\n",
                text_or(pdv->filename, "?"),
                text_or(pdv->driver_name, "(unnamed)"),
                text_or(pdv->version_str, "?"), (unsigned)pdv->driver_class);
    }

    reportf("\nlibcpuid is Copyright (c) Veselin Georgiev and contributors.\n");
    reportf("Distributed under BSD-2-Clause; see system\\THIRDPARTY.TXT.\n");
}

static void set_default_save_path(cpuz_app_t* app) {
    if (app->save_path[0]) return;
    char drive = app->basic.system_drive;
    if (!drive) drive = app->basic.cdrom_drive;
    if (!drive) drive = 'C';
    snprintf(app->save_path, sizeof(app->save_path),
             "%c:\\CPU_DETECT.TXT", drive);
    app->save_path_len = (int)strlen(app->save_path);
}

static void open_save_dialog(cpuz_app_t* app) {
    set_default_save_path(app);
    app->save_error[0] = '\0';
    app->save_modal = 1;
}

static bool validate_save_path(cpuz_app_t* app) {
    const char* path = app->save_path;
    size_t length = strlen(path);
    app->save_error[0] = '\0';
    if (length < 7) {
        copy_text(app->save_error, sizeof(app->save_error),
                  "Enter a complete report path.");
        return false;
    }
    if (path[0] >= 'a' && path[0] <= 'z') {
        app->save_path[0] = (char)(path[0] - ('a' - 'A'));
    }
    if (path[0] < 'A' || path[0] > 'Z' ||
        path[1] != ':' || (path[2] != '\\' && path[2] != '/')) {
        copy_text(app->save_error, sizeof(app->save_error),
                  "Use an absolute path such as C:\\CPU_DETECT.TXT.");
        return false;
    }
    if (!text_ends_with_ci(path, ".TXT")) {
        copy_text(app->save_error, sizeof(app->save_error),
                  "The report filename must end in .TXT.");
        return false;
    }

    int slash = -1;
    for (int index = 2; path[index]; index++) {
        if (path[index] == '\\' || path[index] == '/') slash = index;
    }
    if (slash < 2 || path[slash + 1] == '\0') {
        copy_text(app->save_error, sizeof(app->save_error),
                  "Enter a filename after the destination folder.");
        return false;
    }

    char folder[SAVE_PATH_MAX];
    copy_text(folder, sizeof(folder), path);
    folder[slash == 2 ? 3 : (size_t)slash] = '\0';
    pxe_dirent_t entry;
    memset(&entry, 0, sizeof(entry));
    if (pxe_stat(folder, &entry) != 0 || !entry.is_dir) {
        copy_text(app->save_error, sizeof(app->save_error),
                  "The destination folder does not exist.");
        return false;
    }
    memset(&entry, 0, sizeof(entry));
    if (pxe_stat(path, &entry) == 0 && entry.is_dir) {
        copy_text(app->save_error, sizeof(app->save_error),
                  "The report path points to a folder.");
        return false;
    }
    return true;
}

static void submit_save_report(cpuz_app_t* app) {
    if (!validate_save_path(app)) {
        pxe_beep();
        return;
    }
    build_report(app);
    int result = pxe_write_file(app->save_path, g_report, g_report_length);
    if (result == 0) {
        (void)pxe_r0_sync_filesystems();
        app->save_modal = 0;
        app->save_error[0] = '\0';
        app->notice_is_error = 0;
        snprintf(app->notice, sizeof(app->notice), "Report saved to %s",
                 app->save_path);
        pxe_notify_banner(app->notice);
    } else {
        snprintf(app->save_error, sizeof(app->save_error),
                 "Could not write the report (read-only or full volume).");
        pxe_beep();
    }
}

static void append_save_path_text(cpuz_app_t* app, const char* text) {
    if (!text) return;
    for (size_t index = 0; text[index]; index++) {
        unsigned char value = (unsigned char)text[index];
        if (value < 32 || value == 127 ||
            app->save_path_len >= SAVE_PATH_MAX - 1) {
            continue;
        }
        app->save_path[app->save_path_len++] = (char)value;
        app->save_path[app->save_path_len] = '\0';
    }
}

static void paste_save_path(cpuz_app_t* app) {
    char clipboard[SAVE_PATH_MAX];
    memset(clipboard, 0, sizeof(clipboard));
    if (pxe_clipboard_get(clipboard, sizeof(clipboard) - 1) > 0) {
        append_save_path_text(app, clipboard);
    }
}

static void clock_leaf(const cpuz_app_t* app, uint32_t* base,
                       uint32_t* maximum, uint32_t* bus) {
    *base = 0;
    *maximum = 0;
    *bus = 0;
    if (app->raw.basic_cpuid[0][EAX] < 0x16) return;
    *base = app->raw.basic_cpuid[0x16][EAX] & 0xFFFFu;
    *maximum = app->raw.basic_cpuid[0x16][EBX] & 0xFFFFu;
    *bus = app->raw.basic_cpuid[0x16][ECX] & 0xFFFFu;
}

static void render_tabs(const pxe_win_surface_t* surf, const cpuz_app_t* app) {
    for (int tab = 0; tab < TAB_COUNT; tab++) {
        int active = app->tab == tab;
        int y = active ? TAB_STRIP_Y : TAB_STRIP_Y + 2;
        int h = active ? TAB_STRIP_H : TAB_STRIP_H - 3;
        uint32_t fill = active ? PXE_COL_WHITE : PXE_COL_LIGHT_GRAY;
        pxe_fill_rect(surf, k_tab_x[tab], y, k_tab_w[tab], h, fill);
        pxe_draw_hline(surf, k_tab_x[tab], y, k_tab_w[tab], PXE_COL_WHITE);
        pxe_draw_vline(surf, k_tab_x[tab], y, h, PXE_COL_WHITE);
        pxe_draw_vline(surf, k_tab_x[tab] + k_tab_w[tab] - 1, y, h,
                       PXE_COL_DARK_GRAY);
        if (!active) {
            pxe_draw_hline(surf, k_tab_x[tab], y + h - 1, k_tab_w[tab],
                           PXE_COL_DARK_GRAY);
        }
        const char* tname = app->zh ? k_tab_names_zh[tab] : k_tab_names[tab];
        int text_width = pxe_text_width_small(tname);
        pxe_draw_text_small(surf, k_tab_x[tab] +
                            (k_tab_w[tab] - text_width) / 2, y + 6,
                            tname,
                            active ? PXE_COL_DARK_BLUE : PXE_COL_BLACK);
    }
    pxe_draw_button(surf, surf->width - 66, 6, 58, 22,
                    app->zh ? "刷新" : "Refresh", 0, 1);
}

static void render_cpu(const pxe_win_surface_t* surf, cpuz_app_t* app) {
    char value[192];
    draw_group(surf, CONTENT_X, CONTENT_Y, surf->width - 24, 86,
               "Processor");
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 22, 78,
            surf->width - 112, "Name",
            text_or(app->cpu.brand_str, "CPUID recognition unavailable"),
            PXE_COL_BLACK);
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 38, 78,
            surf->width - 112, "Code Name",
            text_or(app->cpu.cpu_codename, "Unknown"), PXE_COL_BLACK);
    snprintf(value, sizeof(value), "%s / %s / %s",
             text_or(app->cpu.vendor_str, "Unknown vendor"),
             text_or(cpu_architecture_str(app->cpu.architecture), "x86-64"),
             text_or(app->cpu.technology_node, "process unknown"));
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 54, 78,
            surf->width - 112, "Package", value, PXE_COL_DARK_BLUE);

    int signature_y = CONTENT_Y + 92;
    draw_group(surf, CONTENT_X, signature_y, surf->width - 24, 104,
               "Signature and Instructions");
    const struct x86_id_t* x86 = &app->cpu.x86;
    snprintf(value, sizeof(value),
             "Family %X  Model %X  Stepping %X  Ext.Family %X  Ext.Model %X",
             (unsigned)x86->family, (unsigned)x86->model,
             (unsigned)x86->stepping, (unsigned)x86->ext_family,
             (unsigned)x86->ext_model);
    draw_kv(surf, CONTENT_X + 10, signature_y + 22, 72,
            surf->width - 104, "Signature", value, PXE_COL_BLACK);
    draw_kv(surf, CONTENT_X + 10, signature_y + 38, 72,
            surf->width - 104, "Level",
            cpu_feature_level_str(app->cpu.feature_level), PXE_COL_DARK_BLUE);
    draw_kv(surf, CONTENT_X + 10, signature_y + 54, 72,
            surf->width - 104, "Base ISA", app->feature_base,
            PXE_COL_DARK_GREEN);
    draw_kv(surf, CONTENT_X + 10, signature_y + 70, 72,
            surf->width - 104, "Vectors", app->feature_vector,
            PXE_COL_DARK_GREEN);
    draw_kv(surf, CONTENT_X + 10, signature_y + 86, 72,
            surf->width - 104, "Security",
            app->feature_security, PXE_COL_DARK_GREEN);

    int lower_y = signature_y + 110;
    int left_w = (surf->width - 36) / 2;
    draw_group(surf, CONTENT_X, lower_y, left_w, 116, "Clocks");

    uint32_t base = 0, maximum = 0, bus = 0;
    clock_leaf(app, &base, &maximum, &bus);
    if (base || maximum || bus) {
        snprintf(value, sizeof(value), "%u MHz", (unsigned)base);
        draw_kv(surf, CONTENT_X + 8, lower_y + 22, 64,
                left_w - 82, "Base", value, PXE_COL_BLACK);
        snprintf(value, sizeof(value), "%u MHz", (unsigned)maximum);
        draw_kv(surf, CONTENT_X + 8, lower_y + 38, 64,
                left_w - 82, "Max", value, PXE_COL_BLACK);
        snprintf(value, sizeof(value), "%u MHz", (unsigned)bus);
        draw_kv(surf, CONTENT_X + 8, lower_y + 54, 64,
                left_w - 82, "Bus", value, PXE_COL_BLACK);
        if (bus) {
            unsigned multiplier_x10 = (base * 10u + bus / 2u) / bus;
            snprintf(value, sizeof(value), "x %u.%u",
                     multiplier_x10 / 10u, multiplier_x10 % 10u);
            draw_kv(surf, CONTENT_X + 8, lower_y + 70, 64,
                    left_w - 82, "Multiplier", value, PXE_COL_DARK_BLUE);
        } else {
            draw_kv(surf, CONTENT_X + 8, lower_y + 70, 64,
                    left_w - 82, "Multiplier", "Unknown", PXE_COL_DARK_GRAY);
        }
    } else {
        draw_kv(surf, CONTENT_X + 8, lower_y + 22, 64,
                left_w - 82, "CPUID.16h", "Not enumerated",
                PXE_COL_DARK_GRAY);
    }
    snprintf(value, sizeof(value), "%u MHz",
             (unsigned)app->basic.cpu_freq_mhz);
    draw_kv(surf, CONTENT_X + 8, lower_y + 86, 64,
            left_w - 82, "Reported", value, PXE_COL_BLACK);
    draw_kv(surf, CONTENT_X + 8, lower_y + 102, 64,
            left_w - 82, "Source", "kernel PXE ABI", PXE_COL_DARK_GRAY);

    int right_x = CONTENT_X + left_w + 12;
    int right_w = surf->width - right_x - CONTENT_X;
    draw_group(surf, right_x, lower_y, right_w, 116, "Topology");
    snprintf(value, sizeof(value), "%d cores / %d logical CPUs",
             app->cpu.num_cores, app->cpu.num_logical_cpus);
    draw_kv(surf, right_x + 8, lower_y + 22, 58,
            right_w - 74, "CPU", value, PXE_COL_BLACK);
    draw_kv(surf, right_x + 8, lower_y + 38, 58,
            right_w - 74, "Purpose", cpu_purpose_str(app->cpu.purpose),
            PXE_COL_DARK_BLUE);
    draw_kv(surf, right_x + 8, lower_y + 54, 58,
            right_w - 74, "Environment", hypervisor_name(app->hypervisor),
            PXE_COL_BLACK);
    draw_kv(surf, right_x + 8, lower_y + 70, 58,
            right_w - 74, "Build", text_or(app->basic.os_build, "Unknown"),
            PXE_COL_BLACK);
    draw_kv(surf, right_x + 8, lower_y + 86, 58,
            right_w - 74, "Ring", "0 (privileged)", PXE_COL_DARK_GREEN);
    draw_kv(surf, right_x + 8, lower_y + 102, 58,
            right_w - 74, "Status", app->identify_ok ? "Recognized" :
            text_or(app->cpu_error, "Unknown CPU"), app->identify_ok ?
            PXE_COL_DARK_GREEN : PXE_COL_DARK_RED);
}

static void draw_cache_group(const pxe_win_surface_t* surf, int y,
                             const char* title, int size, int assoc, int line,
                             int instances) {
    char size_text[48];
    char assoc_text[32];
    char line_text[32];
    char instance_text[32];
    char descriptor[96];
    format_cache(size, instances, size_text, sizeof(size_text));
    format_assoc(assoc, assoc_text, sizeof(assoc_text));
    format_line(line, line_text, sizeof(line_text));
    format_instances(instances, instance_text, sizeof(instance_text));
    snprintf(descriptor, sizeof(descriptor), "%s, %s line",
             assoc_text, line_text);

    draw_group(surf, CONTENT_X, y, surf->width - 24, 60, title);
    draw_kv(surf, CONTENT_X + 10, y + 23, 48, 190, "Size",
            size_text, PXE_COL_BLACK);
    draw_kv(surf, CONTENT_X + 10, y + 39, 48, 300, "Descriptor",
            descriptor, PXE_COL_DARK_BLUE);
    draw_kv(surf, CONTENT_X + 350, y + 23, 62, 120, "Instances",
            instance_text, PXE_COL_BLACK);
}

static void render_caches(const pxe_win_surface_t* surf, cpuz_app_t* app) {
    int y = CONTENT_Y;
    draw_cache_group(surf, y, "L1 Data Cache", app->cpu.l1_data_cache,
                     app->cpu.l1_data_assoc, app->cpu.l1_data_cacheline,
                     app->cpu.l1_data_instances);
    y += 66;
    draw_cache_group(surf, y, "L1 Instruction Cache",
                     app->cpu.l1_instruction_cache,
                     app->cpu.l1_instruction_assoc,
                     app->cpu.l1_instruction_cacheline,
                     app->cpu.l1_instruction_instances);
    y += 66;
    draw_cache_group(surf, y, "Level 2 Cache", app->cpu.l2_cache,
                     app->cpu.l2_assoc, app->cpu.l2_cacheline,
                     app->cpu.l2_instances);
    y += 66;
    draw_cache_group(surf, y, "Level 3 Cache", app->cpu.l3_cache,
                     app->cpu.l3_assoc, app->cpu.l3_cacheline,
                     app->cpu.l3_instances);
    y += 66;
    draw_cache_group(surf, y, "Level 4 Cache", app->cpu.l4_cache,
                     app->cpu.l4_assoc, app->cpu.l4_cacheline,
                     app->cpu.l4_instances);
}

static void render_pci_identity(const pxe_win_surface_t* surf, int x, int y,
                                int width, const pxe_r0_pci_info_t* pci) {
    char value[160];
    if (!pci) {
        draw_kv(surf, x + 10, y + 22, 70, width - 90, "Status",
                "Not reported", PXE_COL_DARK_GRAY);
        return;
    }
    snprintf(value, sizeof(value), "%s %04X:%04X",
             pci_vendor_name(pci->vendor_id), (unsigned)pci->vendor_id,
             (unsigned)pci->device_id);
    draw_kv(surf, x + 10, y + 22, 70, width - 90, "Device",
            value, PXE_COL_BLACK);
    snprintf(value, sizeof(value), "%02X:%02X.%X  Subsystem %04X:%04X (%s)",
             (unsigned)pci->bus, (unsigned)pci->slot,
             (unsigned)pci->function, (unsigned)pci->subsys_vendor,
             (unsigned)pci->subsys_device,
             pci_vendor_name(pci->subsys_vendor));
    draw_kv(surf, x + 10, y + 38, 70, width - 90, "Address",
            value, PXE_COL_DARK_BLUE);
    const char* gpu_spec = (pci->class_code == 0x03) ? pci_gpu_chip_summary(pci) : NULL;
    if (gpu_spec) {
        snprintf(value, sizeof(value), "%s (rev %02X)", gpu_spec, (unsigned)pci->revision);
        draw_kv(surf, x + 10, y + 54, 70, width - 90, "GPU Spec",
                value, PXE_COL_DARK_GREEN);
    } else {
        snprintf(value, sizeof(value), "%s  rev %02X  IRQ %u",
                 text_or(pci->class_name, "PCI Device"),
                 (unsigned)pci->revision, (unsigned)pci->irq_line);
        draw_kv(surf, x + 10, y + 54, 70, width - 90, "Class",
                value, PXE_COL_BLACK);
    }
}

static void render_mainboard(const pxe_win_surface_t* surf, cpuz_app_t* app) {
    char value[192];
    int width = surf->width - 24;
    draw_group(surf, CONTENT_X, CONTENT_Y, width, 96, "System");
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 22, 84, width - 112,
            "Boot Mode", boot_mode_name(app->basic.boot_mode),
            PXE_COL_BLACK);
    snprintf(value, sizeof(value), "%c:",
             app->basic.system_drive ? app->basic.system_drive : '?');
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 38, 84, width - 112,
            "System Disk", value, PXE_COL_BLACK);
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 54, 84, width - 112,
            "OS Build", text_or(app->basic.os_build, "Unknown"),
            PXE_COL_BLACK);
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 70, 84, width - 112,
            "Kernel Code", text_or(app->basic.os_codename, "Snow Neko"),
            PXE_COL_DARK_BLUE);

    int y = CONTENT_Y + 102;
    draw_group(surf, CONTENT_X, y, width, 80, "Host Bridge");
    render_pci_identity(surf, CONTENT_X, y, width, find_host_bridge(app));

    y += 86;
    draw_group(surf, CONTENT_X, y, width, 80, "PCH / ISA Bridge");
    render_pci_identity(surf, CONTENT_X, y, width, find_pch(app));

    y += 86;
    draw_group(surf, CONTENT_X, y, width, 68, "Firmware Information");
    draw_kv(surf, CONTENT_X + 10, y + 22, 92, width - 118,
            "SMBIOS Board", "Not exposed by the current PXE ABI",
            PXE_COL_DARK_GRAY);
    draw_kv(surf, CONTENT_X + 10, y + 38, 92, width - 118,
            "Detection", "PCI IDs are read directly in Ring 0",
            PXE_COL_DARK_BLUE);
}

static void render_memory(const pxe_win_surface_t* surf, cpuz_app_t* app) {
    char total[32], free_bytes[32], used[32];
    int width = surf->width - 24;
    format_bytes(app->memory.total_bytes, total, sizeof(total));
    format_bytes(app->memory.free_bytes, free_bytes, sizeof(free_bytes));
    uint64_t used_bytes = app->memory.total_bytes > app->memory.free_bytes ?
                          app->memory.total_bytes - app->memory.free_bytes : 0;
    format_bytes(used_bytes, used, sizeof(used));

    draw_group(surf, CONTENT_X, CONTENT_Y, width, 106, "Physical Memory");
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 22, 82, width - 110,
            "Total", total, PXE_COL_BLACK);
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 38, 82, width - 110,
            "Used", used, PXE_COL_DARK_BLUE);
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 54, 82, width - 110,
            "Free", free_bytes, PXE_COL_DARK_GREEN);
    char kernel[160];
    snprintf(kernel, sizeof(kernel), "%u MB total / %u MB usable / %u MB free",
             (unsigned)app->basic.total_ram_mb,
             (unsigned)app->basic.usable_ram_mb,
             (unsigned)app->basic.free_ram_mb);
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 70, 82, width - 110,
            "Kernel", kernel, PXE_COL_BLACK);
    snprintf(kernel, sizeof(kernel), "%llu pages free of %llu",
             (unsigned long long)app->memory.free_pages,
             (unsigned long long)app->memory.total_pages);
    draw_kv(surf, CONTENT_X + 10, CONTENT_Y + 86, 82, width - 110,
            "Pages", kernel, PXE_COL_DARK_GRAY);

    int y = CONTENT_Y + 112;
    draw_group(surf, CONTENT_X, y, width, 82, "Memory Controller");
    render_pci_identity(surf, CONTENT_X, y, width,
                        find_memory_controller(app));

    y += 88;
    draw_group(surf, CONTENT_X, y, width, 94, "Memory Modules");
    draw_kv(surf, CONTENT_X + 10, y + 22, 90, width - 116,
            "SMBIOS Type 17", "Not exposed by the current PXE ABI",
            PXE_COL_DARK_GRAY);
    draw_kv(surf, CONTENT_X + 10, y + 38, 90, width - 116,
            "Alternative", "Physical usage comes from the kernel PMM",
            PXE_COL_DARK_BLUE);
    char process[64];
    snprintf(process, sizeof(process), "%llu active",
             (unsigned long long)app->memory.process_count);
    draw_kv(surf, CONTENT_X + 10, y + 54, 90, width - 116,
            "Processes", process, PXE_COL_BLACK);
    snprintf(process, sizeof(process), "%llu loaded",
             (unsigned long long)app->memory.pdv_loaded_count);
    draw_kv(surf, CONTENT_X + 10, y + 70, 90, width - 116,
            "PDV Drivers", process, PXE_COL_BLACK);
}

static void render_graphics(const pxe_win_surface_t* surf, cpuz_app_t* app) {
    char value[192];
    int width = surf->width - 24;
    draw_group(surf, CONTENT_X, CONTENT_Y, width, 92, "Display Device");
    render_pci_identity(surf, CONTENT_X, CONTENT_Y, width,
                        find_graphics(app));

    int y = CONTENT_Y + 98;
    draw_group(surf, CONTENT_X, y, width, 112, "Neko Graphics Driver");
    draw_kv(surf, CONTENT_X + 10, y + 22, 92, width - 118,
            "Active PDV",
            text_or(app->display.driver_name, "Boot framebuffer"),
            PXE_COL_DARK_BLUE);
    draw_kv(surf, CONTENT_X + 10, y + 38, 92, width - 118,
            "Status",
            text_or(app->display.driver_status, "Active / driver reported"),
            PXE_COL_DARK_GREEN);
    snprintf(value, sizeof(value), "%dx%d @ %d-bit",
             app->basic.screen_w, app->basic.screen_h,
             app->basic.screen_bpp);
    draw_kv(surf, CONTENT_X + 10, y + 54, 92, width - 118,
            "Video Mode", value, PXE_COL_BLACK);
    char vram[32];
    format_bytes(app->display.vram_bytes ?
                 app->display.vram_bytes : app->basic.vram_bytes,
                 vram, sizeof(vram));
    draw_kv(surf, CONTENT_X + 10, y + 70, 92, width - 118,
            "VRAM", vram, PXE_COL_BLACK);
    draw_kv(surf, CONTENT_X + 10, y + 86, 92, width - 118,
            "2D Engine", app->basic.gfx_is_2d_hw ? "Hardware" :
            "Software framebuffer", app->basic.gfx_is_2d_hw ?
            PXE_COL_DARK_GREEN : PXE_COL_DARK_GRAY);
    draw_kv(surf, CONTENT_X + 10, y + 102, 92, width - 118,
            "Backend",
            text_or(app->basic.gfx_backend, "Boot framebuffer"),
            PXE_COL_BLACK);

    y += 118;
    draw_group(surf, CONTENT_X, y, width, 82, "Loaded Display Drivers");
    int printed = 0;
    for (int index = 0; index < app->pdv_count && printed < 3; index++) {
        const pxe_r0_pdv_info_t* pdv = &app->pdv[index];
        if (pdv->driver_class != 1) continue;
        snprintf(value, sizeof(value), "%s  v%s",
                 text_or(pdv->driver_name, text_or(pdv->filename, "?")),
                 text_or(pdv->version_str, "?"));
        draw_value(surf, CONTENT_X + 12, y + 22 + printed * 16,
                   width - 24, value, PXE_COL_BLACK);
        printed++;
    }
    if (!printed) {
        draw_kv(surf, CONTENT_X + 10, y + 22, 86, width - 112,
                "Driver", "Boot framebuffer fallback", PXE_COL_DARK_GRAY);
    }
}

static void save_dialog_origin(int width, int height, int* x, int* y) {
    *x = (width - SAVE_DIALOG_W) / 2;
    *y = (height - SAVE_DIALOG_H) / 2;
    if (*x < 4) *x = 4;
    if (*y < 28) *y = 28;
}

static void render_save_dialog(const pxe_win_surface_t* surf,
                               cpuz_app_t* app) {
    if (!app->save_modal) return;

    int x, y;
    save_dialog_origin(surf->width, surf->height, &x, &y);
    pxe_draw_bevel_box(surf, x, y, SAVE_DIALOG_W, SAVE_DIALOG_H, 0,
                       PXE_COL_LIGHT_GRAY);
    pxe_fill_rect(surf, x + 2, y + 2, SAVE_DIALOG_W - 4, 18,
                  PXE_COL_DARK_BLUE);
    pxe_draw_text(surf, x + 8, y + 2,
                  app->zh ? "保存诊断报告" : "Save Diagnostic Report",
                  PXE_COL_WHITE);
    pxe_draw_text_small(surf, x + 14, y + 27,
                        app->zh ? "请输入以 .TXT 结尾的完整绝对路径:"
                                : "Choose an absolute path ending in .TXT:",
                        PXE_COL_BLACK);

    int field_x = x + 14;
    int field_y = y + 43;
    int field_w = SAVE_DIALOG_W - 28;
    pxe_draw_bevel_box(surf, field_x, field_y, field_w, 26, 1,
                       PXE_COL_WHITE);

    const char* vis_p = app->save_path;
    while (*vis_p && pxe_text_width_small(vis_p) > field_w - 14) {
        (void)utf8_decode_next(&vis_p);
    }
    pxe_draw_text_small(surf, field_x + 6, field_y + 7, vis_p, PXE_COL_BLACK);
    int caret_x = field_x + 6 + pxe_text_width_small(vis_p);
    if (caret_x >= field_x + field_w - 5) {
        caret_x = field_x + field_w - 5;
    }
    pxe_draw_vline(surf, caret_x, field_y + 8, 12, PXE_COL_BLACK);

    pxe_draw_text_small(surf, x + 14, y + 75,
                        app->zh ? "示例: C:\\CPU_DETECT.TXT  E:\\Logs\\CPU.TXT"
                                : "Examples: C:\\CPU_DETECT.TXT  E:\\Logs\\CPU.TXT",
                        PXE_COL_DARK_GRAY);
    if (app->save_error[0]) {
        draw_clipped(surf, x + 14, y + 91, app->save_error,
                     PXE_COL_DARK_RED, SAVE_DIALOG_W - 28);
    } else {
        pxe_draw_text_small(surf, x + 14, y + 91,
                            app->zh ? "Enter 保存 | Ctrl+V 粘贴 | Esc 取消"
                                    : "Enter saves. Ctrl+V pastes. Esc cancels.",
                            PXE_COL_DARK_BLUE);
    }
    pxe_draw_button(surf, x + SAVE_DIALOG_W - 188, y + 113, 82, 24,
                    app->zh ? "保存" : "Save", 0, 1);
    pxe_draw_button(surf, x + SAVE_DIALOG_W - 96, y + 113, 82, 24,
                    app->zh ? "取消" : "Cancel", 0, 1);
}

static void render_about(const pxe_win_surface_t* surf, cpuz_app_t* app) {
    int width = surf->width - 24;
    draw_group(surf, CONTENT_X, CONTENT_Y, width, 194,
               app->zh ? "Pixel CPU 检测" : "Pixel CPU Detect");
    pxe_draw_text(surf, CONTENT_X + 14, CONTENT_Y + 26,
                  app->zh ? "Pixel CPU 检测工具" : APP_NAME,
                  PXE_COL_DARK_BLUE);
    char value[192];
    snprintf(value, sizeof(value),
             app->zh ? "版本 %s  |  Ring 0 CPUID 硬件侦测"
                     : "Version %s  |  Ring 0 CPUID inspector",
             APP_VERSION);
    pxe_draw_text_small(surf, CONTENT_X + 14, CONTENT_Y + 48, value,
                        PXE_COL_BLACK);
    snprintf(value, sizeof(value), "Pixel Neko OS %s (%s)",
             text_or(app->basic.os_build, "Unknown"),
             text_or(app->basic.os_codename, "Snow Neko"));
    pxe_draw_text_small(surf, CONTENT_X + 14, CONTENT_Y + 66, value,
                        PXE_COL_BLACK);
    pxe_draw_hline(surf, CONTENT_X + 12, CONTENT_Y + 84, width - 24,
                   PXE_COL_DARK_GRAY);
    pxe_draw_text_small(surf, CONTENT_X + 14, CONTENT_Y + 98,
                        app->zh ? "CPU 识别核心: libcpuid 0.8.2"
                                : "CPU recognition core: libcpuid 0.8.2",
                        PXE_COL_BLACK);
    pxe_draw_text_small(surf, CONTENT_X + 14, CONTENT_Y + 114,
                        "Copyright (c) Veselin Georgiev and contributors",
                        PXE_COL_DARK_GRAY);
    pxe_draw_text_small(surf, CONTENT_X + 14, CONTENT_Y + 130,
                        app->zh ? "基于 BSD-2-Clause 开源协议"
                                : "BSD-2-Clause open source component",
                        PXE_COL_DARK_GREEN);
    pxe_draw_text_small(surf, CONTENT_X + 14, CONTENT_Y + 150,
                        app->zh ? "报告包含已解码规格信息与原始 CPUID 寄存器页。"
                                : "Report includes decoded data and raw CPUID leaves.",
                        PXE_COL_DARK_BLUE);
    if (app->ime_phrases > 0) {
        snprintf(value, sizeof(value),
                 app->zh ? "IME 词库: 完整 Rime 词库已加载 (%d 词组)"
                         : "IME Lexicon: Full Rime lexicon loaded (%d phrases)",
                 app->ime_phrases);
        pxe_draw_text_small(surf, CONTENT_X + 14, CONTENT_Y + 168,
                            value, PXE_COL_DARK_GREEN);
    } else {
        pxe_draw_text_small(surf, CONTENT_X + 14, CONTENT_Y + 168,
                            app->zh ? "IME 词库: 内置精简回退词库 (IME.PNY 未加载)"
                                    : "IME Lexicon: Core fallback active (IME.PNY unavailable)",
                            PXE_COL_DARK_RED);
    }

    pxe_draw_button(surf, surf->width - 198, surf->height - 58, 186, 26,
                    app->zh ? "保存检测报告..." : "Save Report As...", 0, 1);
    char path[32];
    snprintf(path, sizeof(path), "%c:\\CPU_DETECT.TXT",
             app->basic.system_drive ? app->basic.system_drive : 'C');
    pxe_draw_text_small(surf, CONTENT_X + 2, surf->height - 82,
                        app->zh ? "默认报告保存路径 (可修改):"
                                : "Default report path (editable):",
                        PXE_COL_DARK_GRAY);
    pxe_draw_text_small(surf, CONTENT_X + 2, surf->height - 66,
                        path, PXE_COL_DARK_BLUE);
}

static void render_status(const pxe_win_surface_t* surf, cpuz_app_t* app) {
    int y = surf->height - STATUS_H;
    pxe_draw_hline(surf, 0, y, surf->width, PXE_COL_DARK_GRAY);
    char status[192];
    uint32_t color = PXE_COL_BLACK;
    if (app->notice[0]) {
        copy_text(status, sizeof(status), app->notice);
        color = app->notice_is_error ? PXE_COL_DARK_RED : PXE_COL_DARK_GREEN;
    } else if (!app->identify_ok) {
        snprintf(status, sizeof(status),
                 app->zh ? "CPUID: %s" : "CPUID: %s",
                 text_or(app->cpu_error, app->zh ? "识别失败" : "identification failed"));
        color = PXE_COL_DARK_RED;
    } else {
        snprintf(status, sizeof(status),
                 app->zh ? "%s | %d 个 PCI 设备 | %d 个 PDV 驱动 | libcpuid %s"
                         : "%s | %d PCI devices | %d PDV drivers | libcpuid %s",
                 text_or(app->cpu.cpu_codename, "x86-64 CPU"),
                 app->pci_count, app->pdv_count, LIBCPUID_VERSION);
    }
    draw_clipped(surf, 8, y + 6, status, color, surf->width - 16);
}

static void render(cpuz_app_t* app, const pxe_win_surface_t* surf) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height,
                  PXE_COL_LIGHT_GRAY);
    render_tabs(surf, app);

    int frame_h = surf->height - TAB_STRIP_H - STATUS_H - 8;
    pxe_draw_bevel_box(surf, 6, 30, surf->width - 12, frame_h, 0,
                       PXE_COL_LIGHT_GRAY);

    switch (app->tab) {
        case TAB_CACHES:    render_caches(surf, app); break;
        case TAB_MAINBOARD: render_mainboard(surf, app); break;
        case TAB_MEMORY:    render_memory(surf, app); break;
        case TAB_GRAPHICS:  render_graphics(surf, app); break;
        case TAB_ABOUT:     render_about(surf, app); break;
        case TAB_CPU:
        default:            render_cpu(surf, app); break;
    }
    render_status(surf, app);
    render_save_dialog(surf, app);
}

static int hit_tab(int x, int y) {
    if (y < TAB_STRIP_Y || y >= TAB_STRIP_Y + TAB_STRIP_H) return -1;
    for (int tab = 0; tab < TAB_COUNT; tab++) {
        if (x >= k_tab_x[tab] && x < k_tab_x[tab] + k_tab_w[tab]) {
            return tab;
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    cpuz_app_t* app = &g_app;
    memset(app, 0, sizeof(*app));
    app->tab = TAB_CPU;
    app->hypervisor = HYPERVISOR_UNKNOWN;
    app->zh = pxe_is_zh();
    refresh_all(app);
    set_default_save_path(app);

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    int screen_width = app->basic.screen_w > 0 ? app->basic.screen_w : 640;
    int screen_height = app->basic.screen_h > 0 ? app->basic.screen_h : 480;
    desc.x = (screen_width - 540) / 2;
    desc.y = (screen_height - 410) / 2;
    if (desc.x < 8) desc.x = 8;
    if (desc.y < 28) desc.y = 28;
    desc.w = 540;
    desc.h = 410;
    desc.min_w = 540;
    desc.min_h = 410;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_DEFAULT;
    copy_text(desc.title, sizeof(desc.title), app->zh ? "CPU 检测" : APP_NAME);

    pxe_win_surface_t surface;
    memset(&surface, 0, sizeof(surface));
    app->win_id = pxe_win_create(&desc, &surface);
    if (app->win_id < 0) return 1;

    int running = 1;
    int dirty = 1;
    uint64_t next_refresh = pxe_get_ticks() + 100;

    while (running) {
        pxe_event_t event;
        int poll_result;
        while ((poll_result = pxe_win_poll_event(app->win_id, &event)) > 0) {
            switch (event.type) {
                case PXE_EV_CLOSE:
                    running = 0;
                    break;
                case PXE_EV_REDRAW: {
                    int new_zh = pxe_is_zh();
                    if (new_zh != app->zh) {
                        app->zh = new_zh;
                        pxe_win_set_title(app->win_id, app->zh ? "CPU 检测" : APP_NAME);
                    }
                    dirty = 1;
                    break;
                }
                case PXE_EV_ACTIVATE:
                    dirty = 1;
                    break;
                case PXE_EV_MOUSE_DOWN: {
                    if (!(event.buttons & PXE_BTN_LEFT)) break;
                    if (app->save_modal) {
                        int dialog_x, dialog_y;
                        save_dialog_origin(surface.width, surface.height,
                                           &dialog_x, &dialog_y);
                        if (pxe_point_in_rect(
                                event.x, event.y,
                                dialog_x + SAVE_DIALOG_W - 188,
                                dialog_y + 113, 82, 24)) {
                            submit_save_report(app);
                        } else if (pxe_point_in_rect(
                                       event.x, event.y,
                                       dialog_x + SAVE_DIALOG_W - 96,
                                       dialog_y + 113, 82, 24)) {
                            app->save_modal = 0;
                            app->save_error[0] = '\0';
                        }
                        dirty = 1;
                        break;
                    }
                    int tab = hit_tab(event.x, event.y);
                    if (tab >= 0) {
                        app->tab = tab;
                        app->notice[0] = '\0';
                        dirty = 1;
                        break;
                    }
                    if (event.x >= surface.width - 66 && event.x <
                        surface.width - 8 && event.y >= 6 && event.y < 28) {
                        refresh_all(app);
                        app->notice[0] = '\0';
                        dirty = 1;
                        break;
                    }
                    if (app->tab == TAB_ABOUT &&
                        pxe_point_in_rect(event.x, event.y,
                                          surface.width - 198,
                                          surface.height - 58, 186, 26)) {
                        open_save_dialog(app);
                        dirty = 1;
                    }
                    break;
                }
                case PXE_EV_KEY_DOWN:
                    if (app->save_modal) {
                        if (event.key == PXE_KEY_ESC) {
                            app->save_modal = 0;
                            app->save_error[0] = '\0';
                        } else if (event.key == PXE_KEY_ENTER) {
                            submit_save_report(app);
                        } else if (event.key == PXE_KEY_BACKSPACE) {
                            if (app->save_path_len > 0) {
                                int d = utf8_prev_char_bytes(app->save_path, app->save_path_len);
                                app->save_path_len -= d;
                                app->save_path[app->save_path_len] = '\0';
                                app->save_error[0] = '\0';
                            }
                        } else if (event.key == PXE_KEY_UTF8 && event.text[0]) {
                            append_save_path_text(app, event.text);
                            app->save_error[0] = '\0';
                        } else if (event.modifiers & PXE_MOD_CTRL) {
                            if (event.key == 'v' || event.key == 'V') {
                                paste_save_path(app);
                                app->save_error[0] = '\0';
                            } else if (event.key == 's' || event.key == 'S') {
                                submit_save_report(app);
                            }
                        } else if (event.key >= 32 && event.key <= 126 &&
                                   app->save_path_len <
                                   SAVE_PATH_MAX - 1) {
                            app->save_path[app->save_path_len++] =
                                (char)event.key;
                            app->save_path[app->save_path_len] = '\0';
                            app->save_error[0] = '\0';
                        }
                        dirty = 1;
                        break;
                    }
                    if (event.key == PXE_KEY_LEFT && app->tab > 0) {
                        app->tab--;
                        app->notice[0] = '\0';
                        dirty = 1;
                    } else if (event.key == PXE_KEY_RIGHT &&
                               app->tab + 1 < TAB_COUNT) {
                        app->tab++;
                        app->notice[0] = '\0';
                        dirty = 1;
                    } else if ((event.modifiers & PXE_MOD_CTRL) &&
                               (event.key == 's' || event.key == 'S')) {
                        open_save_dialog(app);
                        dirty = 1;
                    } else if (event.key == 'r' || event.key == 'R') {
                        refresh_all(app);
                        app->notice[0] = '\0';
                        dirty = 1;
                    }
                    break;
                default:
                    break;
            }
        }
        if (poll_result < 0) break;

        uint64_t now = pxe_get_ticks();
        if (now >= next_refresh) {
            refresh_dynamic(app);
            next_refresh = now + 100;
            dirty = 1;
        }

        if (dirty && running) {
            render(app, &surface);
            pxe_win_present(app->win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(30);
        }
    }

    pxe_win_destroy(app->win_id);
    return 0;
}
