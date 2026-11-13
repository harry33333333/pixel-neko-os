#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/fs.h>
#include <pxe/mem.h>
#include <pxe/str.h>
#include <pxe/ring0.h>

#include "../../boot/limine/limine-bios-hdd.h"

#define PART_FS_UNKNOWN 0
#define PART_FS_FAT12   1
#define PART_FS_FAT16   2
#define PART_FS_FAT32   3

#define BLK_TYPE_FLOPPY 1
#define BLK_TYPE_ATAPI  3
#define BLK_TYPE_MEMORY 6

#define MAX_RAW_PARTS   32
#define MAX_DIRS        96
#define MAX_FILES       384
#define MAX_LOG_LINES   48

#define STATE_IDLE       0
#define STATE_INSTALLING 1
#define STATE_DONE       2
#define STATE_ERROR      3

#define ROW_H 21
#define SB_W  14

typedef enum {
    STEP_WELCOME = 0,
    STEP_SELECT,
    STEP_REINSTALL_WARN,
    STEP_FORMAT,
    STEP_CONFIRM,
    STEP_PROGRESS,
    STEP_COMPLETE
} install_step_t;

typedef struct {
    char rel[160];
} rel_path_t;

typedef struct {
    char     rel[160];
    uint64_t size;
} copy_item_t;

typedef struct {
    int win_id;
    pxe_win_surface_t surf;

    pxe_basic_sysinfo_t sysinfo;
    pxe_r0_part_info_t  parts[MAX_RAW_PARTS];
    int part_count;
    int sel_part;
    int part_scroll;

    int  fs_choice; /* 3=FAT32, 2=FAT16, 0=Skip Format */
    char label[12];
    int  label_len;
    int  label_focus;

    int  state;
    int  progress;
    char stage[64];
    char status_detail[80];

    char logs[MAX_LOG_LINES][84];
    int  log_count;

    install_step_t step;
    int install_mode;
    int skip_format;
    int reinstall_ack;
    int install_error;
    uint8_t existing_on_disk[MAX_RAW_PARTS];
    uint8_t existing_on_target[MAX_RAW_PARTS];
    char existing_letter[MAX_RAW_PARTS];
} installer_t;

static uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t* p) {
    uint64_t lo = (uint64_t)get_le32(p);
    uint64_t hi = (uint64_t)get_le32(p + 4);
    return lo | (hi << 32);
}

static void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_le64(uint8_t* p, uint64_t v) {
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFu));
    put_le32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

static uint32_t crc32_ieee(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static int ends_with_ci(const char* s, const char* suf) {
    size_t sl = strlen(s);
    size_t tl = strlen(suf);
    if (sl < tl) return 0;
    return strcasecmp(s + sl - tl, suf) == 0;
}

static void add_log(installer_t* inst, const char* msg) {
    if (inst->log_count >= MAX_LOG_LINES) {
        for (int i = 1; i < MAX_LOG_LINES; i++) {
            memcpy(inst->logs[i - 1], inst->logs[i], sizeof(inst->logs[0]));
        }
        inst->log_count = MAX_LOG_LINES - 1;
    }
    snprintf(inst->logs[inst->log_count], sizeof(inst->logs[0]), "%s", msg);
    inst->log_count++;
    pxe_debug_log(msg);
}

static void fmt_sectors(uint64_t sectors, char* out, size_t cap) {
    uint64_t mb = (sectors * 512ull) / (1024ull * 1024ull);
    if (mb >= 1024ull) {
        snprintf(out, cap, "%u.%u GB",
                 (unsigned)(mb / 1024ull),
                 (unsigned)(((mb % 1024ull) * 10ull) / 1024ull));
    } else {
        snprintf(out, cap, "%u MB", (unsigned)mb);
    }
}

#define RGB_COL(hex24) (0xFF000000u | (uint32_t)(hex24))

typedef struct {
    int cw, ch;
    int card_x, card_y, card_w, card_h;
    int hdr_h, step_h;
    int body_x, body_y, body_w, body_h;
    int foot_y, foot_h;
    int back_x, back_y, back_w, back_h;
    int next_x, next_y, next_w, next_h;
    int exit_x, exit_y, exit_w, exit_h;
    int shut_x, shut_y, shut_w, shut_h;
    int wbtn_x, wbtn_y, wbtn_w, wbtn_h;
    int list_x, list_y, list_w, list_h, vis_rows;
    int ref_x, ref_y, ref_w, ref_h;
    int init_x, init_y, init_w, init_h;
    int fcard_x, fcard_w, fcard_h, fcard_y0, fcard_y1, fcard_y2;
    int lbl_x, lbl_y, lbl_w, lbl_h;
} nt6_layout_t;

static int part_supports_fat32(const pxe_r0_part_info_t* p) {
    return p && p->total_sectors >= 65536ull;
}

static int part_supports_fat16(const pxe_r0_part_info_t* p) {
    return p && p->total_sectors <= 4194304ull;
}

static int part_supports_skip_fmt(const pxe_r0_part_info_t* p) {
    return p && (p->fs_type == PART_FS_FAT16 || p->fs_type == PART_FS_FAT32);
}

static void calc_nt6_layout(const pxe_win_surface_t* surf, nt6_layout_t* l) {
    memset(l, 0, sizeof(*l));
    l->cw = (surf && surf->width > 0) ? surf->width : 798;
    l->ch = (surf && surf->height > 0) ? surf->height : 560;

    l->card_w = (l->cw >= 730) ? 700 : (l->cw - 20);
    if (l->card_w < 580) l->card_w = l->cw - 8;
    l->card_h = (l->ch >= 520) ? 486 : (l->ch - 26);
    if (l->card_h < 400) l->card_h = l->ch - 8;
    l->card_x = (l->cw - l->card_w) / 2;
    l->card_y = (l->ch >= 520) ? ((l->ch - l->card_h) / 2 + 8) : ((l->ch - l->card_h) / 2);

    l->hdr_h  = 48;
    l->step_h = 26;
    l->foot_h = 44;
    l->foot_y = l->card_y + l->card_h - l->foot_h;

    l->body_x = l->card_x + 20;
    l->body_y = l->card_y + l->hdr_h + l->step_h + 10;
    l->body_w = l->card_w - 40;
    l->body_h = l->foot_y - l->body_y - 8;

    int by = l->foot_y + 9;
    l->back_w = 94;  l->back_h = 26;
    l->back_x = l->card_x + 16; l->back_y = by;

    l->exit_w = 82;  l->exit_h = 26;
    l->exit_x = l->back_x + l->back_w + 10; l->exit_y = by;

    l->next_w = 136; l->next_h = 26;
    l->next_x = l->card_x + l->card_w - l->next_w - 16; l->next_y = by;

    l->shut_w = 104; l->shut_h = 26;
    l->shut_x = l->next_x - l->shut_w - 10; l->shut_y = by;

    /* Welcome "Install now" CTA inside right pane */
    l->wbtn_w = 176; l->wbtn_h = 34;
    l->wbtn_x = l->body_x + 196;
    l->wbtn_y = l->body_y + l->body_h - 54;

    /* Partition table & refresh/init controls */
    l->ref_w  = 88;  l->ref_h  = 22;
    l->ref_x  = l->body_x + l->body_w - l->ref_w;
    l->ref_y  = l->body_y + 2;
    l->init_w = 102; l->init_h = 22;
    l->init_x = l->ref_x - l->init_w - 8;
    l->init_y = l->ref_y;

    l->list_x = l->body_x;
    l->list_y = l->body_y + 66;
    l->list_w = l->body_w - SB_W;
    l->list_h = (l->body_h >= 340) ? 164 : 132;
    l->vis_rows = (l->list_h - 4) / ROW_H;
    if (l->vis_rows < 1) l->vis_rows = 1;

    /* Format radio cards & volume label box */
    l->fcard_x  = l->body_x;
    l->fcard_w  = l->body_w;
    l->fcard_h  = (l->body_h >= 340) ? 58 : 50;
    l->fcard_y0 = l->body_y + 42;
    l->fcard_y1 = l->fcard_y0 + l->fcard_h + 8;
    l->fcard_y2 = l->fcard_y1 + l->fcard_h + 8;
    l->lbl_x    = l->body_x + 124;
    l->lbl_y    = l->fcard_y2 + l->fcard_h + 12;
    l->lbl_w    = 176;
    l->lbl_h    = 24;
}

static void sync_fs_choice_to_selection(installer_t* inst) {
    if (inst->sel_part < 0 || inst->sel_part >= inst->part_count) return;
    const pxe_r0_part_info_t* p = &inst->parts[inst->sel_part];
    if (inst->skip_format) {
        if (part_supports_skip_fmt(p)) {
            inst->fs_choice = 0;
            return;
        }
        inst->skip_format = 0;
    }
    if (inst->fs_choice == 0) {
        inst->fs_choice = part_supports_fat32(p) ? PART_FS_FAT32 : PART_FS_FAT16;
    } else if (inst->fs_choice == PART_FS_FAT32 && !part_supports_fat32(p)) {
        inst->fs_choice = PART_FS_FAT16;
    } else if (inst->fs_choice == PART_FS_FAT16 && !part_supports_fat16(p)) {
        inst->fs_choice = PART_FS_FAT32;
    }
}

static void detect_existing_os(installer_t* inst) {
    for (int i = 0; i < MAX_RAW_PARTS; i++) {
        inst->existing_on_disk[i] = 0;
        inst->existing_on_target[i] = 0;
        inst->existing_letter[i] = 0;
    }
    for (int i = 0; i < inst->part_count; i++) {
        char letter = inst->parts[i].mounted_letter;
        if (!letter) continue;
        char kpath[48], ppath[48];
        pxe_dirent_t st;
        snprintf(kpath, sizeof(kpath), "%c:\\boot\\kernel.elf", letter);
        snprintf(ppath, sizeof(ppath), "%c:\\system\\PAYLOAD.TXT", letter);
        if (pxe_stat(kpath, &st) == 0 && !st.is_dir && st.size > 0 &&
            pxe_stat(ppath, &st) == 0 && !st.is_dir && st.size > 0) {
            inst->existing_on_target[i] = 1;
            inst->existing_letter[i] = letter;
            for (int j = 0; j < inst->part_count; j++) {
                if (inst->parts[j].disk_index == inst->parts[i].disk_index) {
                    inst->existing_on_disk[j] = 1;
                    if (!inst->existing_letter[j]) inst->existing_letter[j] = letter;
                }
            }
        }
    }
}

static void refresh_partitions(installer_t* inst) {
    pxe_sys_get_basic_info(&inst->sysinfo);

    pxe_r0_part_info_t raw[MAX_RAW_PARTS];
    memset(raw, 0, sizeof(raw));
    int n = pxe_r0_enum_disks_and_parts(raw, MAX_RAW_PARTS);
    inst->part_count = 0;

    for (int i = 0; i < n && inst->part_count < MAX_RAW_PARTS; i++) {
        const pxe_r0_part_info_t* r = &raw[i];
        if (r->disk_type == BLK_TYPE_FLOPPY ||
            r->disk_type == BLK_TYPE_ATAPI ||
            r->disk_type == BLK_TYPE_MEMORY) {
            continue;
        }
        if (r->is_removable || r->is_efi_system) continue;
        if (r->total_sectors < 16384ull) continue;
        inst->parts[inst->part_count++] = *r;
    }

    if (inst->part_count == 0) {
        inst->sel_part = -1;
        inst->part_scroll = 0;
    } else {
        if (inst->sel_part < 0 || inst->sel_part >= inst->part_count) {
            inst->sel_part = 0;
        }
    }
    detect_existing_os(inst);
    sync_fs_choice_to_selection(inst);
}

static void draw_nt6_btn(const pxe_win_surface_t* surf, int x, int y, int w, int h,
                         const char* label, int style, int enabled) {
    /* style: 0=silver neutral, 1=NT6 blue primary, 2=NT6 green confirm */
    uint32_t bg, border, hi, fg;
    if (!enabled) {
        bg = RGB_COL(0xDFE4EA);
        border = RGB_COL(0x9AA5B1);
        hi = RGB_COL(0xEDF1F5);
        fg = RGB_COL(0x7B8794);
    } else if (style == 1) {
        bg = RGB_COL(0x0B5ED7);
        border = RGB_COL(0x083B8A);
        hi = RGB_COL(0x4B8DF0);
        fg = RGB_COL(0xFFFFFF);
    } else if (style == 2) {
        bg = RGB_COL(0x168A3E);
        border = RGB_COL(0x0E5C29);
        hi = RGB_COL(0x47B86C);
        fg = RGB_COL(0xFFFFFF);
    } else {
        bg = RGB_COL(0xE8EEF4);
        border = RGB_COL(0x7D8C9C);
        hi = RGB_COL(0xFFFFFF);
        fg = RGB_COL(0x102A43);
    }
    pxe_fill_rect(surf, x, y, w, h, bg);
    if (h > 6) {
        pxe_fill_rect(surf, x + 1, y + 1, w - 2, 2, hi);
    }
    pxe_draw_rect(surf, x, y, w, h, border);
    if (label && label[0]) {
        int tw = (int)strlen(label) * 8;
        if (tw <= w - 8) {
            pxe_draw_text(surf, x + (w - tw) / 2, y + (h - 16) / 2, label, fg);
        } else {
            int sw = (int)strlen(label) * 6;
            pxe_draw_text_small(surf, x + (w - sw) / 2, y + (h - 8) / 2, label, fg);
        }
    }
}

static void draw_nt6_backdrop(const pxe_win_surface_t* surf, const installer_t* inst) {
    int bands = 16;
    int band_h = (surf->height + bands - 1) / bands;
    for (int i = 0; i < bands; i++) {
        uint32_t r = (uint32_t)(8 + (i < 8 ? i * 2 : (15 - i) * 2));
        uint32_t g = (uint32_t)(48 + (i < 8 ? i * 7 : (15 - i) * 5));
        uint32_t b = (uint32_t)(78 + (i < 8 ? i * 8 : (15 - i) * 6));
        int yy = i * band_h;
        pxe_fill_rect(surf, 0, yy, surf->width, band_h, RGB_COL((r << 16) | (g << 8) | b));
    }
    /* Subtle NT6 diagonal aurora accent strips */
    pxe_fill_rect(surf, 0, 0, surf->width, 4, RGB_COL(0x38BDF8));
    char brand[96];
    snprintf(brand, sizeof(brand), "Pixel Neko OS Setup  |  Build %s",
             inst->sysinfo.os_build[0] ? inst->sysinfo.os_build : "26A221a");
    pxe_draw_text_small(surf, 18, 12, brand, RGB_COL(0xE0F2FE));
    if (inst->install_mode) {
        const char* hint = "Use Apple Menu to Restart / Shut Down";
        int hw = (int)strlen(hint) * 6;
        if (surf->width - hw - 18 > 280) {
            pxe_draw_text_small(surf, surf->width - hw - 18, 12, hint, RGB_COL(0xBAE6FD));
        }
    }
}

static void draw_neko_badge(const pxe_win_surface_t* surf, int x, int y) {
    pxe_fill_rect(surf, x,      y,      14, 14, RGB_COL(0xDC2626));
    pxe_fill_rect(surf, x + 14, y,      14, 14, RGB_COL(0x16A34A));
    pxe_fill_rect(surf, x,      y + 14, 14, 14, RGB_COL(0x2563EB));
    pxe_fill_rect(surf, x + 14, y + 14, 14, 14, RGB_COL(0xEAB308));
    pxe_draw_rect(surf, x, y, 28, 28, RGB_COL(0x0F172A));
    pxe_fill_rect(surf, x + 9, y + 9, 10, 10, RGB_COL(0xFFFFFF));
    pxe_put_pixel(surf, x + 10, y + 8, RGB_COL(0xFFFFFF));
    pxe_put_pixel(surf, x + 17, y + 8, RGB_COL(0xFFFFFF));
    pxe_put_pixel(surf, x + 12, y + 12, RGB_COL(0x0F172A));
    pxe_put_pixel(surf, x + 15, y + 12, RGB_COL(0x0F172A));
    pxe_put_pixel(surf, x + 13, y + 15, RGB_COL(0x0F172A));
}

static void render_step_welcome(const pxe_win_surface_t* surf, const installer_t* inst,
                                const nt6_layout_t* l) {
    int bx = l->body_x, by = l->body_y, bw = l->body_w, bh = l->body_h;
    int lw = 174;
    pxe_fill_rect(surf, bx, by + 4, lw, bh - 12, RGB_COL(0x0D4A6E));
    pxe_draw_rect(surf, bx, by + 4, lw, bh - 12, RGB_COL(0x082F49));
    draw_neko_badge(surf, bx + (lw - 28) / 2, by + 48);
    pxe_draw_text(surf, bx + 20, by + 92, "Pixel Neko OS", RGB_COL(0xFFFFFF));
    pxe_draw_text_small(surf, bx + 26, by + 116, "Snow Neko Edition", RGB_COL(0x7DD3FC));
    pxe_draw_hline(surf, bx + 18, by + 140, lw - 36, RGB_COL(0x38BDF8));
    char bstr[40];
    snprintf(bstr, sizeof(bstr), "Build %s",
             inst->sysinfo.os_build[0] ? inst->sysinfo.os_build : "26A221a");
    pxe_draw_text_small(surf, bx + 26, by + 156, bstr, RGB_COL(0xE0F2FE));
    char mstr[40];
    snprintf(mstr, sizeof(mstr), "Boot CD: %c:\\",
             inst->sysinfo.cdrom_drive ? inst->sysinfo.cdrom_drive : '-');
    pxe_draw_text_small(surf, bx + 26, by + 176, mstr, RGB_COL(0xBAE6FD));

    int rx = bx + lw + 22;
    int rw = bw - lw - 22;
    pxe_draw_text(surf, rx, by + 8, "Welcome to Pixel Neko OS Setup", RGB_COL(0x0C4A6E));
    pxe_draw_hline(surf, rx, by + 30, rw, RGB_COL(0xCBD5E1));
    pxe_draw_text_small(surf, rx, by + 44,
                        "This guided wizard installs Pixel Neko OS onto an existing",
                        RGB_COL(0x334155));
    pxe_draw_text_small(surf, rx, by + 60,
                        "fixed-disk partition cleanly step by step.",
                        RGB_COL(0x334155));

    pxe_fill_rect(surf, rx, by + 84, rw, 108, RGB_COL(0xF8FAFC));
    pxe_draw_rect(surf, rx, by + 84, rw, 108, RGB_COL(0xCBD5E1));
    pxe_draw_text_small(surf, rx + 12, by + 94,  "Setup Steps Overview:", RGB_COL(0x0F172A));
    pxe_draw_text_small(surf, rx + 12, by + 114, "1. Select destination disk & partition", RGB_COL(0x334155));
    pxe_draw_text_small(surf, rx + 12, by + 132, "2. Check existing OS & select filesystem (FAT32/FAT16)", RGB_COL(0x334155));
    pxe_draw_text_small(surf, rx + 12, by + 150, "3. Review configuration & install bootloader", RGB_COL(0x334155));
    pxe_draw_text_small(surf, rx + 12, by + 168, "4. Reboot into standalone Pixel Neko OS", RGB_COL(0x334155));

    char sysline[96];
    snprintf(sysline, sizeof(sysline), "Hardware: %u MB RAM | %dx%d | %s",
             inst->sysinfo.total_ram_mb, inst->sysinfo.screen_w, inst->sysinfo.screen_h,
             inst->install_mode ? "Installer Mode" : "Live Session");
    int maxc = rw / 6;
    if (maxc > 4 && (int)strlen(sysline) > maxc) sysline[maxc] = '\0';
    pxe_draw_text_small(surf, rx, by + 206, sysline, RGB_COL(0x475569));

    draw_nt6_btn(surf, l->wbtn_x, l->wbtn_y, l->wbtn_w, l->wbtn_h, "Install now >", 2, 1);
}

static void render_step_select(const pxe_win_surface_t* surf, const installer_t* inst,
                               const nt6_layout_t* l) {
    int bx = l->body_x, by = l->body_y;
    pxe_draw_text(surf, bx, by, "Where do you want to install Pixel Neko OS?", RGB_COL(0x0C4A6E));
    pxe_draw_text_small(surf, bx, by + 24,
                        "Select an existing disk partition. Partition geometry is preserved.",
                        RGB_COL(0x475569));

    draw_nt6_btn(surf, l->init_x, l->init_y, l->init_w, l->init_h, "Init Disk", 0, 1);
    draw_nt6_btn(surf, l->ref_x,  l->ref_y,  l->ref_w,  l->ref_h,  "Refresh",   0, 1);

    /* Table Column Header Bar */
    int hdr_y = l->list_y - 20;
    pxe_fill_rect(surf, l->list_x, hdr_y, l->list_w + SB_W, 20, RGB_COL(0xE2E8F0));
    pxe_draw_rect(surf, l->list_x, hdr_y, l->list_w + SB_W, 20, RGB_COL(0x94A3B8));
    pxe_draw_text_small(surf, l->list_x + 8, hdr_y + 6,
                        "Part   Device       Size       FS      Drv  OS Status",
                        RGB_COL(0x0F172A));

    pxe_fill_rect(surf, l->list_x, l->list_y, l->list_w, l->list_h, RGB_COL(0xFFFFFF));
    pxe_draw_rect(surf, l->list_x, l->list_y, l->list_w, l->list_h, RGB_COL(0x94A3B8));

    if (inst->part_count == 0) {
        pxe_draw_text_small(surf, l->list_x + 14, l->list_y + 28,
                            "No writable fixed-disk partition found.", RGB_COL(0xB91C1C));
        pxe_draw_text_small(surf, l->list_x + 14, l->list_y + 48,
                            "If the VM disk is blank, click [Init Disk] and then [Refresh].",
                            RGB_COL(0x475569));
    } else {
        for (int i = 0; i < l->vis_rows; i++) {
            int idx = inst->part_scroll + i;
            if (idx >= inst->part_count) break;
            const pxe_r0_part_info_t* p = &inst->parts[idx];
            int ry = l->list_y + 2 + i * ROW_H;
            int sel = (idx == inst->sel_part);
            uint32_t rbg = sel ? RGB_COL(0x0B5ED7)
                               : ((i & 1) ? RGB_COL(0xF8FAFC) : RGB_COL(0xFFFFFF));
            uint32_t rfg = sel ? RGB_COL(0xFFFFFF) : RGB_COL(0x0F172A);
            pxe_fill_rect(surf, l->list_x + 1, ry, l->list_w - 2, ROW_H, rbg);

            char sz[16], drv[6], os_s[22], line[112];
            fmt_sectors(p->total_sectors, sz, sizeof(sz));
            if (p->mounted_letter) snprintf(drv, sizeof(drv), "%c:", p->mounted_letter);
            else snprintf(drv, sizeof(drv), "--");
            if (inst->existing_on_target[idx]) snprintf(os_s, sizeof(os_s), "Existing OS");
            else if (inst->existing_on_disk[idx]) snprintf(os_s, sizeof(os_s), "OS on Disk");
            else snprintf(os_s, sizeof(os_s), "Clean/Other");

            snprintf(line, sizeof(line),
                     "D%dP%-2d  %-11.11s  %-9.9s  %-6.6s  %-3.3s  %s",
                     p->disk_index, p->part_index,
                     p->disk_name[0] ? p->disk_name : "Disk",
                     sz, p->fs_name[0] ? p->fs_name : "RAW",
                     drv, os_s);
            int maxc = (l->list_w - 14) / 6;
            if (maxc > 3 && (int)strlen(line) > maxc) line[maxc] = '\0';
            pxe_draw_text_small(surf, l->list_x + 8, ry + 6, line, rfg);
        }
    }
    pxe_draw_scrollbar(surf, l->list_x + l->list_w, l->list_y, SB_W, l->list_h,
                       inst->part_scroll, l->vis_rows, inst->part_count);

    int info_y = l->list_y + l->list_h + 10;
    int info_h = (l->body_y + l->body_h) - info_y - 4;
    if (info_h > 44) {
        pxe_fill_rect(surf, l->body_x, info_y, l->body_w, info_h, RGB_COL(0xF1F5F9));
        pxe_draw_rect(surf, l->body_x, info_y, l->body_w, info_h, RGB_COL(0xCBD5E1));
        if (inst->sel_part >= 0 && inst->sel_part < inst->part_count) {
            const pxe_r0_part_info_t* p = &inst->parts[inst->sel_part];
            char d1[96], d2[96];
            snprintf(d1, sizeof(d1),
                     "Selected: Disk %d Partition %d (%s) | Start LBA: %llu | Sectors: %llu",
                     p->disk_index, p->part_index, p->disk_name[0] ? p->disk_name : "HDD",
                     (unsigned long long)p->start_lba, (unsigned long long)p->total_sectors);
            if (inst->existing_on_disk[inst->sel_part]) {
                snprintf(d2, sizeof(d2),
                         "Notice: Existing Pixel Neko OS found on %c:\\ (Next shows warning).",
                         inst->existing_letter[inst->sel_part] ? inst->existing_letter[inst->sel_part] : '?');
            } else {
                snprintf(d2, sizeof(d2),
                         "All other partitions on this disk remain untouched.");
            }
            pxe_draw_text_small(surf, l->body_x + 10, info_y + 10, d1, RGB_COL(0x0F172A));
            pxe_draw_text_small(surf, l->body_x + 10, info_y + 28, d2,
                                inst->existing_on_disk[inst->sel_part]
                                    ? RGB_COL(0xB45309) : RGB_COL(0x475569));
        } else {
            pxe_draw_text_small(surf, l->body_x + 10, info_y + 18,
                                "Select a partition from the list above to continue.",
                                RGB_COL(0x64748B));
        }
    }
}

static void render_step_reinstall_warn(const pxe_win_surface_t* surf, const installer_t* inst,
                                       const nt6_layout_t* l) {
    int bx = l->body_x, by = l->body_y, bw = l->body_w;
    pxe_fill_rect(surf, bx, by + 6, bw, 72, RGB_COL(0xFEF3C7));
    pxe_draw_rect(surf, bx, by + 6, bw, 72, RGB_COL(0xD97706));
    pxe_fill_rect(surf, bx + 16, by + 20, 28, 28, RGB_COL(0xD97706));
    pxe_draw_text(surf, bx + 26, by + 26, "!", RGB_COL(0xFFFFFF));
    pxe_draw_text(surf, bx + 56, by + 18, "Existing Pixel Neko OS Detected", RGB_COL(0x92400E));
    if (inst->sel_part >= 0 && inst->sel_part < inst->part_count) {
        char sub[96];
        char drv = inst->existing_letter[inst->sel_part]
                       ? inst->existing_letter[inst->sel_part] : '?';
        if (inst->existing_on_target[inst->sel_part]) {
            snprintf(sub, sizeof(sub),
                     "Target Disk %d Partition %d already contains Pixel Neko OS (%c:\\).",
                     inst->parts[inst->sel_part].disk_index,
                     inst->parts[inst->sel_part].part_index,
                     drv);
        } else {
            snprintf(sub, sizeof(sub),
                     "Another partition on Disk %d already contains Pixel Neko OS (%c:\\).",
                     inst->parts[inst->sel_part].disk_index,
                     drv);
        }
        pxe_draw_text_small(surf, bx + 56, by + 44, sub, RGB_COL(0x78350F));
    }

    pxe_fill_rect(surf, bx, by + 94, bw, 152, RGB_COL(0xFFFFFF));
    pxe_draw_rect(surf, bx, by + 94, bw, 152, RGB_COL(0xCBD5E1));
    pxe_draw_text_small(surf, bx + 16, by + 108, "Before continuing, please note:", RGB_COL(0x0F172A));
    pxe_draw_text_small(surf, bx + 20, by + 132,
                        "- Reinstalling will overwrite system binaries, APPS\\*.pxe, and PDEVICE\\*.pdv.",
                        RGB_COL(0x334155));
    pxe_draw_text_small(surf, bx + 20, by + 154,
                        "- Disk MBR/Limine Stage 2 bootloader will be updated on this disk.",
                        RGB_COL(0x334155));
    pxe_draw_text_small(surf, bx + 20, by + 176,
                        "- Existing saved settings (\\system\\PIXEL.pse) may be reset or lost.",
                        RGB_COL(0x334155));
    pxe_draw_text_small(surf, bx + 20, by + 198,
                        "- Choosing Format on the next step erases ALL files on the selected partition.",
                        RGB_COL(0xB91C1C));

    pxe_draw_text_small(surf, bx, by + 262,
                        "Click [< Back] to pick a different partition, or [Continue >] to proceed.",
                        RGB_COL(0x475569));
}

static void draw_format_card(const pxe_win_surface_t* surf, int x, int y, int w, int h,
                             const char* title, const char* desc, int selected, int enabled) {
    uint32_t bg = !enabled ? RGB_COL(0xF1F5F9)
                           : (selected ? RGB_COL(0xEBF4FF) : RGB_COL(0xFFFFFF));
    uint32_t brd = !enabled ? RGB_COL(0xCBD5E1)
                            : (selected ? RGB_COL(0x0B5ED7) : RGB_COL(0x94A3B8));
    pxe_fill_rect(surf, x, y, w, h, bg);
    pxe_draw_rect(surf, x, y, w, h, brd);
    if (selected && enabled) {
        pxe_draw_rect(surf, x + 1, y + 1, w - 2, h - 2, brd);
    }
    /* Radio circle */
    int rx = x + 14, ry = y + (h - 14) / 2;
    pxe_draw_rect(surf, rx, ry, 14, 14, enabled ? RGB_COL(0x0B5ED7) : RGB_COL(0x94A3B8));
    if (selected && enabled) {
        pxe_fill_rect(surf, rx + 3, ry + 3, 8, 8, RGB_COL(0x0B5ED7));
    }
    pxe_draw_text(surf, x + 40, y + 10, title,
                  enabled ? RGB_COL(0x0F172A) : RGB_COL(0x94A3B8));
    pxe_draw_text_small(surf, x + 40, y + 32, desc,
                        enabled ? (selected ? RGB_COL(0x1E40AF) : RGB_COL(0x475569))
                                : RGB_COL(0x94A3B8));
}

static void render_step_format(const pxe_win_surface_t* surf, const installer_t* inst,
                               const nt6_layout_t* l) {
    const pxe_r0_part_info_t* p =
        (inst->sel_part >= 0 && inst->sel_part < inst->part_count)
            ? &inst->parts[inst->sel_part] : NULL;
    int en_f32  = part_supports_fat32(p);
    int en_f16  = part_supports_fat16(p);
    int en_skip = part_supports_skip_fmt(p);

    pxe_draw_text(surf, l->body_x, l->body_y,
                  "Select Destination Filesystem & Label", RGB_COL(0x0C4A6E));
    pxe_draw_text_small(surf, l->body_x, l->body_y + 22,
                        "Choose whether to format the selected partition or keep existing data.",
                        RGB_COL(0x475569));

    draw_format_card(surf, l->fcard_x, l->fcard_y0, l->fcard_w, l->fcard_h,
                     "Format as FAT32 (Recommended)",
                     en_f32 ? "Clean FAT32 format. Removes all existing files on partition."
                            : "Partition is smaller than minimum 32 MB for FAT32.",
                     (!inst->skip_format && inst->fs_choice == PART_FS_FAT32), en_f32);

    draw_format_card(surf, l->fcard_x, l->fcard_y1, l->fcard_w, l->fcard_h,
                     "Format as FAT16 (Legacy Compatibility)",
                     en_f16 ? "Classic FAT16 format (up to ~2 GB). Removes all files on partition."
                            : "Partition exceeds ~2 GB FAT16 limit.",
                     (!inst->skip_format && inst->fs_choice == PART_FS_FAT16), en_f16);

    draw_format_card(surf, l->fcard_x, l->fcard_y2, l->fcard_w, l->fcard_h,
                     "Keep Existing Filesystem (Direct Release / Upgrade)",
                     en_skip ? "Keeps existing FAT16/FAT32 volume and writes OS files directly."
                             : "Requires an existing formatted FAT16 or FAT32 partition.",
                     (inst->skip_format || inst->fs_choice == 0), en_skip);

    pxe_draw_text_small(surf, l->body_x, l->lbl_y + 8, "Volume Label:", RGB_COL(0x0F172A));
    pxe_fill_rect(surf, l->lbl_x, l->lbl_y, l->lbl_w, l->lbl_h, RGB_COL(0xFFFFFF));
    pxe_draw_rect(surf, l->lbl_x, l->lbl_y, l->lbl_w, l->lbl_h,
                  inst->label_focus ? RGB_COL(0x0B5ED7) : RGB_COL(0x94A3B8));
    pxe_draw_text(surf, l->lbl_x + 8, l->lbl_y + 4, inst->label, RGB_COL(0x0F172A));
    if (inst->label_focus) {
        pxe_draw_vline(surf, l->lbl_x + 8 + inst->label_len * 8, l->lbl_y + 4, 16, RGB_COL(0x0B5ED7));
    }
    pxe_draw_text_small(surf, l->lbl_x + l->lbl_w + 12, l->lbl_y + 8,
                        "(Click box to edit, up to 11 chars)", RGB_COL(0x64748B));
}

static void render_step_confirm(const pxe_win_surface_t* surf, const installer_t* inst,
                                const nt6_layout_t* l) {
    int bx = l->body_x, by = l->body_y, bw = l->body_w;
    pxe_draw_text(surf, bx, by, "Ready to Install Pixel Neko OS", RGB_COL(0x0C4A6E));
    pxe_draw_text_small(surf, bx, by + 22,
                        "Review the installation summary below before modifying your disk.",
                        RGB_COL(0x475569));

    pxe_fill_rect(surf, bx, by + 44, bw, 184, RGB_COL(0xFFFFFF));
    pxe_draw_rect(surf, bx, by + 44, bw, 184, RGB_COL(0x94A3B8));

    if (inst->sel_part >= 0 && inst->sel_part < inst->part_count) {
        const pxe_r0_part_info_t* p = &inst->parts[inst->sel_part];
        char sz[16], r1[88], r2[88], r3[88], r4[88], r5[88];
        fmt_sectors(p->total_sectors, sz, sizeof(sz));
        snprintf(r1, sizeof(r1), "Disk %d, Partition %d  (%s)",
                 p->disk_index, p->part_index, p->disk_name[0] ? p->disk_name : "Fixed Disk");
        snprintf(r2, sizeof(r2), "%s  (%llu sectors, Start LBA %llu)",
                 sz, (unsigned long long)p->total_sectors, (unsigned long long)p->start_lba);
        const char* fs_action =
            (inst->fs_choice == 0) ? "Keep existing FAT filesystem (Skip Format)"
                                   : (inst->fs_choice == PART_FS_FAT32 ? "Format as FAT32"
                                                                       : "Format as FAT16");
        snprintf(r3, sizeof(r3), "%s", fs_action);
        snprintf(r4, sizeof(r4), "%s", inst->label[0] ? inst->label : "PIXELNEKO");
        snprintf(r5, sizeof(r5), "Limine BIOS Stage 1/2 + Local Offline Boot Config");

        int y0 = by + 58;
        pxe_draw_text_small(surf, bx + 16, y0 + 0,   "Target Partition:", RGB_COL(0x475569));
        pxe_draw_text_small(surf, bx + 150, y0 + 0,  r1, RGB_COL(0x0F172A));
        pxe_draw_text_small(surf, bx + 16, y0 + 28,  "Geometry / Size:", RGB_COL(0x475569));
        pxe_draw_text_small(surf, bx + 150, y0 + 28, r2, RGB_COL(0x0F172A));
        pxe_draw_text_small(surf, bx + 16, y0 + 56,  "Filesystem Mode:", RGB_COL(0x475569));
        pxe_draw_text_small(surf, bx + 150, y0 + 56, r3,
                            inst->fs_choice == 0 ? RGB_COL(0x166534) : RGB_COL(0xB91C1C));
        pxe_draw_text_small(surf, bx + 16, y0 + 84,  "Volume Label:", RGB_COL(0x475569));
        pxe_draw_text_small(surf, bx + 150, y0 + 84, r4, RGB_COL(0x0F172A));
        pxe_draw_text_small(surf, bx + 16, y0 + 112, "Bootloader:", RGB_COL(0x475569));
        pxe_draw_text_small(surf, bx + 150, y0 + 112, r5, RGB_COL(0x0F172A));
        pxe_draw_text_small(surf, bx + 16, y0 + 140, "Payload Source:", RGB_COL(0x475569));
        char r6[64];
        snprintf(r6, sizeof(r6), "Optical / Live Media (%c:\\payload)",
                 inst->sysinfo.cdrom_drive ? inst->sysinfo.cdrom_drive : '-');
        pxe_draw_text_small(surf, bx + 150, y0 + 140, r6, RGB_COL(0x0F172A));
    }

    int nw_y = by + 240;
    if (inst->fs_choice != 0) {
        pxe_fill_rect(surf, bx, nw_y, bw, 42, RGB_COL(0xFEF2F2));
        pxe_draw_rect(surf, bx, nw_y, bw, 42, RGB_COL(0xDC2626));
        pxe_draw_text_small(surf, bx + 12, nw_y + 16,
                            "Warning: Clicking [Install] will FORMAT the selected partition and erase all data!",
                            RGB_COL(0x991B1B));
    } else {
        pxe_fill_rect(surf, bx, nw_y, bw, 42, RGB_COL(0xECFDF5));
        pxe_draw_rect(surf, bx, nw_y, bw, 42, RGB_COL(0x16A34A));
        pxe_draw_text_small(surf, bx + 12, nw_y + 16,
                            "Existing FAT partition preserved. Click [Install] to release files & update boot.",
                            RGB_COL(0x166534));
    }
}

static void render_step_progress(const pxe_win_surface_t* surf, const installer_t* inst,
                                 const nt6_layout_t* l) {
    int bx = l->body_x, by = l->body_y, bw = l->body_w, bh = l->body_h;
    pxe_draw_text(surf, bx, by, "Installing Pixel Neko OS...", RGB_COL(0x0C4A6E));
    pxe_draw_text_small(surf, bx, by + 22,
                        "Your computer stays active during setup. Please do not power off.",
                        RGB_COL(0x475569));

    static const char* const k_tasks[5] = {
        "Preparing and mounting target partition",
        "Scanning installation payload media (\\payload)",
        "Copying kernel, system apps (.pxe) & drivers (.pdv)",
        "Deploying offline Limine boot configuration",
        "Installing Limine BIOS bootloader & verifying files"
    };
    static const int k_min_pct[5] = { 0, 18, 24, 68, 82 };
    static const int k_done_pct[5] = { 18, 24, 68, 82, 100 };

    pxe_fill_rect(surf, bx, by + 40, bw, 114, RGB_COL(0xFFFFFF));
    pxe_draw_rect(surf, bx, by + 40, bw, 114, RGB_COL(0xCBD5E1));
    for (int i = 0; i < 5; i++) {
        int ty = by + 48 + i * 20;
        int done = (inst->progress >= k_done_pct[i]);
        int active = (!done && inst->progress >= k_min_pct[i]);
        const char* tag = done ? "[OK]" : (active ? "[>>]" : "[  ]");
        uint32_t col = done ? RGB_COL(0x15803D)
                            : (active ? RGB_COL(0x0B5ED7) : RGB_COL(0x94A3B8));
        pxe_draw_text_small(surf, bx + 14, ty, tag, col);
        pxe_draw_text_small(surf, bx + 46, ty, k_tasks[i],
                            done ? RGB_COL(0x166534)
                                 : (active ? RGB_COL(0x0F172A) : RGB_COL(0x64748B)));
    }

    char pct_s[16];
    snprintf(pct_s, sizeof(pct_s), "%3d%%", inst->progress);
    pxe_draw_text_small(surf, bx, by + 162,
                        inst->stage[0] ? inst->stage : "Working...", RGB_COL(0x0F172A));
    pxe_draw_text_small(surf, bx + bw - 30, by + 162, pct_s, RGB_COL(0x0F172A));

    int bar_y = by + 178;
    pxe_fill_rect(surf, bx, bar_y, bw, 18, RGB_COL(0xE2E8F0));
    pxe_draw_rect(surf, bx, bar_y, bw, 18, RGB_COL(0x94A3B8));
    int fw = ((bw - 4) * inst->progress) / 100;
    if (fw > bw - 4) fw = bw - 4;
    if (fw > 0) {
        pxe_fill_rect(surf, bx + 2, bar_y + 2, fw, 14, RGB_COL(0x22A84B));
        pxe_fill_rect(surf, bx + 2, bar_y + 2, fw, 5,  RGB_COL(0x4ADE80));
    }

    if (inst->status_detail[0]) {
        pxe_draw_text_small(surf, bx, by + 202, inst->status_detail, RGB_COL(0x475569));
    }

    int log_y = by + 218;
    int log_h = bh - 222;
    if (log_h >= 48) {
        pxe_fill_rect(surf, bx, log_y, bw, log_h, RGB_COL(0x0F172A));
        pxe_draw_rect(surf, bx, log_y, bw, log_h, RGB_COL(0x334155));
        int max_vis = (log_h - 8) / 13;
        int start_l = (inst->log_count > max_vis) ? (inst->log_count - max_vis) : 0;
        for (int i = start_l; i < inst->log_count; i++) {
            pxe_draw_text_small(surf, bx + 8, log_y + 5 + (i - start_l) * 13,
                                inst->logs[i], RGB_COL(0x4ADE80));
        }
    }
}

static void render_step_complete(const pxe_win_surface_t* surf, const installer_t* inst,
                                 const nt6_layout_t* l) {
    int bx = l->body_x, by = l->body_y, bw = l->body_w, bh = l->body_h;
    int ok = (inst->install_error == 0);
    pxe_fill_rect(surf, bx, by + 6, bw, 76,
                  ok ? RGB_COL(0xECFDF5) : RGB_COL(0xFEF2F2));
    pxe_draw_rect(surf, bx, by + 6, bw, 76,
                  ok ? RGB_COL(0x16A34A) : RGB_COL(0xDC2626));
    pxe_fill_rect(surf, bx + 16, by + 22, 32, 32,
                  ok ? RGB_COL(0x16A34A) : RGB_COL(0xDC2626));
    pxe_draw_text(surf, bx + 24, by + 30, ok ? "OK" : "!!", RGB_COL(0xFFFFFF));

    if (ok) {
        pxe_draw_text(surf, bx + 60, by + 18,
                      "Pixel Neko OS Installation Complete", RGB_COL(0x14532D));
        pxe_draw_text_small(surf, bx + 60, by + 44,
                            "System files and Limine bootloader were written successfully.",
                            RGB_COL(0x166534));
        pxe_draw_text_small(surf, bx + 60, by + 60,
                            "Remove installation CD/ISO media before clicking [Reboot System].",
                            RGB_COL(0x15803D));
    } else {
        char etitle[64];
        snprintf(etitle, sizeof(etitle), "Installation Failed (Error %d)", inst->install_error);
        pxe_draw_text(surf, bx + 60, by + 18, etitle, RGB_COL(0x991B1B));
        pxe_draw_text_small(surf, bx + 60, by + 44,
                            inst->stage[0] ? inst->stage : "Check installation log below.",
                            RGB_COL(0xB91C1C));
    }

    int log_y = by + 96;
    int log_h = bh - 102;
    if (log_h >= 60) {
        pxe_fill_rect(surf, bx, log_y, bw, log_h, RGB_COL(0x0F172A));
        pxe_draw_rect(surf, bx, log_y, bw, log_h, RGB_COL(0x334155));
        int max_vis = (log_h - 10) / 14;
        int start_l = (inst->log_count > max_vis) ? (inst->log_count - max_vis) : 0;
        for (int i = start_l; i < inst->log_count; i++) {
            pxe_draw_text_small(surf, bx + 10, log_y + 6 + (i - start_l) * 14,
                                inst->logs[i],
                                ok ? RGB_COL(0x4ADE80) : RGB_COL(0xFCA5A5));
        }
    }
}

static void render_installer(const installer_t* inst) {
    const pxe_win_surface_t* surf = &inst->surf;
    nt6_layout_t l;
    calc_nt6_layout(surf, &l);

    draw_nt6_backdrop(surf, inst);

    /* Dialog outer shadow & frame */
    pxe_fill_rect(surf, l.card_x + 4, l.card_y + 4, l.card_w, l.card_h, RGB_COL(0x041C2B));
    pxe_fill_rect(surf, l.card_x, l.card_y, l.card_w, l.card_h, RGB_COL(0xF8FAFC));
    pxe_draw_rect(surf, l.card_x, l.card_y, l.card_w, l.card_h, RGB_COL(0x0B2D45));

    /* Card top header */
    pxe_fill_rect(surf, l.card_x + 1, l.card_y + 1, l.card_w - 2, l.hdr_h, RGB_COL(0xFFFFFF));
    draw_neko_badge(surf, l.card_x + 14, l.card_y + 10);
    pxe_draw_text(surf, l.card_x + 52, l.card_y + 8, "Install Pixel Neko OS", RGB_COL(0x0F172A));
    pxe_draw_text_small(surf, l.card_x + 52, l.card_y + 28,
                        "Guided Setup Wizard (Ring 0)", RGB_COL(0x64748B));

    /* Two-phase NT6 setup progress breadcrumb bar */
    int sy = l.card_y + l.hdr_h;
    pxe_fill_rect(surf, l.card_x + 1, sy, l.card_w - 2, l.step_h, RGB_COL(0xE6EEF6));
    pxe_draw_hline(surf, l.card_x + 1, sy, l.card_w - 2, RGB_COL(0xCBD5E1));
    pxe_draw_hline(surf, l.card_x + 1, sy + l.step_h - 1, l.card_w - 2, RGB_COL(0xB8C9DB));

    int phase_collect = (inst->step <= STEP_CONFIRM);
    int phase_install = (inst->step >= STEP_PROGRESS);
    pxe_draw_text_small(surf, l.card_x + 20, sy + 9,
                        phase_collect ? "[*] 1. Collecting information"
                                      : "[OK] 1. Collecting information",
                        phase_collect ? RGB_COL(0x0B5ED7) : RGB_COL(0x15803D));
    pxe_draw_text_small(surf, l.card_x + 250, sy + 9,
                        inst->step == STEP_COMPLETE && inst->install_error == 0
                            ? "[OK] 2. Installing Pixel Neko OS"
                            : (phase_install ? "[*] 2. Installing Pixel Neko OS"
                                             : "[ ] 2. Installing Pixel Neko OS"),
                        inst->step == STEP_COMPLETE && inst->install_error == 0
                            ? RGB_COL(0x15803D)
                            : (phase_install ? RGB_COL(0x0B5ED7) : RGB_COL(0x64748B)));

    /* Current step body */
    switch (inst->step) {
        case STEP_WELCOME:        render_step_welcome(surf, inst, &l); break;
        case STEP_SELECT:         render_step_select(surf, inst, &l); break;
        case STEP_REINSTALL_WARN: render_step_reinstall_warn(surf, inst, &l); break;
        case STEP_FORMAT:         render_step_format(surf, inst, &l); break;
        case STEP_CONFIRM:        render_step_confirm(surf, inst, &l); break;
        case STEP_PROGRESS:       render_step_progress(surf, inst, &l); break;
        case STEP_COMPLETE:       render_step_complete(surf, inst, &l); break;
    }

    /* Footer bar */
    pxe_fill_rect(surf, l.card_x + 1, l.foot_y, l.card_w - 2, l.foot_h - 1, RGB_COL(0xE9EEF4));
    pxe_draw_hline(surf, l.card_x + 1, l.foot_y, l.card_w - 2, RGB_COL(0xCBD5E1));

    if (inst->step == STEP_WELCOME) {
        if (!inst->install_mode) {
            draw_nt6_btn(surf, l.back_x, l.back_y, l.back_w, l.back_h, "Exit", 0, 1);
        }
        draw_nt6_btn(surf, l.next_x, l.next_y, l.next_w, l.next_h, "Next >", 1, 1);
    } else if (inst->step == STEP_SELECT) {
        draw_nt6_btn(surf, l.back_x, l.back_y, l.back_w, l.back_h, "< Back", 0, 1);
        draw_nt6_btn(surf, l.next_x, l.next_y, l.next_w, l.next_h, "Next >", 1, inst->sel_part >= 0);
    } else if (inst->step == STEP_REINSTALL_WARN) {
        draw_nt6_btn(surf, l.back_x, l.back_y, l.back_w, l.back_h, "< Back", 0, 1);
        draw_nt6_btn(surf, l.next_x, l.next_y, l.next_w, l.next_h, "Continue >", 1, 1);
    } else if (inst->step == STEP_FORMAT) {
        draw_nt6_btn(surf, l.back_x, l.back_y, l.back_w, l.back_h, "< Back", 0, 1);
        draw_nt6_btn(surf, l.next_x, l.next_y, l.next_w, l.next_h, "Next >", 1, 1);
    } else if (inst->step == STEP_CONFIRM) {
        draw_nt6_btn(surf, l.back_x, l.back_y, l.back_w, l.back_h, "< Back", 0, 1);
        draw_nt6_btn(surf, l.next_x, l.next_y, l.next_w, l.next_h, "Install", 2, 1);
    } else if (inst->step == STEP_PROGRESS) {
        pxe_draw_text_small(surf, l.card_x + 18, l.foot_y + 16,
                            "Installing... Please wait until setup finishes.", RGB_COL(0xB91C1C));
    } else if (inst->step == STEP_COMPLETE) {
        if (inst->install_error == 0) {
            if (!inst->install_mode) {
                draw_nt6_btn(surf, l.back_x, l.back_y, l.back_w, l.back_h, "Exit", 0, 1);
            }
            draw_nt6_btn(surf, l.shut_x, l.shut_y, l.shut_w, l.shut_h, "Shut Down", 0, 1);
            draw_nt6_btn(surf, l.next_x, l.next_y, l.next_w, l.next_h, "Reboot System", 2, 1);
        } else {
            draw_nt6_btn(surf, l.back_x, l.back_y, l.back_w + 18, l.back_h, "< Retry Setup", 0, 1);
            draw_nt6_btn(surf, l.next_x, l.next_y, l.next_w, l.next_h, "Reboot System", 1, 1);
        }
    }
}

static void pump_and_present(installer_t* inst) {
    pxe_event_t ev;
    while (pxe_win_poll_event(inst->win_id, &ev) > 0) {
        /* Drain queue to keep WM responsive during long copies */
    }
    render_installer(inst);
    pxe_win_present(inst->win_id);
}

static void set_stage(installer_t* inst, int pct, const char* stage, const char* detail) {
    inst->progress = pct;
    snprintf(inst->stage, sizeof(inst->stage), "%s", stage);
    snprintf(inst->status_detail, sizeof(inst->status_detail), "%s", detail ? detail : "");
    pump_and_present(inst);
}

static int find_payload_source_drive(installer_t* inst, char* out_letter) {
    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);

    pxe_dirent_t st;
    char probe[48];
    if (info.cdrom_drive) {
        snprintf(probe, sizeof(probe), "%c:\\payload\\system\\PAYLOAD.TXT", info.cdrom_drive);
        if (pxe_stat(probe, &st) == 0 && !st.is_dir) {
            *out_letter = info.cdrom_drive;
            return 0;
        }
    }

    pxe_drive_info_t drvs[26];
    int dn = pxe_get_drives(drvs, 26);
    for (int i = 0; i < dn; i++) {
        if (!drvs[i].present || !drvs[i].letter) continue;
        snprintf(probe, sizeof(probe), "%c:\\payload\\system\\PAYLOAD.TXT", drvs[i].letter);
        if (pxe_stat(probe, &st) == 0 && !st.is_dir) {
            *out_letter = drvs[i].letter;
            return 0;
        }
    }
    (void)inst;
    return -1;
}

static int prepare_gpt_bios_embed(int disk_id, uint16_t size_a, uint16_t size_b,
                                  uint64_t* loc_a, uint64_t* loc_b) {
    uint8_t mbr[512];
    uint8_t gpt[512];
    if (pxe_r0_raw_disk_rw(disk_id, 0, 1, mbr, false) != 0) return -15;
    if (pxe_r0_raw_disk_rw(disk_id, 1, 1, gpt, false) != 0) return -15;

    if (memcmp(gpt, "EFI PART", 8) != 0) return 1; /* Not GPT, use MBR */

    int has_ee = 0;
    for (int i = 0; i < 4; i++) {
        if (mbr[446 + i * 16 + 4] == 0xEEu) {
            has_ee = 1;
            break;
        }
    }
    if (!has_ee) return 1;

    uint32_t hdr_size = get_le32(gpt + 12);
    uint64_t backup_lba = get_le64(gpt + 32);
    uint64_t first_usable = get_le64(gpt + 40);
    uint64_t part_lba = get_le64(gpt + 72);
    uint32_t num_entries = get_le32(gpt + 80);
    uint32_t entry_size = get_le32(gpt + 84);

    if (hdr_size < 92 || hdr_size > 512) return -12;
    if (entry_size < 128 || (512u % entry_size) != 0 || num_entries == 0 || num_entries > 256u) {
        return -12;
    }

    uint32_t table_sects = (num_entries * entry_size + 511u) / 512u;
    uint8_t* tbl = (uint8_t*)malloc((size_t)table_sects * 512u);
    if (!tbl) return -11;
    if (pxe_r0_raw_disk_rw(disk_id, part_lba, table_sects, tbl, false) != 0) {
        free(tbl);
        return -15;
    }

    int last_used = -1;
    for (uint32_t i = 0; i < num_entries; i++) {
        const uint8_t* ent = tbl + (size_t)i * entry_size;
        int nonzero = 0;
        for (int b = 0; b < 16; b++) {
            if (ent[b] != 0) {
                nonzero = 1;
                break;
            }
        }
        if (nonzero) last_used = (int)i;
    }

    uint32_t used_count = (last_used >= 0) ? (uint32_t)(last_used + 1) : 1u;
    uint32_t per_sec = 512u / entry_size;
    uint32_t used_secs = (used_count + per_sec - 1u) / per_sec;
    uint32_t new_count = used_secs * per_sec;
    uint64_t gap_start = part_lba + (uint64_t)used_secs;
    uint64_t stage2_sects = (uint64_t)((size_a + size_b) / 512u);

    if (gap_start + stage2_sects > first_usable) {
        free(tbl);
        return -12;
    }

    *loc_a = gap_start;
    *loc_b = gap_start + (uint64_t)(size_a / 512u);

    uint32_t tbl_crc = crc32_ieee(tbl, (size_t)new_count * entry_size);
    free(tbl);

    put_le32(gpt + 80, new_count);
    put_le32(gpt + 88, tbl_crc);
    put_le32(gpt + 16, 0);
    put_le32(gpt + 16, crc32_ieee(gpt, hdr_size));
    if (pxe_r0_raw_disk_rw(disk_id, 1, 1, gpt, true) != 0) return -12;

    if (backup_lba > first_usable) {
        uint8_t sec_gpt[512];
        if (pxe_r0_raw_disk_rw(disk_id, backup_lba, 1, sec_gpt, false) != 0) return -12;
        if (memcmp(sec_gpt, "EFI PART", 8) == 0) {
            put_le32(sec_gpt + 80, new_count);
            put_le32(sec_gpt + 88, tbl_crc);
            put_le32(sec_gpt + 16, 0);
            put_le32(sec_gpt + 16, crc32_ieee(sec_gpt, hdr_size));
            if (pxe_r0_raw_disk_rw(disk_id, backup_lba, 1, sec_gpt, true) != 0) return -12;
        }
    }
    return 0;
}

static int install_limine_bios_sectors(installer_t* inst, int disk_id) {
    const uint8_t* image = binary_limine_hdd_bin_data;
    size_t image_size = sizeof(binary_limine_hdd_bin_data);
    if (image_size <= 512u) return -15;

    size_t stage2_size = image_size - 512u;
    size_t stage2_sects = (stage2_size + 511u) / 512u;
    uint16_t size_a = (uint16_t)((stage2_sects / 2u) * 512u + ((stage2_sects % 2u) ? 512u : 0u));
    uint16_t size_b = (uint16_t)((stage2_sects / 2u) * 512u);

    uint8_t original_mbr[512];
    if (pxe_r0_raw_disk_rw(disk_id, 0, 1, original_mbr, false) != 0) return -15;

    uint64_t loc_a = 0, loc_b = 0;
    int gpt_res = prepare_gpt_bios_embed(disk_id, size_a, size_b, &loc_a, &loc_b);
    if (gpt_res < 0) return gpt_res;
    if (gpt_res > 0) {
        /* Standard MBR gap */
        uint64_t min_start = 0xFFFFFFFFFFFFFFFFull;
        for (int i = 0; i < 4; i++) {
            const uint8_t* pe = original_mbr + 446 + i * 16;
            uint8_t ptype = pe[4];
            uint32_t st = get_le32(pe + 8);
            uint32_t sc = get_le32(pe + 12);
            if (ptype != 0 && sc > 0 && st > 0 && (uint64_t)st < min_start) {
                min_start = (uint64_t)st;
            }
        }
        loc_a = 1ull;
        loc_b = 1ull + (uint64_t)(size_a / 512u);
        if (min_start == 0xFFFFFFFFFFFFFFFFull || (1ull + (uint64_t)stage2_sects) > min_start) {
            add_log(inst, "MBR gap too small for Limine stage2.");
            return -12;
        }
    }

    /* Stage 2 first half */
    size_t head_bytes = (size_t)size_a;
    uint8_t* head_buf = (uint8_t*)malloc(head_bytes);
    if (!head_buf) return -11;
    memcpy(head_buf, image + 512u, head_bytes);
    int w1 = pxe_r0_raw_disk_rw(disk_id, loc_a, (uint32_t)(size_a / 512u), head_buf, true);
    free(head_buf);
    if (w1 != 0) return -15;

    /* Stage 2 second half */
    size_t tail_off = 512u + (size_t)size_a;
    size_t tail_size = image_size - tail_off;
    size_t tail_sects = (tail_size + 511u) / 512u;
    uint8_t* tail_buf = (uint8_t*)calloc(tail_sects, 512u);
    if (!tail_buf) return -11;
    memcpy(tail_buf, image + tail_off, tail_size);
    int w2 = pxe_r0_raw_disk_rw(disk_id, loc_b, (uint32_t)tail_sects, tail_buf, true);
    free(tail_buf);
    if (w2 != 0) return -15;

    /* MBR Stage 1 */
    uint8_t bootsector[512];
    memcpy(bootsector, image, 512u);
    memcpy(bootsector + 440, original_mbr + 440, 70u);
    put_le16(bootsector + 0x1A4, size_a);
    put_le16(bootsector + 0x1A6, size_b);
    put_le64(bootsector + 0x1A8, loc_a * 512ull);
    put_le64(bootsector + 0x1B0, loc_b * 512ull);

    if (pxe_r0_raw_disk_rw(disk_id, 0, 1, bootsector, true) != 0) return -15;

    /* Read-back verify MBR and first sector of stage2 */
    uint8_t verify_mbr[512];
    if (pxe_r0_raw_disk_rw(disk_id, 0, 1, verify_mbr, false) != 0 ||
        verify_mbr[510] != 0x55u || verify_mbr[511] != 0xAAu ||
        memcmp(verify_mbr + 446, original_mbr + 446, 64u) != 0) {
        return -15;
    }

    uint8_t verify_s2[512];
    if (pxe_r0_raw_disk_rw(disk_id, loc_a, 1, verify_s2, false) != 0 ||
        memcmp(verify_s2, image + 512u, 64u) != 0) {
        return -15;
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "Limine BIOS Stage1/2 OK (LBA %llu / %llu).",
             (unsigned long long)loc_a, (unsigned long long)loc_b);
    add_log(inst, msg);
    return 0;
}

static int run_installation(installer_t* inst) {
    if (inst->sel_part < 0 || inst->sel_part >= inst->part_count) return -1;
    pxe_r0_part_info_t target = inst->parts[inst->sel_part];

    inst->state = STATE_INSTALLING;
    inst->log_count = 0;

    /* Step 1: Format or Skip Format + Resolve target drive letter */
    char target_letter = 0;
    const char* vol_label = inst->label[0] ? inst->label : "PIXELNEKO";

    if (inst->fs_choice == 0) {
        if (target.fs_type != PART_FS_FAT16 && target.fs_type != PART_FS_FAT32) {
            add_log(inst, "Error: Skip Format requires existing FAT16/FAT32.");
            return -2;
        }
        set_stage(inst, 10, "Keeping existing filesystem...", "Remounting VFS volumes");
        add_log(inst, "Skipping format on existing FAT partition.");
    } else {
        set_stage(inst, 6, "Formatting target partition...",
                  inst->fs_choice == PART_FS_FAT32 ? "Creating FAT32 filesystem"
                                                   : "Creating FAT16 filesystem");
        char fmsg[80];
        snprintf(fmsg, sizeof(fmsg), "Formatting P%d (%s, label=%s)...",
                 target.part_index,
                 inst->fs_choice == PART_FS_FAT32 ? "FAT32" : "FAT16",
                 vol_label);
        add_log(inst, fmsg);
        if (pxe_r0_format_partition(target.part_index, inst->fs_choice, vol_label) != 0) {
            add_log(inst, "Format partition failed!");
            return -2;
        }
    }

    set_stage(inst, 15, "Mounting target volume...", "Refreshing VFS partition table");
    pxe_r0_vfs_remount_all();
    refresh_partitions(inst);

    for (int i = 0; i < inst->part_count; i++) {
        if (inst->parts[i].disk_index == target.disk_index &&
            inst->parts[i].start_lba == target.start_lba) {
            inst->sel_part = i;
            target_letter = inst->parts[i].mounted_letter;
            break;
        }
    }
    if (!target_letter) {
        add_log(inst, "Failed to mount target partition drive letter.");
        return -13;
    }

    char mmsg[64];
    snprintf(mmsg, sizeof(mmsg), "Target mounted at %c:\\", target_letter);
    add_log(inst, mmsg);

    /* Step 2: Locate CD payload */
    set_stage(inst, 18, "Scanning installation source media...", "Looking for \\payload\\system\\PAYLOAD.TXT");
    char src_letter = 0;
    if (find_payload_source_drive(inst, &src_letter) != 0) {
        add_log(inst, "Missing \\payload\\system\\PAYLOAD.TXT on any drive.");
        return -10;
    }
    snprintf(mmsg, sizeof(mmsg), "Source payload media found on %c:\\payload", src_letter);
    add_log(inst, mmsg);

    /* Discover directory tree iteratively */
    rel_path_t* dirs = (rel_path_t*)calloc(MAX_DIRS, sizeof(rel_path_t));
    copy_item_t* files = (copy_item_t*)calloc(MAX_FILES, sizeof(copy_item_t));
    pxe_dirent_t* ents = (pxe_dirent_t*)calloc(256, sizeof(pxe_dirent_t));
    if (!dirs || !files || !ents) {
        free(dirs); free(files); free(ents);
        return -11;
    }

    int dir_head = 0;
    int dir_tail = 1;
    dirs[0].rel[0] = '\0';
    int file_count = 0;

    while (dir_head < dir_tail) {
        const char* rel = dirs[dir_head++].rel;
        char src_dir[256];
        char dst_dir[256];
        if (rel[0]) {
            snprintf(src_dir, sizeof(src_dir), "%c:\\payload\\%s", src_letter, rel);
            snprintf(dst_dir, sizeof(dst_dir), "%c:\\%s", target_letter, rel);
            pxe_mkdir(dst_dir);
        } else {
            snprintf(src_dir, sizeof(src_dir), "%c:\\payload", src_letter);
        }

        int cnt = pxe_listdir(src_dir, ents, 256);
        if (cnt < 0) continue;
        for (int i = 0; i < cnt; i++) {
            if (!ents[i].name[0]) continue;
            if (strcmp(ents[i].name, ".") == 0 || strcmp(ents[i].name, "..") == 0) continue;
            char next_rel[160];
            if (rel[0]) {
                snprintf(next_rel, sizeof(next_rel), "%.80s\\%.76s", rel, ents[i].name);
            } else {
                snprintf(next_rel, sizeof(next_rel), "%.156s", ents[i].name);
            }
            if (ents[i].is_dir) {
                if (dir_tail < MAX_DIRS) {
                    snprintf(dirs[dir_tail].rel, sizeof(dirs[dir_tail].rel), "%.159s", next_rel);
                    dir_tail++;
                }
            } else {
                if (file_count < MAX_FILES) {
                    snprintf(files[file_count].rel, sizeof(files[file_count].rel), "%.159s", next_rel);
                    files[file_count].size = ents[i].size;
                    file_count++;
                }
            }
        }
    }
    free(ents);
    free(dirs);

    if (file_count <= 0) {
        free(files);
        add_log(inst, "No payload files found inside \\payload.");
        return -10;
    }

    snprintf(mmsg, sizeof(mmsg), "Copying %d payload files...", file_count);
    set_stage(inst, 24, mmsg, "Includes kernel, boot files, APPS\\*.pxe, PDEVICE\\*.pdv");
    add_log(inst, mmsg);

    /* Step 3: Copy & verify every payload file */
    int pxe_copied = 0;
    int pdv_copied = 0;
    for (int i = 0; i < file_count; i++) {
        char src_file[256], dst_file[256];
        snprintf(src_file, sizeof(src_file), "%c:\\payload\\%s", src_letter, files[i].rel);
        snprintf(dst_file, sizeof(dst_file), "%c:\\%s", target_letter, files[i].rel);

        int pct = 24 + ((i * 42) / file_count);
        snprintf(inst->status_detail, sizeof(inst->status_detail), "Copying %d/%d: %s",
                 i + 1, file_count, files[i].rel);
        inst->progress = pct;
        if ((i % 3) == 0 || files[i].size > 65536ull) {
            pump_and_present(inst);
        }

        if (pxe_copy_file(src_file, dst_file, NULL) != 0) {
            char emsg[84];
            snprintf(emsg, sizeof(emsg), "Copy failed: %s", files[i].rel);
            add_log(inst, emsg);
            free(files);
            return -14;
        }
        pxe_dirent_t vst;
        if (pxe_stat(dst_file, &vst) != 0 || vst.is_dir || vst.size != files[i].size) {
            char emsg[84];
            snprintf(emsg, sizeof(emsg), "Size verify failed: %s", files[i].rel);
            add_log(inst, emsg);
            free(files);
            return -14;
        }
        if (ends_with_ci(files[i].rel, ".pxe")) pxe_copied++;
        if (ends_with_ci(files[i].rel, ".pdv")) pdv_copied++;
    }
    free(files);

    snprintf(mmsg, sizeof(mmsg), "Payload OK: %d files (%d .pxe, %d .pdv).",
             file_count, pxe_copied, pdv_copied);
    add_log(inst, mmsg);

    /* Step 4: Deploy limine.conf for offline boot */
    set_stage(inst, 68, "Deploying Limine offline configuration...",
              "Writing boot\\limine.conf and \\limine.conf");
    char bdir1[16], bdir2[24];
    snprintf(bdir1, sizeof(bdir1), "%c:\\boot", target_letter);
    snprintf(bdir2, sizeof(bdir2), "%c:\\boot\\limine", target_letter);
    pxe_mkdir(bdir1);
    pxe_mkdir(bdir2);

    char cfg_src[64];
    snprintf(cfg_src, sizeof(cfg_src), "%c:\\payload\\system\\limine-installed.conf", src_letter);
    pxe_dirent_t cst;
    if (pxe_stat(cfg_src, &cst) != 0) {
        snprintf(cfg_src, sizeof(cfg_src), "%c:\\system\\limine-installed.conf", target_letter);
    }
    const char* cfg_targets[3] = {
        "boot\\limine.conf", "boot\\limine\\limine.conf", "limine.conf"
    };
    for (int i = 0; i < 3; i++) {
        char cdst[64];
        snprintf(cdst, sizeof(cdst), "%c:\\%s", target_letter, cfg_targets[i]);
        if (pxe_copy_file(cfg_src, cdst, NULL) != 0) {
            add_log(inst, "Failed to deploy limine.conf!");
            return -14;
        }
    }
    pxe_r0_sync_filesystems();

    /* Step 5: Verify critical boot and modular files */
    set_stage(inst, 74, "Verifying installed files...",
              "Checking kernel.elf, limine, APPS\\*.pxe & PDEVICE\\");
    const char* req_files[] = {
        "limine.conf",
        "boot\\limine.conf",
        "boot\\limine\\limine-bios.sys",
        "boot\\kernel.elf",
        "system\\limine-installed.conf",
        "system\\PAYLOAD.TXT",
        "APPS\\install.pxe",
        "APPS\\sysinfo.pxe"
    };
    for (size_t i = 0; i < sizeof(req_files) / sizeof(req_files[0]); i++) {
        char p[80];
        pxe_dirent_t rst;
        snprintf(p, sizeof(p), "%c:\\%s", target_letter, req_files[i]);
        if (pxe_stat(p, &rst) != 0 || rst.is_dir || rst.size == 0) {
            char emsg[84];
            snprintf(emsg, sizeof(emsg), "Missing required file: %s", req_files[i]);
            add_log(inst, emsg);
            return -14;
        }
    }

    char pdev_dir[32];
    pxe_dirent_t pdev_st;
    snprintf(pdev_dir, sizeof(pdev_dir), "%c:\\PDEVICE", target_letter);
    if (pxe_stat(pdev_dir, &pdev_st) != 0 || !pdev_st.is_dir) {
        pxe_mkdir(pdev_dir);
    }

    /* Step 6: Install Limine BIOS MBR + Stage 2 */
    set_stage(inst, 82, "Installing Limine BIOS Bootloader...",
              "Writing Stage 2 and Stage 1 MBR");
    int bios_rc = install_limine_bios_sectors(inst, target.disk_index);
    if (bios_rc != 0) {
        add_log(inst, "Limine BIOS Stage 1/2 installation failed!");
        return bios_rc;
    }

    /* Step 7: Sync filesystems */
    set_stage(inst, 95, "Syncing filesystems...", "Flushing FAT metadata and caches");
    pxe_r0_sync_filesystems();
    pxe_r0_vfs_remount_all();

    set_stage(inst, 100, "Installation Complete!",
              "Remove CD media and click Reboot System.");
    add_log(inst, "Success! Pixel Neko OS is ready for offline boot.");
    return 0;
}

static void start_install(installer_t* inst) {
    if (inst->sel_part < 0 || inst->sel_part >= inst->part_count) return;
    inst->install_error = 0;
    inst->step = STEP_PROGRESS;
    int rc = run_installation(inst);
    inst->install_error = rc;
    if (rc == 0) {
        inst->state = STATE_DONE;
        pxe_beep();
    } else {
        inst->state = STATE_ERROR;
        char est[64];
        snprintf(est, sizeof(est), "Installation failed (code %d).", rc);
        snprintf(inst->stage, sizeof(inst->stage), "%s", est);
        pxe_beep();
    }
    inst->step = STEP_COMPLETE;
    pump_and_present(inst);
}

static void handle_installer_click(installer_t* inst, int mx, int my, int* running) {
    nt6_layout_t l;
    calc_nt6_layout(&inst->surf, &l);

    if (inst->step != STEP_FORMAT) {
        inst->label_focus = 0;
    }

    switch (inst->step) {
        case STEP_WELCOME:
            if (!inst->install_mode &&
                pxe_point_in_rect(mx, my, l.back_x, l.back_y, l.back_w, l.back_h)) {
                *running = 0;
                return;
            }
            if (pxe_point_in_rect(mx, my, l.wbtn_x, l.wbtn_y, l.wbtn_w, l.wbtn_h) ||
                pxe_point_in_rect(mx, my, l.next_x, l.next_y, l.next_w, l.next_h)) {
                inst->step = STEP_SELECT;
                refresh_partitions(inst);
            }
            break;

        case STEP_SELECT:
            if (pxe_point_in_rect(mx, my, l.init_x, l.init_y, l.init_w, l.init_h)) {
                int irc = pxe_r0_init_uninit_disk();
                pxe_r0_vfs_remount_all();
                refresh_partitions(inst);
                char msg[80];
                snprintf(msg, sizeof(msg), "Init Disk finished (rc=%d, parts=%d).",
                         irc, inst->part_count);
                add_log(inst, msg);
                return;
            }
            if (pxe_point_in_rect(mx, my, l.ref_x, l.ref_y, l.ref_w, l.ref_h)) {
                pxe_r0_vfs_remount_all();
                refresh_partitions(inst);
                add_log(inst, "Refreshed disk and partition list.");
                return;
            }
            {
                int max_scr = inst->part_count - l.vis_rows;
                if (max_scr < 0) max_scr = 0;
                if (pxe_point_in_rect(mx, my, l.list_x + l.list_w, l.list_y, SB_W, l.list_h)) {
                    if (my < l.list_y + l.list_h / 2) {
                        if (inst->part_scroll > 0) inst->part_scroll--;
                    } else {
                        if (inst->part_scroll < max_scr) inst->part_scroll++;
                    }
                    return;
                }
                if (pxe_point_in_rect(mx, my, l.list_x, l.list_y, l.list_w, l.list_h)) {
                    int row = (my - l.list_y - 2) / ROW_H;
                    int idx = inst->part_scroll + row;
                    if (row >= 0 && idx >= 0 && idx < inst->part_count) {
                        inst->sel_part = idx;
                        sync_fs_choice_to_selection(inst);
                    }
                    return;
                }
            }
            if (pxe_point_in_rect(mx, my, l.back_x, l.back_y, l.back_w, l.back_h)) {
                inst->step = STEP_WELCOME;
                return;
            }
            if (pxe_point_in_rect(mx, my, l.next_x, l.next_y, l.next_w, l.next_h) &&
                inst->sel_part >= 0 && inst->sel_part < inst->part_count) {
                inst->reinstall_ack = 0;
                sync_fs_choice_to_selection(inst);
                inst->step = inst->existing_on_disk[inst->sel_part]
                                 ? STEP_REINSTALL_WARN : STEP_FORMAT;
            }
            break;

        case STEP_REINSTALL_WARN:
            if (pxe_point_in_rect(mx, my, l.back_x, l.back_y, l.back_w, l.back_h)) {
                inst->reinstall_ack = 0;
                inst->step = STEP_SELECT;
            } else if (pxe_point_in_rect(mx, my, l.next_x, l.next_y, l.next_w, l.next_h)) {
                inst->reinstall_ack = 1;
                inst->step = STEP_FORMAT;
            }
            break;

        case STEP_FORMAT: {
            const pxe_r0_part_info_t* p =
                (inst->sel_part >= 0 && inst->sel_part < inst->part_count)
                    ? &inst->parts[inst->sel_part] : NULL;
            if (pxe_point_in_rect(mx, my, l.fcard_x, l.fcard_y0, l.fcard_w, l.fcard_h)) {
                inst->label_focus = 0;
                if (part_supports_fat32(p)) {
                    inst->skip_format = 0;
                    inst->fs_choice = PART_FS_FAT32;
                }
                return;
            }
            if (pxe_point_in_rect(mx, my, l.fcard_x, l.fcard_y1, l.fcard_w, l.fcard_h)) {
                inst->label_focus = 0;
                if (part_supports_fat16(p)) {
                    inst->skip_format = 0;
                    inst->fs_choice = PART_FS_FAT16;
                }
                return;
            }
            if (pxe_point_in_rect(mx, my, l.fcard_x, l.fcard_y2, l.fcard_w, l.fcard_h)) {
                inst->label_focus = 0;
                if (part_supports_skip_fmt(p)) {
                    inst->skip_format = 1;
                    inst->fs_choice = 0;
                }
                return;
            }
            if (pxe_point_in_rect(mx, my, l.lbl_x, l.lbl_y, l.lbl_w, l.lbl_h)) {
                inst->label_focus = 1;
                return;
            }
            inst->label_focus = 0;
            if (pxe_point_in_rect(mx, my, l.back_x, l.back_y, l.back_w, l.back_h)) {
                inst->step = (inst->sel_part >= 0 && inst->existing_on_disk[inst->sel_part])
                                 ? STEP_REINSTALL_WARN : STEP_SELECT;
                return;
            }
            if (pxe_point_in_rect(mx, my, l.next_x, l.next_y, l.next_w, l.next_h)) {
                sync_fs_choice_to_selection(inst);
                inst->step = STEP_CONFIRM;
            }
            break;
        }

        case STEP_CONFIRM:
            if (pxe_point_in_rect(mx, my, l.back_x, l.back_y, l.back_w, l.back_h)) {
                inst->step = STEP_FORMAT;
            } else if (pxe_point_in_rect(mx, my, l.next_x, l.next_y, l.next_w, l.next_h)) {
                start_install(inst);
            }
            break;

        case STEP_PROGRESS:
            break;

        case STEP_COMPLETE:
            if (inst->install_error == 0) {
                if (!inst->install_mode &&
                    pxe_point_in_rect(mx, my, l.back_x, l.back_y, l.back_w, l.back_h)) {
                    *running = 0;
                    return;
                }
                if (pxe_point_in_rect(mx, my, l.shut_x, l.shut_y, l.shut_w, l.shut_h)) {
                    pxe_r0_sync_filesystems();
                    pxe_r0_power_control(PXE_POWER_SHUTDOWN);
                    return;
                }
                if (pxe_point_in_rect(mx, my, l.next_x, l.next_y, l.next_w, l.next_h)) {
                    pxe_r0_sync_filesystems();
                    pxe_r0_power_control(PXE_POWER_REBOOT);
                }
            } else {
                if (pxe_point_in_rect(mx, my, l.back_x, l.back_y, l.back_w + 18, l.back_h)) {
                    inst->state = STATE_IDLE;
                    inst->progress = 0;
                    inst->install_error = 0;
                    inst->step = STEP_SELECT;
                    refresh_partitions(inst);
                    return;
                }
                if (pxe_point_in_rect(mx, my, l.next_x, l.next_y, l.next_w, l.next_h)) {
                    pxe_r0_sync_filesystems();
                    pxe_r0_power_control(PXE_POWER_REBOOT);
                }
            }
            break;
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    installer_t* inst = (installer_t*)calloc(1, sizeof(installer_t));
    if (!inst) return 1;

    strcpy(inst->label, "PIXELNEKO");
    inst->label_len = (int)strlen(inst->label);
    inst->fs_choice = PART_FS_FAT32;
    inst->sel_part = -1;
    inst->state = STATE_IDLE;
    inst->step = STEP_WELCOME;
    strcpy(inst->stage, "Select a target partition and proceed step by step.");

    refresh_partitions(inst);
    inst->install_mode = (inst->sysinfo.boot_mode == 1) ? 1 : 0;
    add_log(inst, "Installer ready. Ring 0 privileged disk/VFS APIs active.");

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (inst->install_mode) {
        int fw = (inst->sysinfo.screen_w > 64) ? (inst->sysinfo.screen_w - 2) : 798;
        int fh = (inst->sysinfo.screen_h > 64) ? (inst->sysinfo.screen_h - 40) : 560;
        desc.x = 0;
        desc.y = 20;
        desc.w = fw;
        desc.h = fh;
        desc.min_w = fw;
        desc.min_h = fh;
        desc.flags = PXE_WIN_NO_CLOSE;
    } else {
        int ww = (inst->sysinfo.screen_w > 700) ? 680 : (inst->sysinfo.screen_w - 16);
        int wh = (inst->sysinfo.screen_h > 530) ? 472 : (inst->sysinfo.screen_h - 48);
        if (ww < 540) ww = 540;
        if (wh < 400) wh = 400;
        desc.x = -1;
        desc.y = -1;
        desc.w = ww;
        desc.h = wh;
        desc.min_w = ww;
        desc.min_h = wh;
        desc.flags = PXE_WIN_NO_CLOSE | PXE_WIN_CENTERED;
    }
    desc.format = PXE_CANVAS_RGB32;
    strcpy(desc.title, "Install Pixel Neko OS");

    inst->win_id = pxe_win_create(&desc, &inst->surf);
    if (inst->win_id < 0) {
        free(inst);
        return 1;
    }

    int running = 1;
    int dirty = 1;

    while (running) {
        pxe_event_t ev;
        int rc;
        while ((rc = pxe_win_poll_event(inst->win_id, &ev)) > 0) {
            switch (ev.type) {
                case PXE_EV_CLOSE:
                    if (!inst->install_mode && inst->state != STATE_INSTALLING) running = 0;
                    break;
                case PXE_EV_RESIZE:
                    pxe_win_get_surface(inst->win_id, &inst->surf);
                    dirty = 1;
                    break;
                case PXE_EV_REDRAW:
                case PXE_EV_ACTIVATE:
                    dirty = 1;
                    break;
                case PXE_EV_MOUSE_DOWN: {
                    if (!(ev.buttons & PXE_BTN_LEFT)) break;
                    if (inst->state == STATE_INSTALLING) break;
                    handle_installer_click(inst, ev.x, ev.y, &running);
                    dirty = 1;
                    break;
                }
                case PXE_EV_KEY_DOWN:
                    if (inst->state == STATE_INSTALLING) break;
                    if (inst->step == STEP_FORMAT && inst->label_focus) {
                        if (ev.key == PXE_KEY_BACKSPACE) {
                            if (inst->label_len > 0) inst->label[--inst->label_len] = '\0';
                            dirty = 1;
                        } else if (ev.key == PXE_KEY_ENTER || ev.key == PXE_KEY_ESC) {
                            inst->label_focus = 0;
                            dirty = 1;
                        } else if (inst->label_len < 11) {
                            char ch = (char)ev.key;
                            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                            if ((ch >= 'A' && ch <= 'Z') ||
                                (ch >= '0' && ch <= '9') ||
                                ch == '_' || ch == '-') {
                                inst->label[inst->label_len++] = ch;
                                inst->label[inst->label_len] = '\0';
                                dirty = 1;
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        if (rc < 0) break;

        if (dirty && running) {
            render_installer(inst);
            pxe_win_present(inst->win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(30);
        }
    }

    pxe_win_destroy(inst->win_id);
    free(inst);
    return 0;
}
