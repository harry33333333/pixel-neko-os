#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/fs.h>
#include <pxe/mem.h>
#include <pxe/str.h>
#include <pxe/ring0.h>

#define TAB_OVERVIEW 0
#define TAB_PCI      1
#define TAB_USB      2
#define TAB_DRIVES   3
#define TAB_PDV      4
#define TAB_COUNT    5

#define MAX_PCI 64
#define MAX_USB 64
#define MAX_PDV 32

#define ROW_H   16
#define SB_W    14

typedef struct {
    int win_id;
    int tab;
    int scroll[TAB_COUNT];
    int selected[TAB_COUNT];
    int sb_dragging;
    int sb_drag_start_y;
    int sb_drag_start_off;

    pxe_basic_sysinfo_t info;
    pxe_display_caps_t  disp;
    pxe_r0_mem_info_t   mem;
    pxe_r0_pci_info_t   pci[MAX_PCI];
    int pci_count;
    pxe_r0_usb_info_t   usb[MAX_USB];
    int usb_count;
    pxe_drive_info_t    drives[26];
    int drive_count;
    pxe_r0_pdv_info_t   pdv[MAX_PDV];
    int pdv_count;

    char cpu_features[96];
    char notice[96];
    int  notice_is_err;

    int  load_modal;
    char load_path[128];
    int  load_len;
} sysinfo_app_t;

static const char* k_tab_names[TAB_COUNT] = {
    "Overview", "PCI Bus", "USB", "Drives", "Drivers (.pdv)"
};

static const int k_tab_x[TAB_COUNT] = { 8, 86, 160, 214, 286 };
static const int k_tab_w[TAB_COUNT] = { 74, 70, 50, 68, 118 };

static void query_cpu_features(char* out, size_t cap) {
    uint32_t max_leaf = 0, b = 0, c = 0, d = 0;
    __asm__ volatile ("cpuid" : "=a"(max_leaf), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));

    out[0] = '\0';
    size_t pos = 0;

    if (max_leaf >= 1) {
        uint32_t a1 = 0, b1 = 0, c1 = 0, d1 = 0;
        __asm__ volatile ("cpuid" : "=a"(a1), "=b"(b1), "=c"(c1), "=d"(d1) : "a"(1), "c"(0));
        (void)a1; (void)b1;

        const struct { uint32_t reg_is_c; uint32_t bit; const char* name; } feats[] = {
            { 0, 0,  "FPU" },
            { 0, 9,  "APIC" },
            { 0, 23, "MMX" },
            { 0, 25, "SSE" },
            { 0, 26, "SSE2" },
            { 1, 0,  "SSE3" },
            { 1, 9,  "SSSE3" },
            { 1, 19, "SSE4.1" },
            { 1, 20, "SSE4.2" },
            { 1, 28, "AVX" },
        };
        for (size_t i = 0; i < sizeof(feats) / sizeof(feats[0]); i++) {
            uint32_t v = feats[i].reg_is_c ? c1 : d1;
            if (v & (1u << feats[i].bit)) {
                int n = snprintf(out + pos, cap - pos, "%s%s",
                                 pos > 0 ? " " : "", feats[i].name);
                if (n > 0 && pos + (size_t)n < cap) pos += (size_t)n;
            }
        }
    }

    uint32_t max_ext = 0;
    __asm__ volatile ("cpuid" : "=a"(max_ext), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (max_ext >= 0x80000001u) {
        uint32_t ax = 0, bx = 0, cx = 0, dx = 0;
        __asm__ volatile ("cpuid" : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx) : "a"(0x80000001u), "c"(0));
        (void)ax; (void)bx; (void)cx;
        if (dx & (1u << 20)) {
            int n = snprintf(out + pos, cap - pos, "%sNX", pos > 0 ? " " : "");
            if (n > 0 && pos + (size_t)n < cap) pos += (size_t)n;
        }
        if (dx & (1u << 29)) {
            int n = snprintf(out + pos, cap - pos, "%sLM64", pos > 0 ? " " : "");
            if (n > 0 && pos + (size_t)n < cap) pos += (size_t)n;
        }
    }

    if (out[0] == '\0') {
        snprintf(out, cap, "x86_64 Standard");
    }
}

static void fmt_kb(uint64_t kb, char* out, size_t cap) {
    if (kb >= 1048576ull) {
        snprintf(out, cap, "%u.%u GB",
                 (unsigned)(kb / 1048576ull),
                 (unsigned)(((kb % 1048576ull) * 10ull) / 1048576ull));
    } else if (kb >= 1024ull) {
        snprintf(out, cap, "%u.%u MB",
                 (unsigned)(kb / 1024ull),
                 (unsigned)(((kb % 1024ull) * 10ull) / 1024ull));
    } else {
        snprintf(out, cap, "%u KB", (unsigned)kb);
    }
}

static const char* pdv_class_name(uint16_t cls) {
    switch (cls) {
        case 1:  return "Display";
        case 2:  return "Audio";
        case 3:  return "Input";
        case 4:  return "Storage";
        case 5:  return "Network";
        case 6:  return "System";
        default: return "Unknown";
    }
}

static void refresh_all(sysinfo_app_t* s) {
    memset(&s->info, 0, sizeof(s->info));
    pxe_sys_get_basic_info(&s->info);

    memset(&s->disp, 0, sizeof(s->disp));
    (void)pxe_display_get_caps(&s->disp);

    memset(&s->mem, 0, sizeof(s->mem));
    pxe_r0_get_memory_summary(&s->mem);

    int n = pxe_r0_enum_pci_devices(s->pci, MAX_PCI);
    s->pci_count = (n > 0) ? n : 0;

    n = pxe_r0_enum_usb_devices(s->usb, MAX_USB);
    s->usb_count = (n > 0) ? n : 0;

    n = pxe_get_drives(s->drives, 26);
    s->drive_count = (n > 0) ? n : 0;

    n = pxe_r0_enum_pdv_drivers(s->pdv, MAX_PDV);
    s->pdv_count = (n > 0) ? n : 0;

    for (int t = 0; t < TAB_COUNT; t++) {
        int cnt = 0;
        if (t == TAB_PCI) cnt = s->pci_count;
        else if (t == TAB_USB) cnt = s->usb_count;
        else if (t == TAB_DRIVES) cnt = s->drive_count;
        else if (t == TAB_PDV) cnt = s->pdv_count;
        if (s->selected[t] >= cnt) s->selected[t] = cnt > 0 ? (cnt - 1) : -1;
        if (s->scroll[t] >= cnt) s->scroll[t] = 0;
    }
}

static int tab_item_count(const sysinfo_app_t* s, int tab) {
    switch (tab) {
        case TAB_PCI:    return s->pci_count;
        case TAB_USB:    return s->usb_count;
        case TAB_DRIVES: return s->drive_count;
        case TAB_PDV:    return s->pdv_count;
        default:         return 0;
    }
}

static void get_list_geom(const pxe_win_surface_t* surf, int tab,
                          int* lx, int* ly, int* lw, int* lh, int* vis) {
    *lx = 10;
    *ly = 56;
    *lw = surf->width - 20 - SB_W;
    *lh = (tab == TAB_PDV) ? (surf->height - 122) : (surf->height - 92);
    if (*lh < ROW_H * 2) *lh = ROW_H * 2;
    *vis = (*lh - 4) / ROW_H;
    if (*vis < 1) *vis = 1;
}

static void draw_kv(const pxe_win_surface_t* surf, int x, int y,
                    const char* key, const char* val, uint8_t vcol) {
    pxe_draw_text_small(surf, x, y, key, PXE_COL_DARK_GRAY);
    pxe_draw_text_small(surf, x + 98, y, val, vcol);
}

static void render_overview(const pxe_win_surface_t* surf, const sysinfo_app_t* s) {
    char buf[128];
    int x = 18;
    int y = 44;
    int rw = surf->width - 36;

    pxe_draw_text(surf, x, y, s->info.os_name[0] ? s->info.os_name : "Pixel Neko OS", PXE_COL_DARK_BLUE);
    snprintf(buf, sizeof(buf), "Build %s (%s) [Ring 0 App]",
             s->info.os_build[0] ? s->info.os_build : "unknown",
             s->info.os_codename[0] ? s->info.os_codename : "Darwin");
    pxe_draw_text_small(surf, x, y + 18, buf, PXE_COL_BLACK);

    if (s->info.boot_mode == 2) {
        snprintf(buf, sizeof(buf), "Local Installed Disk (%c:\\) | CD: %c:\\",
                 s->info.system_drive ? s->info.system_drive : 'C',
                 s->info.cdrom_drive ? s->info.cdrom_drive : '-');
    } else if (s->info.boot_mode == 1) {
        snprintf(buf, sizeof(buf), "Installer Environment (Live CD %c:\\)",
                 s->info.cdrom_drive ? s->info.cdrom_drive : (s->info.system_drive ? s->info.system_drive : 'D'));
    } else {
        snprintf(buf, sizeof(buf), "Live CD Mode (%c:\\)",
                 s->info.cdrom_drive ? s->info.cdrom_drive : (s->info.system_drive ? s->info.system_drive : 'D'));
    }
    draw_kv(surf, x, y + 34, "Boot Mode:", buf, PXE_COL_BLACK);

    pxe_draw_hline(surf, x, y + 48, rw, PXE_COL_DARK_GRAY);
    y += 56;

    draw_kv(surf, x, y, "Processor:",
            s->info.cpu_model[0] ? s->info.cpu_model : "x86_64 Compatible CPU", PXE_COL_BLACK);
    snprintf(buf, sizeof(buf), "%s | %u MHz | flags=0x%X",
             s->info.cpu_vendor[0] ? s->info.cpu_vendor : "Unknown",
             (unsigned)s->info.cpu_freq_mhz,
             (unsigned)s->info.cpu_flags);
    draw_kv(surf, x, y + 16, "CPU Clock:", buf, PXE_COL_BLACK);
    draw_kv(surf, x, y + 32, "CPU Features:", s->cpu_features, PXE_COL_DARK_GREEN);

    pxe_draw_hline(surf, x, y + 46, rw, PXE_COL_DARK_GRAY);
    y += 54;

    snprintf(buf, sizeof(buf), "%u MB Total | %u MB Usable | %u MB Free",
             (unsigned)s->info.total_ram_mb,
             (unsigned)s->info.usable_ram_mb,
             (unsigned)s->info.free_ram_mb);
    draw_kv(surf, x, y, "Physical RAM:", buf, PXE_COL_BLACK);

    snprintf(buf, sizeof(buf), "%llu free / %llu total pages | Procs: %llu | PDVs: %llu",
             (unsigned long long)s->mem.free_pages,
             (unsigned long long)s->mem.total_pages,
             (unsigned long long)s->mem.process_count,
             (unsigned long long)s->mem.pdv_loaded_count);
    draw_kv(surf, x, y + 16, "Kernel Pages:", buf, PXE_COL_BLACK);

    pxe_draw_hline(surf, x, y + 30, rw, PXE_COL_DARK_GRAY);
    y += 38;

    char vram_str[24];
    fmt_kb(s->info.vram_bytes / 1024ull, vram_str, sizeof(vram_str));
    if (s->disp.struct_size == sizeof(s->disp)) {
        snprintf(buf, sizeof(buf), "%dx%d @ %dbpp | VRAM %s | Max %dx%d",
                 s->info.screen_w, s->info.screen_h, s->info.screen_bpp,
                 vram_str, s->disp.max_w, s->disp.max_h);
    } else {
        snprintf(buf, sizeof(buf), "%dx%d @ %dbpp | VRAM %s | %s",
                 s->info.screen_w, s->info.screen_h, s->info.screen_bpp,
                 vram_str,
                 s->info.gfx_backend[0] ? s->info.gfx_backend : "Framebuffer");
    }
    draw_kv(surf, x, y, "Display:", buf, PXE_COL_BLACK);

    snprintf(buf, sizeof(buf), "2D:%s | Drv:%s | %s",
             s->info.gfx_is_2d_hw ? "Yes" : "No",
             (s->disp.flags & PXE_DISP_FLAG_DRIVER_ACTIVE) ? "Active" : "Fallback",
             s->info.gfx_status[0] ? s->info.gfx_status
                                   : (s->info.gfx_fallback[0] ? s->info.gfx_fallback : "OK"));
    draw_kv(surf, x, y + 16, "Gfx Accel:", buf,
            s->info.gfx_is_2d_hw ? PXE_COL_DARK_GREEN : PXE_COL_DARK_BLUE);

    snprintf(buf, sizeof(buf), "%s | %s%s",
             s->info.has_audio_hw ? "Present" : "None",
             s->info.audio_backend[0] ? s->info.audio_backend : "No Codec",
             s->info.audio_muted ? " [Muted]" : "");
    draw_kv(surf, x, y + 32, "Audio HW:", buf, PXE_COL_BLACK);
    if (s->info.audio_probe[0]) {
        draw_kv(surf, x, y + 48, "Audio Probe:", s->info.audio_probe, PXE_COL_DARK_GRAY);
    }

    pxe_draw_hline(surf, x, y + 64, rw, PXE_COL_DARK_GRAY);
    snprintf(buf, sizeof(buf), "Devices: %d PCI | %d USB | %d Drives | %d PDV Drivers",
             s->pci_count, s->usb_count, s->drive_count, s->pdv_count);
    pxe_draw_text_small(surf, x, y + 74, buf, PXE_COL_DARK_BLUE);
}

static void render_table(const pxe_win_surface_t* surf, const sysinfo_app_t* s) {
    int lx, ly, lw, lh, vis;
    get_list_geom(surf, s->tab, &lx, &ly, &lw, &lh, &vis);

    /* Column Header */
    pxe_fill_rect(surf, lx, ly - 18, lw + SB_W, 18, PXE_COL_LIGHT_GRAY);
    pxe_draw_rect(surf, lx, ly - 18, lw + SB_W, 18, PXE_COL_DARK_GRAY);
    if (s->tab == TAB_PCI) {
        pxe_draw_text_small(surf, lx + 6, ly - 13,
                            "Bus:Sl.F  Ven:Dev    Class                 IRQ  BAR0",
                            PXE_COL_BLACK);
    } else if (s->tab == TAB_USB) {
        pxe_draw_text_small(surf, lx + 6, ly - 13,
                            "Addr Port Speed     HCI        Ven:Prod   Cls/Sub/Pr EP",
                            PXE_COL_BLACK);
    } else if (s->tab == TAB_DRIVES) {
        pxe_draw_text_small(surf, lx + 6, ly - 13,
                            "Drv  FS       Label             Total      Free / Access",
                            PXE_COL_BLACK);
    } else if (s->tab == TAB_PDV) {
        pxe_draw_text_small(surf, lx + 6, ly - 13,
                            "File          Driver Name       Class    Ver     R/O/W Pg",
                            PXE_COL_BLACK);
    }

    pxe_draw_bevel_box(surf, lx, ly, lw, lh, 1, PXE_COL_WHITE);

    int count = tab_item_count(s, s->tab);
    int scroll = s->scroll[s->tab];
    int sel = s->selected[s->tab];

    if (count == 0) {
        const char* empty_msg = "No entries reported.";
        if (s->tab == TAB_USB) empty_msg = "No USB devices connected / enumerated.";
        else if (s->tab == TAB_PDV) empty_msg = "No .pdv drivers loaded. Click Rescan PDEVICE\\.";
        pxe_draw_text_small(surf, lx + 12, ly + 16, empty_msg, PXE_COL_DARK_GRAY);
    }

    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx >= count) break;
        int ry = ly + 2 + i * ROW_H;
        int is_sel = (idx == sel);
        uint8_t fg = is_sel ? PXE_COL_WHITE : PXE_COL_BLACK;
        if (is_sel) {
            pxe_fill_rect(surf, lx + 2, ry, lw - 4, ROW_H, PXE_COL_DARK_BLUE);
        }

        char line[96];
        line[0] = '\0';
        if (s->tab == TAB_PCI) {
            const pxe_r0_pci_info_t* p = &s->pci[idx];
            snprintf(line, sizeof(line), "%02X:%02X.%X   %04X:%04X  %-20.20s  %3u  0x%llX",
                     (unsigned)p->bus, (unsigned)p->slot, (unsigned)p->function,
                     (unsigned)p->vendor_id, (unsigned)p->device_id,
                     p->class_name[0] ? p->class_name : "PCI Device",
                     (unsigned)p->irq_line,
                     (unsigned long long)p->bar0_addr);
        } else if (s->tab == TAB_USB) {
            const pxe_r0_usb_info_t* u = &s->usb[idx];
            snprintf(line, sizeof(line), "%3u  %3u  %-9.9s %-10.10s %04X:%04X  %02X/%02X/%02X   %u",
                     (unsigned)u->address, (unsigned)u->port,
                     u->speed_name[0] ? u->speed_name : "Unknown",
                     u->hci_name[0] ? u->hci_name : "USB",
                     (unsigned)u->vendor_id, (unsigned)u->product_id,
                     (unsigned)u->class_code, (unsigned)u->subclass, (unsigned)u->protocol,
                     (unsigned)u->endpoint_count);
        } else if (s->tab == TAB_DRIVES) {
            const pxe_drive_info_t* d = &s->drives[idx];
            char tot[16], fre[20];
            fmt_kb(d->total_kb, tot, sizeof(tot));
            if (d->is_readonly) snprintf(fre, sizeof(fre), "Read-Only");
            else if (d->is_free_known) fmt_kb(d->free_kb, fre, sizeof(fre));
            else snprintf(fre, sizeof(fre), "--");
            snprintf(line, sizeof(line), "%c:\\  %-8.8s %-16.16s  %-9.9s  %s",
                     d->letter ? d->letter : '?',
                     d->fs_name[0] ? d->fs_name : "RAW",
                     d->label[0] ? d->label : "(no label)",
                     tot, fre);
        } else if (s->tab == TAB_PDV) {
            const pxe_r0_pdv_info_t* d = &s->pdv[idx];
            snprintf(line, sizeof(line), "%-13.13s %-17.17s %-7.7s %-7.7s %u/%u/%u",
                     d->filename[0] ? d->filename : "-",
                     d->driver_name[0] ? d->driver_name : "(unnamed)",
                     pdv_class_name(d->driver_class),
                     d->version_str[0] ? d->version_str : "-",
                     (unsigned)d->rx_pages, (unsigned)d->ro_pages, (unsigned)d->rw_pages);
        }
        int maxc = (lw - 12) / 6;
        if (maxc > 3 && (int)strlen(line) > maxc) line[maxc] = '\0';
        pxe_draw_text_small(surf, lx + 6, ry + 4, line, fg);
    }

    pxe_draw_scrollbar(surf, lx + lw, ly, SB_W, lh, scroll, vis, count);

    if (s->tab == TAB_PDV) {
        int by = ly + lh + 6;
        pxe_draw_button(surf, 10,  by, 134, 22, "Rescan PDEVICE\\", 0, 1);
        pxe_draw_button(surf, 152, by, 104, 22, "Load .pdv...", 0, 1);
    }
}

static void render_sysinfo(const pxe_win_surface_t* surf, const sysinfo_app_t* s) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    /* Tab strip + Refresh button */
    for (int t = 0; t < TAB_COUNT; t++) {
        int active = (s->tab == t);
        int ty = active ? 6 : 8;
        int th = active ? 25 : 22;
        uint8_t bg = active ? PXE_COL_WHITE : PXE_COL_LIGHT_GRAY;
        pxe_fill_rect(surf, k_tab_x[t], ty, k_tab_w[t], th, bg);
        pxe_draw_hline(surf, k_tab_x[t], ty, k_tab_w[t], PXE_COL_WHITE);
        pxe_draw_vline(surf, k_tab_x[t], ty, th, PXE_COL_WHITE);
        pxe_draw_vline(surf, k_tab_x[t] + k_tab_w[t] - 1, ty, th, PXE_COL_DARK_GRAY);
        if (!active) {
            pxe_draw_hline(surf, k_tab_x[t], ty + th - 1, k_tab_w[t], PXE_COL_DARK_GRAY);
        }
        int tw = (int)strlen(k_tab_names[t]) * 6;
        pxe_draw_text_small(surf, k_tab_x[t] + (k_tab_w[t] - tw) / 2, ty + 7,
                            k_tab_names[t], active ? PXE_COL_DARK_BLUE : PXE_COL_BLACK);
    }
    pxe_draw_button(surf, surf->width - 74, 6, 66, 22, "Refresh", 0, 1);

    /* Main frame */
    int fx = 6, fy = 30, fw = surf->width - 12, fh = surf->height - 56;
    pxe_draw_bevel_box(surf, fx, fy, fw, fh, 0, PXE_COL_LIGHT_GRAY);
    if (s->tab == TAB_OVERVIEW) {
        render_overview(surf, s);
    } else {
        render_table(surf, s);
    }

    /* Status line */
    int sy = surf->height - 22;
    pxe_draw_hline(surf, 0, sy, surf->width, PXE_COL_DARK_GRAY);
    char status[112];
    status[0] = '\0';
    uint8_t scol = PXE_COL_BLACK;
    if (s->notice[0]) {
        snprintf(status, sizeof(status), "%s", s->notice);
        scol = s->notice_is_err ? PXE_COL_DARK_RED : PXE_COL_DARK_GREEN;
    } else if (s->tab == TAB_PCI && s->selected[TAB_PCI] >= 0 && s->selected[TAB_PCI] < s->pci_count) {
        const pxe_r0_pci_info_t* p = &s->pci[s->selected[TAB_PCI]];
        snprintf(status, sizeof(status),
                 "PCI Subsys %04X:%04X Rev 0x%02X | Class %02X.%02X.%02X | BAR0 Size: %u KB",
                 (unsigned)p->subsys_vendor, (unsigned)p->subsys_device,
                 (unsigned)p->revision, (unsigned)p->class_code,
                 (unsigned)p->subclass, (unsigned)p->prog_if,
                 (unsigned)(p->bar0_size / 1024u));
    } else if (s->tab == TAB_PDV && s->selected[TAB_PDV] >= 0 && s->selected[TAB_PDV] < s->pdv_count) {
        const pxe_r0_pdv_info_t* d = &s->pdv[s->selected[TAB_PDV]];
        snprintf(status, sizeof(status), "Provider: %s | Base: 0x%llX",
                 d->provider[0] ? d->provider : "Unknown",
                 (unsigned long long)d->base_virt);
    } else if (s->tab == TAB_PDV) {
        snprintf(status, sizeof(status), "Loaded .pdv drivers: %d (source: PDEVICE\\)", s->pdv_count);
    } else {
        snprintf(status, sizeof(status), "Ring 0 Privileged Diagnostics | Items: %d",
                 s->tab == TAB_OVERVIEW ? 1 : tab_item_count(s, s->tab));
    }
    int maxs = (surf->width - 16) / 6;
    if (maxs > 3 && (int)strlen(status) > maxs) status[maxs] = '\0';
    pxe_draw_text_small(surf, 8, sy + 6, status, scol);

    /* Load .pdv modal */
    if (s->load_modal) {
        int dw = 310, dh = 98;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3, "Load .pdv Driver", PXE_COL_WHITE);
        pxe_draw_text_small(surf, dx + 10, dy + 25, "VFS path (e.g. C:\\PDEVICE\\DRIVER.PDV):", PXE_COL_BLACK);
        pxe_draw_bevel_box(surf, dx + 10, dy + 38, dw - 20, 22, 1, PXE_COL_WHITE);
        int maxc = (dw - 28) / 8;
        int start = (s->load_len > maxc) ? (s->load_len - maxc) : 0;
        pxe_draw_text(surf, dx + 14, dy + 41, s->load_path + start, PXE_COL_BLACK);
        pxe_draw_vline(surf, dx + 14 + (s->load_len - start) * 8, dy + 41, 16, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + 68,  dy + 68, 76, 22, "Load", 0, 1);
        pxe_draw_button(surf, dx + 166, dy + 68, 76, 22, "Cancel", 0, 1);
    }
}

static void submit_load_pdv(sysinfo_app_t* s) {
    s->load_modal = 0;
    if (s->load_path[0] == '\0') {
        s->notice_is_err = 1;
        snprintf(s->notice, sizeof(s->notice), "Empty .pdv path.");
        pxe_beep();
        return;
    }
    int rc = pxe_r0_load_pdv_file(s->load_path);
    refresh_all(s);
    if (rc == 0) {
        s->notice_is_err = 0;
        snprintf(s->notice, sizeof(s->notice), "Loaded driver (%d total active).", s->pdv_count);
    } else if (rc > 0) {
        s->notice_is_err = 0;
        snprintf(s->notice, sizeof(s->notice), "Driver already loaded or skipped (%d active).", s->pdv_count);
    } else {
        s->notice_is_err = 1;
        snprintf(s->notice, sizeof(s->notice), "Load failed (err=%d). Check path / PDV1 / HW match.", rc);
        pxe_beep();
    }
}

static void do_rescan_pdevice(sysinfo_app_t* s) {
    int rc = pxe_r0_rescan_pdevice_dir();
    refresh_all(s);
    if (rc >= 0) {
        s->notice_is_err = 0;
        snprintf(s->notice, sizeof(s->notice), "PDEVICE\\ rescan complete (%d new, %d active).", rc, s->pdv_count);
    } else {
        s->notice_is_err = 1;
        snprintf(s->notice, sizeof(s->notice), "PDEVICE\\ rescan failed (err=%d).", rc);
        pxe_beep();
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    sysinfo_app_t* s = (sysinfo_app_t*)calloc(1, sizeof(sysinfo_app_t));
    if (!s) return 1;
    for (int t = 0; t < TAB_COUNT; t++) s->selected[t] = -1;

    query_cpu_features(s->cpu_features, sizeof(s->cpu_features));
    refresh_all(s);
    char drv = s->info.system_drive ? s->info.system_drive
             : (s->info.cdrom_drive ? s->info.cdrom_drive : 'C');
    snprintf(s->load_path, sizeof(s->load_path), "%c:\\PDEVICE\\", drv);
    s->load_len = (int)strlen(s->load_path);

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 68;
    desc.y = 36;
    desc.w = 500;
    desc.h = 360;
    desc.min_w = 500;
    desc.min_h = 360;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_DEFAULT;
    strcpy(desc.title, "System Information");

    pxe_win_surface_t surf;
    s->win_id = pxe_win_create(&desc, &surf);
    if (s->win_id < 0) {
        free(s);
        return 1;
    }

    int running = 1;
    int dirty = 1;

    while (running) {
        pxe_event_t ev;
        int rc;
        while ((rc = pxe_win_poll_event(s->win_id, &ev)) > 0) {
            switch (ev.type) {
                case PXE_EV_CLOSE:
                    running = 0;
                    break;
                case PXE_EV_REDRAW:
                case PXE_EV_ACTIVATE:
                    dirty = 1;
                    break;
                case PXE_EV_MOUSE_UP:
                    s->sb_dragging = 0;
                    break;
                case PXE_EV_MOUSE_MOVE:
                    if (s->sb_dragging && (ev.buttons & PXE_BTN_LEFT) && s->tab != TAB_OVERVIEW) {
                        int lx, ly, lw, lh, vis;
                        get_list_geom(&surf, s->tab, &lx, &ly, &lw, &lh, &vis);
                        int count = tab_item_count(s, s->tab);
                        int max_scr = count - vis;
                        if (max_scr < 0) max_scr = 0;
                        int track = lh - 28;
                        if (track < 1) track = 1;
                        int delta = ((ev.y - s->sb_drag_start_y) * max_scr) / track;
                        int nscr = s->sb_drag_start_off + delta;
                        if (nscr < 0) nscr = 0;
                        if (nscr > max_scr) nscr = max_scr;
                        if (nscr != s->scroll[s->tab]) {
                            s->scroll[s->tab] = nscr;
                            dirty = 1;
                        }
                    }
                    break;
                case PXE_EV_MOUSE_DOWN: {
                    if (!(ev.buttons & PXE_BTN_LEFT)) break;
                    int mx = ev.x, my = ev.y;

                    if (s->load_modal) {
                        int dw = 310, dh = 98;
                        int dx = (surf.width - dw) / 2;
                        int dy = (surf.height - dh) / 2;
                        if (pxe_point_in_rect(mx, my, dx + 68, dy + 68, 76, 22)) {
                            submit_load_pdv(s);
                        } else if (pxe_point_in_rect(mx, my, dx + 166, dy + 68, 76, 22)) {
                            s->load_modal = 0;
                        }
                        dirty = 1;
                        break;
                    }

                    /* Refresh button */
                    if (pxe_point_in_rect(mx, my, surf.width - 74, 6, 66, 22)) {
                        s->notice[0] = '\0';
                        refresh_all(s);
                        dirty = 1;
                        break;
                    }

                    /* Tabs */
                    int tab_hit = 0;
                    for (int t = 0; t < TAB_COUNT; t++) {
                        if (pxe_point_in_rect(mx, my, k_tab_x[t], 6, k_tab_w[t], 25)) {
                            s->tab = t;
                            s->notice[0] = '\0';
                            tab_hit = 1;
                            dirty = 1;
                            break;
                        }
                    }
                    if (tab_hit || s->tab == TAB_OVERVIEW) break;

                    /* PDV action buttons */
                    int lx, ly, lw, lh, vis;
                    get_list_geom(&surf, s->tab, &lx, &ly, &lw, &lh, &vis);
                    int count = tab_item_count(s, s->tab);
                    int max_scr = count - vis;
                    if (max_scr < 0) max_scr = 0;

                    if (s->tab == TAB_PDV) {
                        int by = ly + lh + 6;
                        if (pxe_point_in_rect(mx, my, 10, by, 134, 22)) {
                            do_rescan_pdevice(s);
                            dirty = 1;
                            break;
                        }
                        if (pxe_point_in_rect(mx, my, 152, by, 104, 22)) {
                            s->load_modal = 1;
                            dirty = 1;
                            break;
                        }
                    }

                    /* Scrollbar */
                    int sb_x = lx + lw;
                    if (pxe_point_in_rect(mx, my, sb_x, ly, SB_W, lh)) {
                        if (my < ly + 14) {
                            if (s->scroll[s->tab] > 0) s->scroll[s->tab]--;
                        } else if (my >= ly + lh - 14) {
                            if (s->scroll[s->tab] < max_scr) s->scroll[s->tab]++;
                        } else {
                            s->sb_dragging = 1;
                            s->sb_drag_start_y = my;
                            s->sb_drag_start_off = s->scroll[s->tab];
                        }
                        dirty = 1;
                        break;
                    }

                    /* Table row select */
                    if (pxe_point_in_rect(mx, my, lx, ly, lw, lh)) {
                        int row = (my - ly - 2) / ROW_H;
                        int idx = s->scroll[s->tab] + row;
                        if (row >= 0 && idx >= 0 && idx < count) {
                            s->selected[s->tab] = idx;
                            s->notice[0] = '\0';
                        } else {
                            s->selected[s->tab] = -1;
                        }
                        dirty = 1;
                    }
                    break;
                }
                case PXE_EV_KEY_DOWN: {
                    if (s->load_modal) {
                        if (ev.key == PXE_KEY_ESC) {
                            s->load_modal = 0;
                        } else if (ev.key == PXE_KEY_ENTER) {
                            submit_load_pdv(s);
                        } else if (ev.key == PXE_KEY_BACKSPACE) {
                            if (s->load_len > 0) s->load_path[--s->load_len] = '\0';
                        } else if (ev.key >= 32 && ev.key <= 126 && s->load_len < 120) {
                            s->load_path[s->load_len++] = (char)ev.key;
                            s->load_path[s->load_len] = '\0';
                        }
                        dirty = 1;
                        break;
                    }
                    if (s->tab == TAB_OVERVIEW) break;
                    int lx, ly, lw, lh, vis;
                    get_list_geom(&surf, s->tab, &lx, &ly, &lw, &lh, &vis);
                    (void)lx; (void)ly; (void)lw; (void)lh;
                    int count = tab_item_count(s, s->tab);
                    int max_scr = count - vis;
                    if (max_scr < 0) max_scr = 0;

                    if (ev.key == PXE_KEY_UP && count > 0) {
                        if (s->selected[s->tab] <= 0) s->selected[s->tab] = 0;
                        else s->selected[s->tab]--;
                        if (s->selected[s->tab] < s->scroll[s->tab]) {
                            s->scroll[s->tab] = s->selected[s->tab];
                        }
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_DOWN && count > 0) {
                        if (s->selected[s->tab] < 0) s->selected[s->tab] = 0;
                        else if (s->selected[s->tab] < count - 1) s->selected[s->tab]++;
                        if (s->selected[s->tab] >= s->scroll[s->tab] + vis) {
                            s->scroll[s->tab] = s->selected[s->tab] - vis + 1;
                        }
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_PGUP && count > 0) {
                        s->scroll[s->tab] -= vis;
                        if (s->scroll[s->tab] < 0) s->scroll[s->tab] = 0;
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_PGDN && count > 0) {
                        s->scroll[s->tab] += vis;
                        if (s->scroll[s->tab] > max_scr) s->scroll[s->tab] = max_scr;
                        dirty = 1;
                    }
                    break;
                }
                default:
                    break;
            }
        }
        if (rc < 0) break;

        if (dirty && running) {
            render_sysinfo(&surf, s);
            pxe_win_present(s->win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(30);
        }
    }

    pxe_win_destroy(s->win_id);
    free(s);
    return 0;
}
