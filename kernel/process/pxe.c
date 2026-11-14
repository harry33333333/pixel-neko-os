#include "pxe.h"
#include "audio.h"
#include "blkdev.h"
#include "cpu.h"
#include "format.h"
#include "gfx_accel.h"
#include "heap.h"
#include "log.h"
#include "mm.h"
#include "mouse.h"
#include "neko_abi.h"
#include "part.h"
#include "pci.h"
#include "pdv.h"
#include "pmm.h"
#include "port.h"
#include "power.h"
#include "scheduler.h"
#include "settings.h"
#include "speaker.h"
#include "string.h"
#include "ui_compat.h"
#include "usb_core.h"
#include "version.h"
#include "vfs.h"
#include "vmm.h"
#include "wallpaper.h"
#include "wm.h"

extern int screen_w;
extern int screen_h;
extern int screen_bpp;
extern int total_ram_mb;
extern int usable_ram_mb;
extern int g_desktop_pattern;
#include "image_dec.h"
#include "wallpaper.h"
#include "cpu.h"
#include "wm.h"
#include "fat.h"
#include "gfx_accel.h"
#include "audio.h"

char g_clipboard[4096] = {0};

/* Ring 0 PXE high-half slot arena: [0xFFFFFFFFC0000000 .. 0xFFFFFFFFE0000000) */
#define PXE_R0_ARENA_BASE  0xFFFFFFFFC0000000ULL
#define PXE_R0_SLOT_STRIDE 0x0000000001000000ULL /* 16 MB per Ring 0 PXE process */
#define PXE_R0_MAX_SLOTS   32

/* Ring 3 PIE default base & user stack */
#define PXE_R3_PIE_BASE    0x0000000000400000ULL
#define PXE_R3_STACK_TOP   0x00007ffffff00000ULL
#define PXE_MAX_FILE_BYTES (8U * 1024U * 1024U)
#define PXE_R0_ALLOC_MAGIC 0x50584552304D454DULL

/* ELF64 structures for ET_DYN / ET_EXEC loading */
#define PXE_ELF_MAG0       0x7FU
#define PXE_ELF_MAG1       'E'
#define PXE_ELF_MAG2       'L'
#define PXE_ELF_MAG3       'F'
#define PXE_ELFCLASS64     2U
#define PXE_ELFDATA2LSB    1U
#define PXE_ET_EXEC        2U
#define PXE_ET_DYN         3U
#define PXE_EM_X86_64      62U
#define PXE_PT_LOAD        1U
#define PXE_PF_X           0x1U
#define PXE_PF_W           0x2U
#define PXE_PF_R           0x4U
#define PXE_SHT_RELA       4U

#define PXE_R_X86_64_NONE      0U
#define PXE_R_X86_64_64        1U
#define PXE_R_X86_64_GLOB_DAT  6U
#define PXE_R_X86_64_JUMP_SLOT 7U
#define PXE_R_X86_64_RELATIVE  8U

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) pxe_elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) pxe_elf64_phdr_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} __attribute__((packed)) pxe_elf64_shdr_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) pxe_elf64_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) pxe_elf64_rela_t;

static uint32_t g_pxe_crc_table[256];
static bool g_pxe_crc_ready = false;
static bool g_r0_slots[PXE_R0_MAX_SLOTS];

static const pxe_ring0_api_t g_pxe_ring0_api;

static inline uint64_t pxe_irq_save(void) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    return rflags;
}

static inline void pxe_irq_restore(uint64_t rflags) {
    if (rflags & (1ULL << 9)) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

static char pxe_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int pxe_strcasecmp(const char* a, const char* b) {
    if (!a || !b) return a == b ? 0 : -1;
    while (*a && *b) {
        char ca = pxe_ascii_lower(*a++);
        char cb = pxe_ascii_lower(*b++);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
    return (int)(unsigned char)pxe_ascii_lower(*a) -
           (int)(unsigned char)pxe_ascii_lower(*b);
}

static void pxe_init_crc32(void) {
    if (g_pxe_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++) {
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        g_pxe_crc_table[i] = c;
    }
    g_pxe_crc_ready = true;
}

static uint32_t pxe_compute_image_crc32(const uint8_t* buf, size_t len) {
    pxe_init_crc32();
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = (i >= 0x30U && i < 0x34U) ? 0U : buf[i];
        crc = g_pxe_crc_table[(crc ^ byte) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

void pxe_subsystem_init(void) {
    pxe_init_crc32();
    memset(g_r0_slots, 0, sizeof(g_r0_slots));
    log_printf("[PXE ] Dual-Ring (*.pxe) executable subsystem ready.\n");
}

bool pxe_is_trusted_apps_path(const char* path) {
    if (!path || strlen(path) < 8) return false;
    if (path[1] != ':') return false;

    char letter = path[0];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 32);

    char sys_drv = settings_get_system_drive();
    char cd_drv  = vfs_get_boot_cdrom_letter();
    if (letter != sys_drv && letter != cd_drv) return false;

    /* Require exact "<DRV>:\APPS\<file>.pxe" without ".." traversal */
    const char* p = path + 2;
    if (p[0] != '\\' && p[0] != '/') return false;
    if (pxe_ascii_lower(p[1]) != 'a' ||
        pxe_ascii_lower(p[2]) != 'p' ||
        pxe_ascii_lower(p[3]) != 'p' ||
        pxe_ascii_lower(p[4]) != 's' ||
        (p[5] != '\\' && p[5] != '/')) {
        return false;
    }
    for (size_t i = 6; p[i]; i++) {
        if (p[i] == '.' && p[i + 1] == '.') return false;
    }
    return true;
}

/* =========================================================================
 * Shared Kernel Runtime Backend (Ring 3 Syscalls + Ring 0 Direct Dispatch)
 * ========================================================================= */

static unsigned char pxe_cmos_read(unsigned char reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t pxe_bcd_to_bin(uint8_t b) {
    return (uint8_t)(((b >> 4) * 10U) + (b & 0x0FU));
}

int pxe_kern_get_rtc(pxe_rtc_time_t* out) {
    if (!out) return -1;
    int timeout = 10000;
    while ((pxe_cmos_read(0x0A) & 0x80U) && --timeout > 0) {}

    uint8_t sec  = pxe_cmos_read(0x00);
    uint8_t min  = pxe_cmos_read(0x02);
    uint8_t hr   = pxe_cmos_read(0x04);
    uint8_t day  = pxe_cmos_read(0x07);
    uint8_t mon  = pxe_cmos_read(0x08);
    uint8_t yr   = pxe_cmos_read(0x09);
    uint8_t regb = pxe_cmos_read(0x0B);

    if (!(regb & 0x04U)) {
        sec = pxe_bcd_to_bin(sec);
        min = pxe_bcd_to_bin(min);
        hr  = pxe_bcd_to_bin((uint8_t)(hr & 0x7FU));
        day = pxe_bcd_to_bin(day);
        mon = pxe_bcd_to_bin(mon);
        yr  = pxe_bcd_to_bin(yr);
    }

    memset(out, 0, sizeof(*out));
    out->year   = (uint16_t)(2000U + yr);
    out->month  = mon;
    out->day    = day;
    out->hour   = hr;
    out->minute = min;
    out->second = sec;
    return 0;
}

int pxe_kern_get_basic_sysinfo(pxe_basic_sysinfo_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    strncpy(out->os_name, OS_NAME, sizeof(out->os_name) - 1);
    strncpy(out->os_build, KERNEL_BUILD, sizeof(out->os_build) - 1);
    strncpy(out->os_codename, OS_CODENAME, sizeof(out->os_codename) - 1);
    strncpy(out->cpu_model, cpu_model_string, sizeof(out->cpu_model) - 1);
    strncpy(out->cpu_vendor, g_cpu_info.vendor, sizeof(out->cpu_vendor) - 1);
    out->cpu_freq_mhz    = g_cpu_info.mhz;
    out->total_ram_mb    = (uint32_t)total_ram_mb;
    out->usable_ram_mb   = (uint32_t)usable_ram_mb;
    out->free_ram_mb     = (uint32_t)((pmm_get_free_page_count() * PAGE_SIZE) / (1024ULL * 1024ULL));
    out->screen_w        = screen_w;
    out->screen_h        = screen_h;
    out->screen_bpp      = screen_bpp;
    if (ui_install_mode_requested()) {
        out->boot_mode = 1;
    } else if (settings_get_system_drive() != 0 && !vfs_is_booted_from_cdrom()) {
        out->boot_mode = 2;
    } else {
        out->boot_mode = 0;
    }
    out->system_drive      = settings_get_system_drive();
    out->cdrom_drive       = vfs_get_boot_cdrom_letter();
    out->desktop_pattern   = (uint8_t)g_desktop_pattern;
    out->mouse_speed       = (uint8_t)mouse_get_speed();
    out->volume            = (uint8_t)audio_get_volume();
    out->wallpaper_active  = (uint8_t)(wallpaper_is_active() ? 1 : 0);
    out->wallpaper_mode    = (uint8_t)wallpaper_get_mode();
    out->settings_writable = (uint8_t)((out->system_drive != 0 && !vfs_is_booted_from_cdrom()) ? 1 : 0);
    out->has_audio_hw      = (uint8_t)(audio_has_hardware() ? 1 : 0);
    out->audio_muted       = (uint8_t)(audio_is_muted() ? 1 : 0);
    out->gfx_is_2d_hw      = (uint8_t)(gfx_accel_is_hardware_2d_active() ? 1 : 0);
    out->gfx_has_hw_cursor = (uint8_t)(gfx_accel_has_hardware_cursor() ? 1 : 0);
    out->vram_bytes        = (uint64_t)gfx_accel_get_vram_size();

    static const int k_res_w[4] = { 640, 800, 1024, 1280 };
    static const int k_res_h[4] = { 480, 600,  768, 1024 };
    static const int k_bpps[4]  = { 32, 24, 16, 8 };
    uint16_t mask = 0;
    for (int r = 0; r < 4; r++) {
        for (int b = 0; b < 4; b++) {
            if (gfx_accel_supports_bpp(k_res_w[r], k_res_h[r], k_bpps[b])) {
                mask |= (uint16_t)(1u << (r * 4 + b));
            }
        }
    }
    out->mode_support_mask = mask;

    uint32_t cflags = 0;
    if (g_cpu_info.basic_edx & (1u << 25)) cflags |= PXE_CPU_SSE;
    if (g_cpu_info.has_sse2 || (g_cpu_info.basic_edx & (1u << 26))) cflags |= PXE_CPU_SSE2;
    if (g_cpu_info.has_sse3 || (g_cpu_info.basic_ecx & (1u << 0)))  cflags |= PXE_CPU_SSE3;
    if (g_cpu_info.basic_ecx & (1u << 9))  cflags |= PXE_CPU_SSSE3;
    if (g_cpu_info.basic_ecx & (1u << 19)) cflags |= PXE_CPU_SSE41;
    if (g_cpu_info.basic_ecx & (1u << 20)) cflags |= PXE_CPU_SSE42;
    if (g_cpu_info.basic_ecx & (1u << 28)) cflags |= PXE_CPU_AVX;
    if (g_cpu_info.has_nx)                 cflags |= PXE_CPU_NX;
    if (g_cpu_info.basic_edx & (1u << 3))  cflags |= PXE_CPU_PSE;
    if (g_cpu_info.basic_edx & (1u << 6))  cflags |= PXE_CPU_PAE;
    if (g_cpu_info.has_apic)               cflags |= PXE_CPU_APIC;
    if (g_cpu_info.basic_ecx & (1u << 21)) cflags |= PXE_CPU_X2APIC;
    if (g_cpu_info.basic_edx & (1u << 4))  cflags |= PXE_CPU_TSC;
    if (g_cpu_info.basic_ecx & (1u << 30)) cflags |= PXE_CPU_RDRAND;
    out->cpu_flags = cflags;

    if (gfx_accel_get_name()) {
        strncpy(out->gfx_backend, gfx_accel_get_name(), sizeof(out->gfx_backend) - 1);
    }
    if (gfx_accel_get_status()) {
        strncpy(out->gfx_status, gfx_accel_get_status(), sizeof(out->gfx_status) - 1);
    }
    if (gfx_accel_get_fallback_reason()) {
        strncpy(out->gfx_fallback, gfx_accel_get_fallback_reason(), sizeof(out->gfx_fallback) - 1);
    }
    if (audio_get_hardware_name()) {
        strncpy(out->audio_backend, audio_get_hardware_name(), sizeof(out->audio_backend) - 1);
    }
    if (audio_get_probe_status()) {
        strncpy(out->audio_probe, audio_get_probe_status(), sizeof(out->audio_probe) - 1);
    }
    return 0;
}

int pxe_kern_get_display_caps(pxe_display_caps_t* out) {
    if (!out) return -NEKO_EINVAL;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);

    int has_drv = gfx_accel_has_valid_driver();
    uint32_t flags = 0;
    if (has_drv) {
        flags |= PXE_DISP_FLAG_DRIVER_ACTIVE | PXE_DISP_FLAG_UNLIMITED_RES;
    }
    if (gfx_accel_is_hardware_2d_active()) {
        flags |= PXE_DISP_FLAG_HW_2D;
    }
    out->flags       = flags;
    out->current_w   = screen_w;
    out->current_h   = screen_h;
    out->current_bpp = screen_bpp;
    out->min_w       = GFX_MIN_WIDTH;
    out->min_h       = GFX_MIN_HEIGHT;
    out->max_w       = gfx_accel_get_max_width();
    out->max_h       = gfx_accel_get_max_height();
    out->bpp_mask    = (uint32_t)gfx_accel_get_supported_bpp_mask();
    out->vram_bytes  = (uint64_t)gfx_accel_get_vram_size();

    if (gfx_accel_get_name()) {
        strncpy(out->driver_name, gfx_accel_get_name(), sizeof(out->driver_name) - 1);
    }
    if (gfx_accel_get_status()) {
        strncpy(out->driver_status, gfx_accel_get_status(), sizeof(out->driver_status) - 1);
    }

    int total = gfx_accel_get_mode_count();
    if (total > (int)PXE_DISP_MAX_MODES) total = (int)PXE_DISP_MAX_MODES;
    int written = 0;
    for (int i = 0; i < total; i++) {
        int mw = 0, mh = 0;
        unsigned char bmask = 0;
        unsigned char pref = 0;
        if (gfx_accel_get_mode(i, &mw, &mh, &bmask, &pref)) {
            out->modes[written].width = (uint16_t)mw;
            out->modes[written].height = (uint16_t)mh;
            out->modes[written].bpp_mask = bmask;
            out->modes[written].preferred = pref;
            out->modes[written].reserved = 0U;
            written++;
        }
    }
    out->mode_count = written;
    return 0;
}

int pxe_kern_sys_config(process_t* proc, int cmd, int arg0, int arg1, const char* str_arg) {
    if (proc && proc->exec_ring == 3 && !(proc->pxe_flags & PXE_FLAG_SYS_SETTINGS)) {
        return -NEKO_EPERM;
    }
    switch (cmd) {
        case PXE_CFG_SET_RESOLUTION: {
            int w = arg0 & 0xFFFF;
            int h = (arg0 >> 16) & 0xFFFF;
            int bpp = arg1;
            if (w < GFX_MIN_WIDTH || h < GFX_MIN_HEIGHT) return -NEKO_EINVAL;
            if (w != screen_w || h != screen_h || bpp != screen_bpp) {
                if (!gfx_accel_try_set_mode(w, h, bpp)) return -NEKO_EINVAL;
            }
            settings_set_resolution(w, h, bpp);
            wm_request_desktop_redraw();
            return 0;
        }
        case PXE_CFG_APPLY_RESOLUTION: {
            int w = arg0 & 0xFFFF;
            int h = (arg0 >> 16) & 0xFFFF;
            int bpp = arg1;
            if (w < GFX_MIN_WIDTH || h < GFX_MIN_HEIGHT) return -NEKO_EINVAL;
            if (!gfx_accel_try_set_mode(w, h, bpp)) return -NEKO_EINVAL;
            wm_request_desktop_redraw();
            return 0;
        }
        case PXE_CFG_SET_PATTERN:
            settings_set_pattern(arg0);
            wm_request_desktop_redraw();
            return 0;
        case PXE_CFG_SET_WALLPAPER: {
            if (!str_arg || !str_arg[0]) return -NEKO_EINVAL;
            image_t img;
            image_init(&img);
            char err_msg[96];
            err_msg[0] = '\0';
            if (image_load_from_file(str_arg, &img, err_msg, sizeof(err_msg)) != 0) {
                return -NEKO_EINVAL;
            }
            wallpaper_mode_t mode = (arg0 >= 0 && arg0 <= WP_MODE_TILE)
                ? (wallpaper_mode_t)arg0 : WP_MODE_FILL;
            int rc = wallpaper_set_image(&img, mode);
            image_free(&img);
            if (rc != 0) return -NEKO_ENOMEM;
            settings_set_wallpaper(str_arg, mode);
            wm_request_desktop_redraw();
            return 0;
        }
        case PXE_CFG_SET_WALLPAPER_MODE: {
            wallpaper_mode_t mode = (arg0 >= 0 && arg0 <= WP_MODE_TILE)
                ? (wallpaper_mode_t)arg0 : WP_MODE_FILL;
            wallpaper_set_mode(mode);
            wm_request_desktop_redraw();
            return 0;
        }
        case PXE_CFG_CLEAR_WALLPAPER:
            wallpaper_clear();
            settings_clear_wallpaper();
            wm_request_desktop_redraw();
            return 0;
        case PXE_CFG_SET_MOUSE_SPEED:
            settings_set_mouse_speed(arg0);
            return 0;
        case PXE_CFG_SAVE_SETTINGS:
            return settings_save() ? 0 : -1;
        default:
            return -NEKO_EINVAL;
    }
}

int pxe_kern_clipboard_op(int is_set, char* buf, size_t max_len) {
    if (!buf) return -1;
    if (is_set) {
        size_t n = strlen(buf);
        if (n >= sizeof(g_clipboard)) n = sizeof(g_clipboard) - 1;
        memcpy(g_clipboard, buf, n);
        g_clipboard[n] = '\0';
        return (int)n;
    } else {
        if (max_len == 0) return 0;
        size_t n = strlen(g_clipboard);
        if (n >= max_len) n = max_len - 1;
        memcpy(buf, g_clipboard, n);
        buf[n] = '\0';
        return (int)n;
    }
}

int pxe_kern_fs_open(process_t* proc, const char* path) {
    if (!proc || !path || !path[0]) return -1;
    int fd = -1;
    for (int i = 0; i < PROCESS_MAX_STREAMS; i++) {
        if (!proc->streams[i]) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;

    vfs_stream_t* stream = (vfs_stream_t*)kmalloc(sizeof(vfs_stream_t));
    if (!stream) return -1;
    if (vfs_stream_open(path, stream) != 0) {
        kfree(stream);
        return -1;
    }
    proc->streams[fd] = stream;
    return fd;
}

int pxe_kern_fs_create(process_t* proc, const char* path) {
    if (!proc || !path || !path[0]) return -1;
    int fd = -1;
    for (int i = 0; i < PROCESS_MAX_STREAMS; i++) {
        if (!proc->streams[i]) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;

    vfs_stream_t* stream = (vfs_stream_t*)kmalloc(sizeof(vfs_stream_t));
    if (!stream) return -1;
    if (vfs_stream_create(path, stream) != 0) {
        kfree(stream);
        return -1;
    }
    proc->streams[fd] = stream;
    return fd;
}

int pxe_kern_fs_read(process_t* proc, int fd, void* buf, size_t count) {
    if (!proc || !buf || fd < 0 || fd >= PROCESS_MAX_STREAMS) return -1;
    vfs_stream_t* stream = (vfs_stream_t*)proc->streams[fd];
    if (!stream || !stream->is_open) return -1;
    if (count == 0) return 0;
    if (count > 0x40000000U) count = 0x40000000U;
    return vfs_stream_read(stream, buf, (unsigned int)count);
}

int pxe_kern_fs_write(process_t* proc, int fd, const void* buf, size_t count) {
    if (!proc || fd < 0 || fd >= PROCESS_MAX_STREAMS) return -1;
    vfs_stream_t* stream = (vfs_stream_t*)proc->streams[fd];
    if (!stream || !stream->is_open || !stream->is_write) return -1;
    if (count == 0) return 0;
    if (!buf) return -1;
    if (count > 0x40000000U) count = 0x40000000U;
    return vfs_stream_write(stream, buf, (unsigned int)count);
}

int pxe_kern_fs_seek(process_t* proc, int fd, uint64_t offset) {
    if (!proc || fd < 0 || fd >= PROCESS_MAX_STREAMS) return -1;
    vfs_stream_t* stream = (vfs_stream_t*)proc->streams[fd];
    if (!stream || !stream->is_open) return -1;
    return vfs_stream_seek(stream, offset);
}

int pxe_kern_fs_close(process_t* proc, int fd) {
    if (!proc || fd < 0 || fd >= PROCESS_MAX_STREAMS) return -1;
    vfs_stream_t* stream = (vfs_stream_t*)proc->streams[fd];
    if (!stream) return -1;
    if (stream->is_open) vfs_stream_close(stream);
    kfree(stream);
    proc->streams[fd] = NULL;
    return 0;
}

int pxe_kern_fs_stat(const char* path, pxe_dirent_t* out) {
    if (!path || !out) return -1;
    vfs_dirent_t ent;
    if (vfs_find_file(path, &ent) != VFS_OK) return -1;
    memset(out, 0, sizeof(*out));
    strncpy(out->name, ent.name, sizeof(out->name) - 1);
    out->is_dir = ent.is_dir;
    out->size   = ent.size;
    return 0;
}

int pxe_kern_fs_read_file(const char* path, void* buf, size_t max_size) {
    if (!path || !buf || max_size == 0) return -1;
    if (max_size > 0x40000000U) max_size = 0x40000000U;
    return vfs_read_file(path, (unsigned char*)buf, (unsigned int)max_size);
}

int pxe_kern_fs_write_file(const char* path, const void* buf, size_t size) {
    if (!path || (!buf && size > 0)) return -1;
    static const unsigned char empty_byte = 0;
    const unsigned char* ptr = buf ? (const unsigned char*)buf : &empty_byte;
    return vfs_write_file(path, ptr, (unsigned int)size);
}

static pxe_dirent_t* g_listdir_out = NULL;
static int g_listdir_cap = 0;
static int g_listdir_cnt = 0;

static void pxe_listdir_cb(const vfs_dirent_t* ent) {
    if (!ent || !g_listdir_out || g_listdir_cnt >= g_listdir_cap) return;
    if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) return;
    pxe_dirent_t* dst = &g_listdir_out[g_listdir_cnt++];
    memset(dst, 0, sizeof(*dst));
    strncpy(dst->name, ent->name, sizeof(dst->name) - 1);
    dst->is_dir = ent->is_dir;
    dst->size   = ent->size;
}

int pxe_kern_fs_listdir(const char* path, pxe_dirent_t* out, int max_entries) {
    if (!path || !out || max_entries <= 0) return -1;
    g_listdir_out = out;
    g_listdir_cap = max_entries;
    g_listdir_cnt = 0;
    int rc = vfs_list_dir(path, pxe_listdir_cb);
    int cnt = g_listdir_cnt;
    g_listdir_out = NULL;
    g_listdir_cap = 0;
    g_listdir_cnt = 0;
    return (rc == VFS_OK) ? cnt : -1;
}

int pxe_kern_fs_mkdir(const char* path) {
    return vfs_mkdir(path);
}

int pxe_kern_fs_unlink(const char* path) {
    return vfs_unlink(path);
}

int pxe_kern_fs_rename(const char* old_path, const char* new_path) {
    return vfs_rename(old_path, new_path);
}

int pxe_kern_fs_get_drives(pxe_drive_info_t* out, int max_drives) {
    if (!out || max_drives <= 0) return -1;
    int count = 0;
    for (int i = 0; i < 26 && count < max_drives; i++) {
        vfs_drive_t* d = &g_vfs_drives[i];
        if (!d->present) continue;
        pxe_drive_info_t* dst = &out[count++];
        memset(dst, 0, sizeof(*dst));
        dst->letter       = d->letter;
        dst->present      = (uint8_t)d->present;
        dst->mounted      = (uint8_t)d->mounted;
        dst->drv_type     = (uint8_t)d->drv_type;
        dst->is_readonly  = (uint8_t)d->is_readonly;
        dst->is_supported = (uint8_t)d->is_supported;
        uint64_t tot = d->total_kb, fre = d->free_kb;
        int known = d->is_free_known;
        vfs_get_drive_space(d->letter, &tot, &fre, &known);
        dst->total_kb      = tot;
        dst->free_kb       = fre;
        dst->is_free_known = (uint8_t)known;
        const char* fs_str = vfs_get_fs_name(d->fs_type);
        if (fs_str) strncpy(dst->fs_name, fs_str, sizeof(dst->fs_name) - 1);
        strncpy(dst->label, d->label, sizeof(dst->label) - 1);
    }
    return count;
}

int pxe_kern_fs_eject(char letter) {
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 32);
    return vfs_eject_media(letter);
}

int pxe_kern_audio_get_status(pxe_audio_status_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->has_hardware = audio_has_hardware();
    out->state        = (int32_t)audio_get_state();
    out->volume       = audio_get_volume();
    out->muted        = audio_is_muted();
    const audio_info_t* info = audio_get_info();
    if (info) {
        out->sample_rate  = info->sample_rate;
        out->channels     = info->channels;
        out->bit_depth    = info->bit_depth;
        out->bitrate_kbps = info->bitrate_kbps;
    }
    out->position_sec = (int32_t)audio_get_position_sec();
    out->duration_sec = (int32_t)audio_get_duration_sec();
    audio_get_spectrum(out->spectrum);
    audio_get_vu(&out->vu_left, &out->vu_right);
    if (audio_get_hardware_name()) {
        strncpy(out->hw_name, audio_get_hardware_name(), sizeof(out->hw_name) - 1);
    }
    if (audio_get_title()) {
        strncpy(out->title, audio_get_title(), sizeof(out->title) - 1);
    }
    return 0;
}

int pxe_kern_audio_cmd(int cmd, int arg, const char* path, void* io_buf) {
    switch (cmd) {
        case PXE_AUD_PLAY_FILE:
            if (!path || !path[0]) return -1;
            return audio_play_file(path);
        case PXE_AUD_PAUSE:
            audio_pause();
            return 0;
        case PXE_AUD_RESUME:
            audio_resume();
            return 0;
        case PXE_AUD_STOP:
            audio_stop();
            return 0;
        case PXE_AUD_SEEK:
            audio_seek_sec((unsigned int)arg);
            return 0;
        case PXE_AUD_SET_VOLUME:
            audio_set_volume(arg);
            return 0;
        case PXE_AUD_TOGGLE_MUTE:
            audio_toggle_mute();
            return 0;
        case PXE_AUD_GET_STATUS:
            return pxe_kern_audio_get_status((pxe_audio_status_t*)io_buf);
        case PXE_AUD_FEED_PCM: {
            const pxe_pcm_chunk_t* ch = (const pxe_pcm_chunk_t*)io_buf;
            if (!ch || !ch->samples || ch->sample_count == 0) return -1;
            return audio_feed_raw_pcm(ch->samples, ch->sample_count,
                                      ch->src_rate, ch->src_channels,
                                      ch->src_bits, ch->src_float);
        }
        default:
            return -1;
    }
}

/* =========================================================================
 * Ring 0 Direct Dispatch Table Implementation (g_pxe_ring0_api)
 * ========================================================================= */

static void r0_api_exit(int status) {
    process_exit_current(status);
    scheduler_yield_kernel();
    for (;;) { __asm__ volatile ("pause"); }
}

static int r0_api_getpid(void) {
    return (int)process_current_pid();
}

static void r0_api_yield(void) {
    scheduler_mark_current_ready();
    scheduler_yield_kernel();
}

static void r0_api_sleep_ms(uint32_t ms) {
    uint64_t ticks = (ms + 9U) / 10U;
    if (ticks == 0) ticks = 1;
    scheduler_block_current(scheduler_get_ticks() + ticks);
    scheduler_yield_kernel();
}

static uint64_t r0_api_get_ticks(void) {
    return scheduler_get_ticks();
}

static int r0_api_spawn(const char* path, int argc, const char* const* argv) {
    return pxe_spawn_file(path, argc, argv, process_current(), true);
}

static int r0_api_waitpid(int pid, int* out_status) {
    process_t* parent = process_current();
    if (!parent || pid <= 0) return -NEKO_ECHILD;
    process_t* child = process_find_by_pid((uint32_t)pid);
    if (!child) return -NEKO_ECHILD;
    if (child->state != PROCESS_ZOMBIE) return -NEKO_EAGAIN;
    if (out_status) *out_status = child->exit_status;
    child->waited = true;
    return pid;
}

static void r0_api_debug_log(const char* msg) {
    if (!msg) return;
    log_printf("[RING0:%u] %s\n", process_current_pid(), msg);
}

static int r0_api_sys_config(int cmd, int arg0, int arg1, const char* str_arg) {
    return pxe_kern_sys_config(process_current(), cmd, arg0, arg1, str_arg);
}

/* Process-tracked Ring 0 heap (auto-freed on process exit) */
static void* r0_api_malloc(size_t size) {
    if (size == 0) size = 1;
    process_t* p = process_current();
    r0_alloc_node_t* node = (r0_alloc_node_t*)kmalloc(sizeof(r0_alloc_node_t) + size);
    if (!node) return NULL;
    node->size  = size;
    node->magic = PXE_R0_ALLOC_MAGIC;
    node->prev  = NULL;

    uint64_t flags = pxe_irq_save();
    if (p) {
        node->next = p->r0_allocs;
        if (p->r0_allocs) p->r0_allocs->prev = node;
        p->r0_allocs = node;
        p->r0_heap_bytes += size;
    } else {
        node->next = NULL;
    }
    pxe_irq_restore(flags);
    return (void*)(node + 1);
}

static void r0_api_free(void* ptr) {
    if (!ptr) return;
    r0_alloc_node_t* node = ((r0_alloc_node_t*)ptr) - 1;
    if (node->magic != PXE_R0_ALLOC_MAGIC) return;
    node->magic = 0;

    process_t* p = process_current();
    uint64_t flags = pxe_irq_save();
    if (node->prev) {
        node->prev->next = node->next;
    } else if (p && p->r0_allocs == node) {
        p->r0_allocs = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    if (p) {
        if (p->r0_heap_bytes >= node->size) p->r0_heap_bytes -= node->size;
        else p->r0_heap_bytes = 0;
    }
    pxe_irq_restore(flags);
    kfree(node);
}

static void* r0_api_realloc(void* ptr, size_t size) {
    if (!ptr) return r0_api_malloc(size);
    if (size == 0) {
        r0_api_free(ptr);
        return NULL;
    }
    r0_alloc_node_t* old_node = ((r0_alloc_node_t*)ptr) - 1;
    if (old_node->magic != PXE_R0_ALLOC_MAGIC) return NULL;
    if (size <= old_node->size) return ptr;

    void* new_ptr = r0_api_malloc(size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_node->size);
    r0_api_free(ptr);
    return new_ptr;
}

static int r0_api_win_create(const pxe_win_desc_t* desc, pxe_win_surface_t* out_surf) {
    return wm_pxe_win_create(process_current(), desc, out_surf);
}

static int r0_api_win_destroy(int win_id) {
    return wm_pxe_win_destroy(process_current_pid(), win_id);
}

static int r0_api_win_set_title(int win_id, const char* title) {
    return wm_pxe_win_set_title(process_current_pid(), win_id, title);
}

static int r0_api_win_set_menu(int win_id, const pxe_menu_bar_t* menu) {
    return wm_pxe_win_set_menu(process_current_pid(), win_id, menu);
}

static int r0_api_win_get_surface(int win_id, pxe_win_surface_t* out_surf) {
    return wm_pxe_win_get_surface(process_current(), win_id, out_surf);
}

static int r0_api_win_present(int win_id, int x, int y, int w, int h) {
    return wm_pxe_win_present(process_current_pid(), win_id, x, y, w, h);
}

static int r0_api_win_poll_event(int win_id, pxe_event_t* out_ev, int block) {
    uint32_t pid = process_current_pid();
    for (;;) {
        int rc = wm_pxe_win_poll_event(pid, win_id, out_ev);
        if (rc != 0 || !block) return rc;
        scheduler_block_on_window(win_id, 5);
        scheduler_yield_kernel();
    }
}

static int r0_api_fs_open(const char* path) {
    return pxe_kern_fs_open(process_current(), path);
}

static int r0_api_fs_create(const char* path) {
    return pxe_kern_fs_create(process_current(), path);
}

static int r0_api_fs_read(int fd, void* buf, size_t count) {
    return pxe_kern_fs_read(process_current(), fd, buf, count);
}

static int r0_api_fs_write(int fd, const void* buf, size_t count) {
    return pxe_kern_fs_write(process_current(), fd, buf, count);
}

static int r0_api_fs_seek(int fd, uint64_t offset) {
    return pxe_kern_fs_seek(process_current(), fd, offset);
}

static int r0_api_fs_close(int fd) {
    return pxe_kern_fs_close(process_current(), fd);
}

/* Ring 0 Privileged Introspection & Hardware APIs */
static int r0_api_enum_processes(pxe_r0_proc_info_t* out_list, int max_count) {
    if (!out_list || max_count <= 0) return -1;
    uint64_t flags = pxe_irq_save();
    int count = 0;
    for (process_t* p = process_head(); p && count < max_count; p = p->next) {
        pxe_r0_proc_info_t* dst = &out_list[count++];
        memset(dst, 0, sizeof(*dst));
        dst->pid             = p->pid;
        dst->ppid            = p->ppid;
        dst->exec_ring       = p->exec_ring;
        dst->state           = (uint8_t)p->state;
        dst->window_count    = (uint16_t)wm_pxe_count_windows_for_pid(p->pid);
        dst->cpu_ticks_total = p->cpu_ticks_total;
        dst->mem_bytes_used  = p->mem_pages_used * PAGE_SIZE + p->r0_heap_bytes;
        strncpy(dst->name, p->name, sizeof(dst->name) - 1);
        strncpy(dst->exe_path, p->exe_path, sizeof(dst->exe_path) - 1);
    }
    pxe_irq_restore(flags);
    return count;
}

static int r0_api_kill_process(uint32_t pid, int exit_code) {
    return process_kill(pid, exit_code);
}

static int r0_api_focus_process_window(uint32_t pid) {
    return wm_pxe_focus_pid(pid, NULL);
}

static void r0_api_get_memory_summary(pxe_r0_mem_info_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    uint64_t tot  = pmm_get_total_page_count();
    uint64_t fre  = pmm_get_free_page_count();
    uint64_t used = (tot >= fre) ? (tot - fre) : 0;
    out->total_pages      = tot;
    out->free_pages       = fre;
    out->used_pages       = used;
    out->total_bytes      = tot * PAGE_SIZE;
    out->free_bytes       = fre * PAGE_SIZE;
    out->pdv_loaded_count = (uint64_t)pdv_get_loaded_count();

    uint64_t flags = pxe_irq_save();
    uint64_t pcnt = 0;
    for (process_t* p = process_head(); p; p = p->next) {
        if (p->state != PROCESS_ZOMBIE) pcnt++;
    }
    pxe_irq_restore(flags);
    out->process_count = pcnt;
}

static int r0_api_enum_pci_devices(pxe_r0_pci_info_t* out, int max_count) {
    if (!out || max_count <= 0) return -1;
    int total = pci_get_device_count();
    int count = 0;
    for (int i = 0; i < total && count < max_count; i++) {
        const pci_device_t* dev = pci_get_device(i);
        if (!dev) continue;
        pxe_r0_pci_info_t* dst = &out[count++];
        memset(dst, 0, sizeof(*dst));
        dst->segment       = dev->segment;
        dst->bus           = dev->bus;
        dst->slot          = dev->slot;
        dst->function      = dev->function;
        dst->class_code    = dev->class_code;
        dst->subclass      = dev->subclass;
        dst->prog_if       = dev->prog_if;
        dst->vendor_id     = dev->vendor;
        dst->device_id     = dev->device;
        dst->subsys_vendor = dev->subsystem_vendor;
        dst->subsys_device = dev->subsystem_device;
        dst->revision      = dev->revision;
        dst->irq_line      = dev->irq_line;
        dst->bar0_addr     = dev->bar[0].address;
        dst->bar0_size     = dev->bar[0].size;
        const char* cname  = pci_class_name(dev->class_code, dev->subclass);
        if (cname) strncpy(dst->class_name, cname, sizeof(dst->class_name) - 1);
    }
    return count;
}

static int r0_api_enum_usb_devices(pxe_r0_usb_info_t* out, int max_count) {
    if (!out || max_count <= 0) return -1;
    int count = 0;
    for (uint8_t addr = 1; addr <= 126 && count < max_count; addr++) {
        usb_device_t* dev = usb_find_device_by_address(addr);
        if (!dev) continue;
        pxe_r0_usb_info_t* dst = &out[count++];
        memset(dst, 0, sizeof(*dst));
        dst->address        = dev->address;
        dst->speed          = dev->speed;
        dst->port           = dev->port;
        dst->parent_hub     = dev->parent_hub_address;
        dst->class_code     = dev->class_code;
        dst->subclass       = dev->subclass;
        dst->protocol       = dev->protocol;
        dst->endpoint_count = dev->endpoint_count;
        dst->vendor_id      = dev->vendor;
        dst->product_id     = dev->product;
        if (dev->hci && dev->hci->name) {
            strncpy(dst->hci_name, dev->hci->name, sizeof(dst->hci_name) - 1);
        }
        const char* sp = usb_speed_name(dev->speed);
        if (sp) strncpy(dst->speed_name, sp, sizeof(dst->speed_name) - 1);
    }
    return count;
}

static int r0_api_enum_pdv_drivers(pxe_r0_pdv_info_t* out, int max_count) {
    if (!out || max_count <= 0) return -1;
    int total = pdv_get_loaded_count();
    int count = 0;
    for (int i = 0; i < total && count < max_count; i++) {
        const pdv_module_info_t* info = pdv_get_loaded_info(i);
        if (!info) continue;
        pxe_r0_pdv_info_t* dst = &out[count++];
        memset(dst, 0, sizeof(*dst));
        strncpy(dst->filename, info->filename, sizeof(dst->filename) - 1);
        strncpy(dst->driver_name, info->driver_name, sizeof(dst->driver_name) - 1);
        strncpy(dst->provider, info->provider, sizeof(dst->provider) - 1);
        strncpy(dst->version_str, info->version_str, sizeof(dst->version_str) - 1);
        dst->driver_class = info->driver_class;
        dst->base_virt    = info->base_virt;
        dst->rx_pages     = (uint32_t)info->rx_pages;
        dst->ro_pages     = (uint32_t)info->ro_pages;
        dst->rw_pages     = (uint32_t)info->rw_pages;
    }
    return count;
}

static int r0_api_load_pdv_file(const char* pdv_path) {
    if (!pdv_path || !pdv_path[0]) return -1;
    return pdv_load_file(pdv_path, false, true);
}

static int r0_api_rescan_pdevice_dir(void) {
    return pdv_autoload_boot_drivers(false, false);
}

static int r0_api_enum_disks_and_parts(pxe_r0_part_info_t* out, int max_count) {
    if (!out || max_count <= 0) return -1;
    int count = 0;
    for (int i = 0; i < g_partition_count && count < max_count; i++) {
        partition_entry_t* pe = &g_partitions[i];
        if (!pe->dev) continue;
        pxe_r0_part_info_t* dst = &out[count++];
        memset(dst, 0, sizeof(*dst));
        dst->part_index         = i;
        dst->disk_index         = pe->dev->id;
        dst->disk_type          = (uint8_t)pe->dev->type;
        dst->sys_id             = pe->sys_id;
        dst->fs_type            = (uint8_t)pe->fs_type;
        dst->is_removable       = (uint8_t)pe->dev->is_removable;
        dst->is_efi_system      = (uint8_t)pe->is_efi_system;
        dst->start_lba          = pe->start_lba;
        dst->total_sectors      = pe->total_sectors;
        dst->disk_total_sectors = pe->dev->total_sectors;
        strncpy(dst->disk_name, pe->dev->name, sizeof(dst->disk_name) - 1);
        const char* fsn = vfs_get_fs_name(pe->fs_type);
        if (fsn) strncpy(dst->fs_name, fsn, sizeof(dst->fs_name) - 1);
        strncpy(dst->label, pe->label, sizeof(dst->label) - 1);

        for (int d = 0; d < 26; d++) {
            if (g_vfs_drives[d].present &&
                g_vfs_drives[d].dev == pe->dev &&
                g_vfs_drives[d].start_lba == pe->start_lba) {
                dst->mounted_letter = g_vfs_drives[d].letter;
                break;
            }
        }
    }
    return count;
}

static int r0_api_format_partition(int part_idx, int fs_type, const char* label) {
    (void)label;
    partition_entry_t* pe = part_get(part_idx);
    if (!pe || !pe->dev) return -1;
    return format_partition(pe->dev, pe->start_lba, pe->total_sectors, (part_fs_type_t)fs_type);
}

static int r0_api_raw_disk_rw(int disk_id, uint64_t lba, uint32_t count, void* buf, int is_write) {
    blkdev_t* dev = blkdev_get(disk_id);
    if (!dev || !buf || count == 0) return -1;
    return is_write ? block_write(dev, lba, count, buf) : block_read(dev, lba, count, buf);
}

static int r0_api_vfs_remount_all(void) {
    fat_flush_all();
    part_scan_all();
    vfs_init();
    return 0;
}

static void r0_api_power_control(int action) {
    fat_flush_all();
    if (action == PXE_POWER_REBOOT) {
        sys_reboot();
    } else if (action == PXE_POWER_SHUTDOWN) {
        sys_shutdown();
    }
}

static int r0_api_sync_filesystems(void) {
    fat_flush_all();
    return 0;
}

static int r0_api_init_uninit_disk(void) {
    blkdev_t* dev = part_get_uninit_disk();
    if (!dev) return -1;
    part_initialize_disk(dev);
    part_clear_uninit_disk();
    part_scan_all();
    vfs_init();
    return 0;
}

static const pxe_ring0_api_t g_pxe_ring0_api = {
    .api_version          = PXE_R0_API_VERSION,
    .struct_size          = sizeof(pxe_ring0_api_t),
    .exit                 = r0_api_exit,
    .getpid               = r0_api_getpid,
    .yield                = r0_api_yield,
    .sleep_ms             = r0_api_sleep_ms,
    .get_ticks            = r0_api_get_ticks,
    .spawn                = r0_api_spawn,
    .waitpid              = r0_api_waitpid,
    .debug_log            = r0_api_debug_log,
    .get_rtc_time         = pxe_kern_get_rtc,
    .get_basic_sysinfo    = pxe_kern_get_basic_sysinfo,
    .sys_config           = r0_api_sys_config,
    .r0_malloc            = r0_api_malloc,
    .r0_realloc           = r0_api_realloc,
    .r0_free              = r0_api_free,
    .win_create           = r0_api_win_create,
    .win_destroy          = r0_api_win_destroy,
    .win_set_title        = r0_api_win_set_title,
    .win_set_menu         = r0_api_win_set_menu,
    .win_get_surface      = r0_api_win_get_surface,
    .win_present          = r0_api_win_present,
    .win_poll_event       = r0_api_win_poll_event,
    .clipboard_op         = pxe_kern_clipboard_op,
    .notify_banner        = wm_notify_banner,
    .beep                 = speaker_beep,
    .fs_open              = r0_api_fs_open,
    .fs_read              = r0_api_fs_read,
    .fs_seek              = r0_api_fs_seek,
    .fs_close             = r0_api_fs_close,
    .fs_stat              = pxe_kern_fs_stat,
    .fs_read_file         = pxe_kern_fs_read_file,
    .fs_write_file        = pxe_kern_fs_write_file,
    .fs_listdir           = pxe_kern_fs_listdir,
    .fs_mkdir             = pxe_kern_fs_mkdir,
    .fs_unlink            = pxe_kern_fs_unlink,
    .fs_rename            = pxe_kern_fs_rename,
    .fs_get_drives        = pxe_kern_fs_get_drives,
    .fs_eject             = pxe_kern_fs_eject,
    .audio_cmd            = pxe_kern_audio_cmd,
    .enum_processes       = r0_api_enum_processes,
    .kill_process         = r0_api_kill_process,
    .focus_process_window = r0_api_focus_process_window,
    .get_memory_summary   = r0_api_get_memory_summary,
    .enum_pci_devices     = r0_api_enum_pci_devices,
    .enum_usb_devices     = r0_api_enum_usb_devices,
    .enum_pdv_drivers     = r0_api_enum_pdv_drivers,
    .load_pdv_file        = r0_api_load_pdv_file,
    .rescan_pdevice_dir   = r0_api_rescan_pdevice_dir,
    .enum_disks_and_parts = r0_api_enum_disks_and_parts,
    .format_partition     = r0_api_format_partition,
    .raw_disk_rw          = r0_api_raw_disk_rw,
    .vfs_remount_all      = r0_api_vfs_remount_all,
    .power_control        = r0_api_power_control,
    .sync_filesystems     = r0_api_sync_filesystems,
    .init_uninit_disk     = r0_api_init_uninit_disk,
    .get_display_caps     = pxe_kern_get_display_caps,
    .fs_create            = r0_api_fs_create,
    .fs_write             = r0_api_fs_write
};

/* =========================================================================
 * Dual-Ring ELF64 PIE / EXEC Loader & Reaper
 * ========================================================================= */

void pxe_release_r0_image(process_t* p) {
    if (!p || p->r0_slot < 0 || p->r0_slot >= PXE_R0_MAX_SLOTS) return;

    pagetable_t kpml4 = vmm_get_kernel_pml4();
    for (int s = 0; s < 4; s++) {
        if (p->r0_seg_pages[s] > 0 && p->r0_seg_vbase[s] != 0) {
            for (size_t pg = 0; pg < p->r0_seg_pages[s]; pg++) {
                vmm_unmap_page(kpml4, p->r0_seg_vbase[s] + pg * PAGE_SIZE);
            }
        }
        if (p->r0_seg_phys[s] && p->r0_seg_pages[s] > 0) {
            pmm_free_pages(p->r0_seg_phys[s], p->r0_seg_pages[s]);
        }
        p->r0_seg_vbase[s] = 0;
        p->r0_seg_phys[s]  = 0;
        p->r0_seg_pages[s] = 0;
    }

    uint64_t flags = pxe_irq_save();
    g_r0_slots[p->r0_slot] = false;
    p->r0_slot = -1;
    pxe_irq_restore(flags);
}

static bool pxe_validate_elf64(const uint8_t* elf, size_t elf_size) {
    if (!elf || elf_size < sizeof(pxe_elf64_ehdr_t)) return false;
    const pxe_elf64_ehdr_t* eh = (const pxe_elf64_ehdr_t*)elf;
    if (eh->e_ident[0] != PXE_ELF_MAG0 || eh->e_ident[1] != PXE_ELF_MAG1 ||
        eh->e_ident[2] != PXE_ELF_MAG2 || eh->e_ident[3] != PXE_ELF_MAG3) {
        return false;
    }
    if (eh->e_ident[4] != PXE_ELFCLASS64 || eh->e_ident[5] != PXE_ELFDATA2LSB) return false;
    if (eh->e_type != PXE_ET_DYN && eh->e_type != PXE_ET_EXEC) return false;
    if (eh->e_machine != PXE_EM_X86_64) return false;
    if (eh->e_phentsize < sizeof(pxe_elf64_phdr_t) || eh->e_phnum == 0) return false;
    uint64_t ph_end = eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize;
    if (ph_end > elf_size) return false;
    return true;
}

static bool pxe_write_va_via_pml4(pagetable_t pml4, uint64_t va, const void* src, size_t len) {
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < len; i++) {
        uint64_t cur = va + i;
        phys_addr_t page_phys = 0;
        if (!vmm_virt_to_phys(pml4, cur & PAGE_MASK, &page_phys)) return false;
        uint8_t* dst = (uint8_t*)phys_to_virt(page_phys) + (cur & (PAGE_SIZE - 1));
        *dst = s[i];
    }
    return true;
}

static bool pxe_write_r0_seg(process_t* p, uint64_t va, const void* src, size_t len) {
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < len; i++) {
        uint64_t cur = va + i;
        bool placed = false;
        for (int seg = 0; seg < 4; seg++) {
            uint64_t base = p->r0_seg_vbase[seg];
            uint64_t end  = base + p->r0_seg_pages[seg] * PAGE_SIZE;
            if (p->r0_seg_pages[seg] > 0 && cur >= base && cur < end) {
                uint8_t* dst = (uint8_t*)phys_to_virt(p->r0_seg_phys[seg]) + (cur - base);
                *dst = s[i];
                placed = true;
                break;
            }
        }
        if (!placed) return false;
    }
    return true;
}

static bool pxe_apply_elf_relocations(const uint8_t* elf, size_t elf_size,
                                      uint64_t load_bias, process_t* proc, bool is_ring0) {
    const pxe_elf64_ehdr_t* eh = (const pxe_elf64_ehdr_t*)elf;
    if (eh->e_shoff == 0 || eh->e_shnum == 0 ||
        eh->e_shentsize < sizeof(pxe_elf64_shdr_t)) {
        return true;
    }
    if (eh->e_shoff + (uint64_t)eh->e_shnum * eh->e_shentsize > elf_size) {
        return false;
    }

    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        const pxe_elf64_shdr_t* sh =
            (const pxe_elf64_shdr_t*)(elf + eh->e_shoff + (size_t)i * eh->e_shentsize);
        if (sh->sh_type != PXE_SHT_RELA || sh->sh_size == 0) continue;
        if (sh->sh_offset + sh->sh_size > elf_size) return false;

        const pxe_elf64_sym_t* symtab = NULL;
        size_t sym_count = 0;
        if (sh->sh_link < eh->e_shnum) {
            const pxe_elf64_shdr_t* sym_sh =
                (const pxe_elf64_shdr_t*)(elf + eh->e_shoff + (size_t)sh->sh_link * eh->e_shentsize);
            if (sym_sh->sh_offset + sym_sh->sh_size <= elf_size &&
                sym_sh->sh_entsize >= sizeof(pxe_elf64_sym_t)) {
                symtab = (const pxe_elf64_sym_t*)(elf + sym_sh->sh_offset);
                sym_count = (size_t)(sym_sh->sh_size / sizeof(pxe_elf64_sym_t));
            }
        }

        size_t rela_cnt = (size_t)(sh->sh_size / sizeof(pxe_elf64_rela_t));
        const pxe_elf64_rela_t* relas = (const pxe_elf64_rela_t*)(elf + sh->sh_offset);

        for (size_t r = 0; r < rela_cnt; r++) {
            uint32_t rtype = (uint32_t)(relas[r].r_info & 0xFFFFFFFFULL);
            uint32_t sym_i = (uint32_t)(relas[r].r_info >> 32);
            if (rtype == PXE_R_X86_64_NONE) continue;

            uint64_t target_va = load_bias + relas[r].r_offset;
            uint64_t val = 0;

            if (rtype == PXE_R_X86_64_RELATIVE) {
                val = (uint64_t)((int64_t)load_bias + relas[r].r_addend);
            } else if (rtype == PXE_R_X86_64_64 ||
                       rtype == PXE_R_X86_64_GLOB_DAT ||
                       rtype == PXE_R_X86_64_JUMP_SLOT) {
                if (!symtab || sym_i >= sym_count) return false;
                uint64_t sym_val = symtab[sym_i].st_value;
                if (sym_val != 0) sym_val += load_bias;
                if (rtype == PXE_R_X86_64_64) {
                    val = (uint64_t)((int64_t)sym_val + relas[r].r_addend);
                } else {
                    val = sym_val;
                }
            } else {
                log_printf("[PXE ] Unsupported ELF64 relocation type %u\n", rtype);
                return false;
            }

            bool ok = is_ring0
                ? pxe_write_r0_seg(proc, target_va, &val, sizeof(val))
                : pxe_write_va_via_pml4(proc->pml4, target_va, &val, sizeof(val));
            if (!ok) {
                log_printf("[PXE ] Relocation target 0x%llx out of mapped segments\n",
                           (unsigned long long)target_va);
                return false;
            }
        }
    }
    return true;
}

static process_t* pxe_load_ring3_process(const pxe_header_t* hdr, const uint8_t* elf,
                                         size_t elf_size, const char* path,
                                         int argc, const char* const* argv,
                                         process_t* parent) {
    const pxe_elf64_ehdr_t* eh = (const pxe_elf64_ehdr_t*)elf;
    if (eh->e_type != PXE_ET_DYN && eh->e_type != PXE_ET_EXEC) return NULL;
    uint64_t load_bias = (eh->e_type == PXE_ET_DYN) ? PXE_R3_PIE_BASE : 0ULL;

    const char* pname = hdr->app_name[0] ? hdr->app_name : "pxe_app";
    process_t* proc = process_create_empty(pname, parent);
    if (!proc) return NULL;

    proc->exec_ring = PXE_RING_3;
    proc->pxe_flags = hdr->flags;
    proc->is_pxe    = true;
    strncpy(proc->exe_path, path, sizeof(proc->exe_path) - 1);

    uint64_t max_seg_end = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const pxe_elf64_phdr_t* ph =
            (const pxe_elf64_phdr_t*)(elf + eh->e_phoff + (size_t)i * eh->e_phentsize);
        if (ph->p_type != PXE_PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_filesz > ph->p_memsz || ph->p_offset + ph->p_filesz > elf_size) {
            vmm_destroy_address_space(proc->pml4);
            kfree(proc);
            return NULL;
        }

        uint64_t seg_va_start = (load_bias + ph->p_vaddr) & PAGE_MASK;
        uint64_t seg_va_end   = ((load_bias + ph->p_vaddr + ph->p_memsz) + PAGE_SIZE - 1) & PAGE_MASK;
        if (seg_va_start < 0x10000ULL || seg_va_end >= 0x0000000040000000ULL) {
            vmm_destroy_address_space(proc->pml4);
            kfree(proc);
            return NULL;
        }
        if (seg_va_end > max_seg_end) max_seg_end = seg_va_end;

        uint64_t pte_flags = PTE_PRESENT | PTE_USER;
        if (ph->p_flags & PXE_PF_W) pte_flags |= PTE_WRITABLE;
        if (!(ph->p_flags & PXE_PF_X)) pte_flags |= PTE_NX;

        for (uint64_t va = seg_va_start; va < seg_va_end; va += PAGE_SIZE) {
            phys_addr_t existing = 0;
            if (!vmm_virt_to_phys(proc->pml4, va, &existing)) {
                phys_addr_t page = pmm_alloc_page();
                if (!page) {
                    vmm_destroy_address_space(proc->pml4);
                    kfree(proc);
                    return NULL;
                }
                memset(phys_to_virt(page), 0, PAGE_SIZE);
                if (!vmm_map_page(proc->pml4, va, page, pte_flags)) {
                    pmm_free_page(page);
                    vmm_destroy_address_space(proc->pml4);
                    kfree(proc);
                    return NULL;
                }
                proc->mem_pages_used++;
            }
        }

        if (ph->p_filesz > 0) {
            if (!pxe_write_va_via_pml4(proc->pml4, load_bias + ph->p_vaddr,
                                       elf + ph->p_offset, (size_t)ph->p_filesz)) {
                vmm_destroy_address_space(proc->pml4);
                kfree(proc);
                return NULL;
            }
        }
    }

    if (eh->e_type == PXE_ET_DYN) {
        if (!pxe_apply_elf_relocations(elf, elf_size, load_bias, proc, false)) {
            vmm_destroy_address_space(proc->pml4);
            kfree(proc);
            return NULL;
        }
    }

    /* Configure Ring 3 heap above loaded segments */
    uint64_t heap_start = (max_seg_end + 0xFFFFFULL) & ~0xFFFFFULL;
    if (heap_start < 0x0000000010000000ULL) heap_start = 0x0000000010000000ULL;
    proc->heap_base  = heap_start;
    proc->heap_brk   = heap_start;
    proc->heap_limit = 0x0000000050000000ULL;
    if (hdr->heap_init_size > 0) {
        uint32_t init_heap = hdr->heap_init_size;
        if (init_heap > 8U * 1024U * 1024U) init_heap = 8U * 1024U * 1024U;
        (void)process_sbrk(proc, (int64_t)init_heap);
    }

    /* Allocate Ring 3 user stack & build System V argc/argv frame */
    size_t stack_pages = (hdr->stack_size > 0)
        ? ((hdr->stack_size + PAGE_SIZE - 1) / PAGE_SIZE)
        : 16;
    if (stack_pages < 4) stack_pages = 4;
    if (stack_pages > 128) stack_pages = 128;

    uint64_t stack_base = PXE_R3_STACK_TOP - stack_pages * PAGE_SIZE;
    for (size_t pg = 0; pg < stack_pages; pg++) {
        phys_addr_t spage = pmm_alloc_page();
        if (!spage) {
            vmm_destroy_address_space(proc->pml4);
            kfree(proc);
            return NULL;
        }
        memset(phys_to_virt(spage), 0, PAGE_SIZE);
        if (!vmm_map_page(proc->pml4, stack_base + pg * PAGE_SIZE, spage,
                          PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)) {
            pmm_free_page(spage);
            vmm_destroy_address_space(proc->pml4);
            kfree(proc);
            return NULL;
        }
        proc->mem_pages_used++;
    }

    int safe_argc = (argc > 0) ? argc : 1;
    if (safe_argc > 16) safe_argc = 16;
    uint64_t arg_ptrs[16];
    uint64_t sp = PXE_R3_STACK_TOP - 64;

    for (int i = safe_argc - 1; i >= 0; i--) {
        const char* s = (argv && i < argc && argv[i]) ? argv[i] : pname;
        size_t slen = strlen(s) + 1;
        if (slen > 192) slen = 192;
        sp -= slen;
        pxe_write_va_via_pml4(proc->pml4, sp, s, slen - 1);
        uint8_t nul = 0;
        pxe_write_va_via_pml4(proc->pml4, sp + slen - 1, &nul, 1);
        arg_ptrs[i] = sp;
    }

    sp &= ~0xFULL;
    if (((safe_argc + 2) & 1) != 0) {
        sp -= 8;
    }
    uint64_t zero64 = 0;
    sp -= 8;
    pxe_write_va_via_pml4(proc->pml4, sp, &zero64, 8);
    for (int i = safe_argc - 1; i >= 0; i--) {
        sp -= 8;
        pxe_write_va_via_pml4(proc->pml4, sp, &arg_ptrs[i], 8);
    }
    uint64_t argc64 = (uint64_t)safe_argc;
    sp -= 8;
    pxe_write_va_via_pml4(proc->pml4, sp, &argc64, 8);

    uint64_t entry_va = load_bias + eh->e_entry;
    process_register(proc);
    if (!process_create_user_thread(proc, entry_va, sp)) {
        process_unlink(proc);
        vmm_destroy_address_space(proc->pml4);
        kfree(proc);
        return NULL;
    }
    return proc;
}

static process_t* pxe_load_ring0_process(const pxe_header_t* hdr, const uint8_t* elf,
                                         size_t elf_size, const char* path,
                                         int argc, const char* const* argv,
                                         process_t* parent) {
    const pxe_elf64_ehdr_t* eh = (const pxe_elf64_ehdr_t*)elf;
    if (eh->e_type != PXE_ET_DYN) {
        log_printf("[PXE ] Ring 0 PXE '%s' must be ELF64 PIE (ET_DYN).\n", path);
        return NULL;
    }

    uint64_t flags = pxe_irq_save();
    int slot = -1;
    for (int i = 0; i < PXE_R0_MAX_SLOTS; i++) {
        if (!g_r0_slots[i]) {
            g_r0_slots[i] = true;
            slot = i;
            break;
        }
    }
    pxe_irq_restore(flags);
    if (slot < 0) {
        log_printf("[PXE ] All %d Ring 0 PXE slots are in use.\n", PXE_R0_MAX_SLOTS);
        return NULL;
    }

    uint64_t load_bias = PXE_R0_ARENA_BASE + (uint64_t)slot * PXE_R0_SLOT_STRIDE;
    const char* pname = hdr->app_name[0] ? hdr->app_name : "pxe_r0";
    process_t* proc = process_create_r0_empty(pname, parent);
    if (!proc) {
        g_r0_slots[slot] = false;
        return NULL;
    }

    proc->exec_ring   = PXE_RING_0;
    proc->pxe_flags   = hdr->flags;
    proc->is_pxe      = true;
    proc->r0_slot     = slot;
    proc->r0_mod_base = load_bias;
    strncpy(proc->exe_path, path, sizeof(proc->exe_path) - 1);

    uint32_t seg_pte_flags[4] = {0, 0, 0, 0};
    int seg_idx = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const pxe_elf64_phdr_t* ph =
            (const pxe_elf64_phdr_t*)(elf + eh->e_phoff + (size_t)i * eh->e_phentsize);
        if (ph->p_type != PXE_PT_LOAD || ph->p_memsz == 0) continue;
        if (seg_idx >= 4 || ph->p_filesz > ph->p_memsz ||
            ph->p_offset + ph->p_filesz > elf_size) {
            pxe_release_r0_image(proc);
            kfree(proc);
            return NULL;
        }

        uint64_t rel_start = ph->p_vaddr & PAGE_MASK;
        uint64_t rel_end   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & PAGE_MASK;
        if (rel_end > PXE_R0_SLOT_STRIDE) {
            pxe_release_r0_image(proc);
            kfree(proc);
            return NULL;
        }

        size_t pages = (size_t)((rel_end - rel_start) / PAGE_SIZE);
        phys_addr_t phys = pmm_alloc_pages(pages);
        if (!phys) {
            pxe_release_r0_image(proc);
            kfree(proc);
            return NULL;
        }
        memset(phys_to_virt(phys), 0, pages * PAGE_SIZE);

        if (ph->p_filesz > 0) {
            size_t page_off = (size_t)(ph->p_vaddr - rel_start);
            memcpy((uint8_t*)phys_to_virt(phys) + page_off,
                   elf + ph->p_offset, (size_t)ph->p_filesz);
        }

        proc->r0_seg_vbase[seg_idx] = load_bias + rel_start;
        proc->r0_seg_phys[seg_idx]  = phys;
        proc->r0_seg_pages[seg_idx] = pages;
        proc->mem_pages_used       += pages;
        seg_pte_flags[seg_idx]      = ph->p_flags;
        seg_idx++;
    }

    if (seg_idx == 0 || !pxe_apply_elf_relocations(elf, elf_size, load_bias, proc, true)) {
        pxe_release_r0_image(proc);
        kfree(proc);
        return NULL;
    }

    /* Enforce strict W^X PTEs in kernel PML4 */
    pagetable_t kpml4 = vmm_get_kernel_pml4();
    for (int s = 0; s < seg_idx; s++) {
        uint64_t pte = PTE_PRESENT;
        if (seg_pte_flags[s] & PXE_PF_X) {
            /* R-X code segment */
            pte = PTE_PRESENT;
        } else if (seg_pte_flags[s] & PXE_PF_W) {
            /* RW- data/bss/got segment */
            pte = PTE_PRESENT | PTE_WRITABLE | PTE_NX;
        } else {
            /* R-- rodata segment */
            pte = PTE_PRESENT | PTE_NX;
        }
        for (size_t pg = 0; pg < proc->r0_seg_pages[s]; pg++) {
            if (!vmm_map_page(kpml4,
                              proc->r0_seg_vbase[s] + pg * PAGE_SIZE,
                              proc->r0_seg_phys[s] + pg * PAGE_SIZE,
                              pte)) {
                pxe_release_r0_image(proc);
                kfree(proc);
                return NULL;
            }
        }
    }

    size_t stack_pages = (hdr->stack_size > 0)
        ? ((hdr->stack_size + PAGE_SIZE - 1) / PAGE_SIZE)
        : 16;
    if (stack_pages < 8) stack_pages = 8;
    if (stack_pages > 64) stack_pages = 64;

    uint64_t entry_va = load_bias + eh->e_entry;
    process_register(proc);
    if (!process_create_r0_thread(proc, entry_va, argc, argv,
                                  (uint64_t)(uintptr_t)&g_pxe_ring0_api, stack_pages)) {
        process_unlink(proc);
        pxe_release_r0_image(proc);
        kfree(proc);
        return NULL;
    }
    return proc;
}

int pxe_spawn_file(const char* path, int argc, const char* const* argv,
                   process_t* parent, bool bypass_r0_prompt) {
    if (!path || !path[0]) return -NEKO_ENOENT;

    vfs_dirent_t dent;
    if (vfs_find_file(path, &dent) != VFS_OK || dent.is_dir) {
        log_printf("[PXE ] File not found: %s\n", path);
        return -NEKO_ENOENT;
    }
    if (dent.size < sizeof(pxe_header_t) || dent.size > PXE_MAX_FILE_BYTES) {
        log_printf("[PXE ] Invalid PXE file size (%llu bytes): %s\n",
                   (unsigned long long)dent.size, path);
        return -NEKO_EINVAL;
    }

    size_t file_sz = (size_t)dent.size;
    uint8_t* raw = (uint8_t*)kmalloc(file_sz);
    if (!raw) return -NEKO_ENOMEM;

    int rd = vfs_read_file(path, raw, (unsigned int)file_sz);
    if (rd < 0 || (size_t)rd != file_sz) {
        kfree(raw);
        return -NEKO_ENOENT;
    }

    const pxe_header_t* hdr = (const pxe_header_t*)raw;
    if (hdr->magic != PXE_MAGIC ||
        hdr->abi_version != PXE_ABI_VERSION_1_0 ||
        hdr->arch != PXE_ARCH_X86_64 ||
        hdr->header_size < sizeof(pxe_header_t) ||
        (hdr->exec_ring != PXE_RING_3 && hdr->exec_ring != PXE_RING_0)) {
        log_printf("[PXE ] Invalid PXE header in %s\n", path);
        kfree(raw);
        return -NEKO_EINVAL;
    }

    uint32_t calc_crc = pxe_compute_image_crc32(raw, file_sz);
    if (calc_crc != hdr->checksum_crc32) {
        log_printf("[PXE ] CRC32 mismatch in %s (expected 0x%08x, got 0x%08x)\n",
                   path, hdr->checksum_crc32, calc_crc);
        kfree(raw);
        return -NEKO_EINVAL;
    }

    if ((uint64_t)hdr->elf_offset + (uint64_t)hdr->elf_size > file_sz ||
        hdr->elf_size < sizeof(pxe_elf64_ehdr_t)) {
        log_printf("[PXE ] Invalid ELF payload bounds in %s\n", path);
        kfree(raw);
        return -NEKO_EINVAL;
    }

    /* Single-instance check: focus existing live process if already running */
    if (hdr->flags & PXE_FLAG_SINGLE_INST) {
        uint64_t irq_f = pxe_irq_save();
        process_t* existing = NULL;
        for (process_t* q = process_head(); q; q = q->next) {
            if (q->state != PROCESS_ZOMBIE && q->is_pxe &&
                (pxe_strcasecmp(q->exe_path, path) == 0 ||
                 (hdr->app_name[0] && pxe_strcasecmp(q->name, hdr->app_name) == 0))) {
                existing = q;
                break;
            }
        }
        uint32_t existing_pid = existing ? existing->pid : 0;
        pxe_irq_restore(irq_f);

        if (existing_pid != 0) {
            const char* arg1 = (argv && argc > 1) ? argv[1] : NULL;
            wm_pxe_focus_pid(existing_pid, arg1);
            kfree(raw);
            return (int)existing_pid;
        }
    }

    /* Gate untrusted Ring 0 binaries behind desktop user confirmation */
    if (hdr->exec_ring == PXE_RING_0 && !bypass_r0_prompt &&
        !pxe_is_trusted_apps_path(path)) {
        const char* arg1 = (argv && argc > 1) ? argv[1] : NULL;
        kfree(raw);
        wm_request_ring0_launch(path, arg1);
        return 0;
    }

    const uint8_t* elf_ptr = raw + hdr->elf_offset;
    size_t elf_sz = (size_t)hdr->elf_size;
    if (!pxe_validate_elf64(elf_ptr, elf_sz)) {
        log_printf("[PXE ] Invalid embedded ELF64 in %s\n", path);
        kfree(raw);
        return -NEKO_EINVAL;
    }

    const char* def_argv[1] = { path };
    int final_argc = (argc > 0 && argv) ? argc : 1;
    const char* const* final_argv = (argc > 0 && argv) ? argv : def_argv;

    process_t* child = (hdr->exec_ring == PXE_RING_0)
        ? pxe_load_ring0_process(hdr, elf_ptr, elf_sz, path, final_argc, final_argv, parent)
        : pxe_load_ring3_process(hdr, elf_ptr, elf_sz, path, final_argc, final_argv, parent);

    kfree(raw);
    if (!child) {
        log_printf("[PXE ] Failed to spawn %s\n", path);
        return -NEKO_ENOMEM;
    }

    log_printf("[PXE ] Spawned '%s' (PID=%u, Ring %u, path=%s)\n",
               child->name, child->pid, (unsigned int)child->exec_ring, path);
    return (int)child->pid;
}

static bool pxe_try_apps_candidate(char drv, const char* filename, char* out_path, int out_size) {
    if (drv < 'A' || drv > 'Z' || !filename || !*filename || !out_path) return false;
    int flen = (int)strlen(filename);
    if (8 + flen + 1 > out_size) return false;
    out_path[0] = drv;
    out_path[1] = ':';
    out_path[2] = '\\';
    out_path[3] = 'A';
    out_path[4] = 'P';
    out_path[5] = 'P';
    out_path[6] = 'S';
    out_path[7] = '\\';
    memcpy(out_path + 8, filename, (size_t)flen + 1);

    vfs_stream_t stream;
    if (vfs_stream_open(out_path, &stream) == 0) {
        vfs_stream_close(&stream);
        return true;
    }
    return false;
}

bool pxe_resolve_builtin_app_path(const char* filename, char* out_path, int out_size) {
    if (!filename || !*filename || !out_path || out_size < 16) return false;

    char sys_drv = settings_get_system_drive();
    char cd_drv  = vfs_get_boot_cdrom_letter();

    if (sys_drv && pxe_try_apps_candidate(sys_drv, filename, out_path, out_size)) {
        return true;
    }
    if (cd_drv && cd_drv != sys_drv &&
        pxe_try_apps_candidate(cd_drv, filename, out_path, out_size)) {
        return true;
    }
    for (char d = 'C'; d <= 'Z'; d++) {
        if (d == sys_drv || d == cd_drv) continue;
        if (pxe_try_apps_candidate(d, filename, out_path, out_size)) {
            return true;
        }
    }
    out_path[0] = '\0';
    return false;
}

bool pxe_load_builtin_icon16(const char* filename, uint8_t out_icon[16][16]) {
    if (!filename || !*filename || !out_icon) return false;
    char path[96];
    if (!pxe_resolve_builtin_app_path(filename, path, (int)sizeof(path))) return false;

    vfs_stream_t stream;
    if (vfs_stream_open(path, &stream) != 0) return false;

    pxe_header_t hdr;
    int hn = vfs_stream_read(&stream, &hdr, sizeof(hdr));
    if (hn != (int)sizeof(hdr) || hdr.magic != PXE_MAGIC ||
        hdr.rsrc_size < sizeof(pxe_resource_t) || hdr.rsrc_offset < sizeof(hdr)) {
        vfs_stream_close(&stream);
        return false;
    }

    if (vfs_stream_seek(&stream, (uint64_t)hdr.rsrc_offset) != 0) {
        vfs_stream_close(&stream);
        return false;
    }

    pxe_resource_t rsrc;
    int rn = vfs_stream_read(&stream, &rsrc, sizeof(rsrc));
    vfs_stream_close(&stream);
    if (rn != (int)sizeof(rsrc)) return false;

    bool has_pixel = false;
    for (int i = 0; i < 256; i++) {
        if (rsrc.icon_16x16[i] != 255u) {
            has_pixel = true;
            break;
        }
    }
    if (!has_pixel) return false;

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            out_icon[y][x] = rsrc.icon_16x16[y * 16 + x];
        }
    }
    return true;
}

int pxe_launch_builtin_app(const char* filename, const char* arg1, bool bypass_r0_prompt) {
    char path[96];
    if (!pxe_resolve_builtin_app_path(filename, path, (int)sizeof(path))) {
        log_printf("[PXE ] Built-in app not found: %s\n", filename ? filename : "(null)");
        wm_notify_banner("App not found in APPS\\");
        return -1;
    }

    const char* argv[2];
    argv[0] = path;
    argv[1] = (arg1 && arg1[0]) ? arg1 : NULL;
    int argc = (arg1 && arg1[0]) ? 2 : 1;
    int pid = pxe_spawn_file(path, argc, argv, NULL, bypass_r0_prompt);
    if (pid < 0) {
        wm_notify_banner("Failed to launch app");
        return pid;
    }
    return pid;
}

int pxe_ui_launch_from_path(const char* path, const char* arg1) {
    if (!path || !path[0]) return -1;

    vfs_stream_t stream;
    if (vfs_stream_open(path, &stream) != 0) {
        wm_notify_banner("Cannot open PXE");
        return -1;
    }
    pxe_header_t hdr;
    int n = vfs_stream_read(&stream, &hdr, sizeof(hdr));
    vfs_stream_close(&stream);
    if (n != (int)sizeof(hdr) || hdr.magic != PXE_MAGIC) {
        wm_notify_banner("Invalid PXE file");
        return -1;
    }

    const char* argv[2];
    argv[0] = path;
    argv[1] = (arg1 && arg1[0]) ? arg1 : NULL;
    int argc = (arg1 && arg1[0]) ? 2 : 1;
    int pid = pxe_spawn_file(path, argc, argv, NULL, false);
    if (pid < 0) {
        wm_notify_banner("PXE launch failed");
    }
    return pid;
}

