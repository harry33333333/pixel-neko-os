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

#define ROW_H 18
#define SB_W  14

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
} installer_t;

static uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

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

static void sync_fs_choice_to_selection(installer_t* inst) {
    if (inst->sel_part < 0 || inst->sel_part >= inst->part_count) return;
    const pxe_r0_part_info_t* p = &inst->parts[inst->sel_part];
    if (inst->fs_choice == 0) {
        if (p->fs_type != PART_FS_FAT16 && p->fs_type != PART_FS_FAT32) {
            inst->fs_choice = (p->total_sectors >= 65536ull) ? PART_FS_FAT32 : PART_FS_FAT16;
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
        sync_fs_choice_to_selection(inst);
    }
}

static void render_installer(const installer_t* inst) {
    const pxe_win_surface_t* surf = &inst->surf;
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    /* Top Banner */
    pxe_fill_rect(surf, 0, 0, surf->width, 44, PXE_COL_DARK_BLUE);
    pxe_draw_text(surf, 12, 6, "Pixel Neko OS Offline Installer (Ring 0)", PXE_COL_WHITE);
    char hdr_sub[96];
    snprintf(hdr_sub, sizeof(hdr_sub),
             "Installs system payload, APPS/*.pxe, PDEVICE/*.pdv & Limine Boot | CD: %c:\\",
             inst->sysinfo.cdrom_drive ? inst->sysinfo.cdrom_drive : '-');
    pxe_draw_text_small(surf, 12, 26, hdr_sub, PXE_COL_CYAN);

    /* Step 1: Partitions */
    pxe_draw_text_small(surf, 10, 52, "1. Target Fixed Disk Partition:", PXE_COL_BLACK);
    int busy = (inst->state == STATE_INSTALLING);
    pxe_draw_button(surf, 344, 48, 90, 20, "Init Disk", 0, !busy);
    pxe_draw_button(surf, 440, 48, 90, 20, "Refresh", 0, !busy);

    int lx = 10, ly = 72, lw = surf->width - 20 - SB_W, lh = 94;
    int vis = (lh - 4) / ROW_H;
    pxe_draw_bevel_box(surf, lx, ly, lw, lh, 1, PXE_COL_WHITE);

    if (inst->part_count == 0) {
        pxe_draw_text_small(surf, lx + 10, ly + 22,
                            "No writable fixed-disk partition found.", PXE_COL_DARK_RED);
        pxe_draw_text_small(surf, lx + 10, ly + 40,
                            "If the disk is blank/unpartitioned, click [Init Disk] above.",
                            PXE_COL_DARK_GRAY);
    } else {
        for (int i = 0; i < vis; i++) {
            int idx = inst->part_scroll + i;
            if (idx >= inst->part_count) break;
            const pxe_r0_part_info_t* p = &inst->parts[idx];
            int ry = ly + 2 + i * ROW_H;
            int sel = (idx == inst->sel_part);
            uint8_t fg = sel ? PXE_COL_WHITE : PXE_COL_BLACK;
            if (sel) {
                pxe_fill_rect(surf, lx + 2, ry, lw - 4, ROW_H, PXE_COL_DARK_BLUE);
            }
            char sz[16], line[96];
            fmt_sectors(p->total_sectors, sz, sizeof(sz));
            char drv_s[4];
            if (p->mounted_letter) snprintf(drv_s, sizeof(drv_s), "%c:", p->mounted_letter);
            else snprintf(drv_s, sizeof(drv_s), "--");
            snprintf(line, sizeof(line),
                     "P%d D%d %-10.10s  %-8.8s  %-7.7s  LBA:%-7llu %s %s",
                     p->part_index, p->disk_index,
                     p->disk_name[0] ? p->disk_name : "Disk",
                     sz,
                     p->fs_name[0] ? p->fs_name : "RAW",
                     (unsigned long long)p->start_lba,
                     drv_s,
                     p->label[0] ? p->label : "");
            int maxc = (lw - 10) / 6;
            if (maxc > 3 && (int)strlen(line) > maxc) line[maxc] = '\0';
            pxe_draw_text_small(surf, lx + 6, ry + 5, line, fg);
        }
    }
    pxe_draw_scrollbar(surf, lx + lw, ly, SB_W, lh, inst->part_scroll, vis, inst->part_count);

    /* Step 2: Filesystem & Label */
    pxe_draw_text_small(surf, 10, 178, "2. Format:", PXE_COL_BLACK);
    pxe_draw_button(surf, 80,  173, 64, 22,
                    inst->fs_choice == PART_FS_FAT32 ? "*FAT32" : "FAT32",
                    inst->fs_choice == PART_FS_FAT32, !busy);
    pxe_draw_button(surf, 148, 173, 64, 22,
                    inst->fs_choice == PART_FS_FAT16 ? "*FAT16" : "FAT16",
                    inst->fs_choice == PART_FS_FAT16, !busy);
    pxe_draw_button(surf, 216, 173, 92, 22,
                    inst->fs_choice == 0 ? "*Skip Format" : "Skip Format",
                    inst->fs_choice == 0, !busy);

    pxe_draw_text_small(surf, 322, 178, "Label:", PXE_COL_BLACK);
    pxe_draw_bevel_box(surf, 366, 173, 164, 22, 1, PXE_COL_WHITE);
    if (inst->label_focus && !busy) {
        pxe_draw_rect(surf, 367, 174, 162, 20, PXE_COL_BLUE);
    }
    pxe_draw_text(surf, 372, 176, inst->label, PXE_COL_BLACK);
    if (inst->label_focus && !busy) {
        pxe_draw_vline(surf, 372 + inst->label_len * 8, 176, 16, PXE_COL_BLACK);
    }

    /* Step 3: Progress & Log */
    pxe_draw_text_small(surf, 10, 206, inst->stage[0] ? inst->stage : "Ready to install.",
                        inst->state == STATE_ERROR ? PXE_COL_DARK_RED
                                                   : (inst->state == STATE_DONE ? PXE_COL_DARK_GREEN
                                                                                : PXE_COL_DARK_BLUE));
    char pct[16];
    snprintf(pct, sizeof(pct), "%3d%%", inst->progress);
    pxe_draw_text_small(surf, surf->width - 38, 206, pct, PXE_COL_BLACK);

    pxe_draw_bevel_box(surf, 10, 222, surf->width - 20, 18, 1, PXE_COL_WHITE);
    int bar_max = surf->width - 24;
    int bar_w = (inst->progress * bar_max) / 100;
    if (bar_w > bar_max) bar_w = bar_max;
    if (bar_w > 0) {
        uint8_t bcol = (inst->state == STATE_ERROR) ? PXE_COL_RED
                     : (inst->state == STATE_DONE ? PXE_COL_DARK_GREEN : PXE_COL_BLUE);
        pxe_fill_rect(surf, 12, 224, bar_w, 14, bcol);
    }
    if (inst->status_detail[0]) {
        pxe_draw_text_small(surf, 12, 244, inst->status_detail, PXE_COL_DARK_GRAY);
    }

    int log_y = 258, log_h = 98;
    pxe_draw_bevel_box(surf, 10, log_y, surf->width - 20, log_h, 1, PXE_COL_BLACK);
    int max_vis_logs = 7;
    int start_log = (inst->log_count > max_vis_logs) ? (inst->log_count - max_vis_logs) : 0;
    for (int i = start_log; i < inst->log_count; i++) {
        int ry = log_y + 4 + (i - start_log) * 13;
        pxe_draw_text_small(surf, 16, ry, inst->logs[i], PXE_COL_GREEN);
    }

    /* Bottom Buttons */
    if (inst->state == STATE_INSTALLING) {
        pxe_draw_text(surf, 14, 370, "Installing... Do not power off the machine!", PXE_COL_DARK_RED);
    } else if (inst->state == STATE_DONE) {
        pxe_draw_button(surf, 10,  364, 96,  26, "Close", 0, 1);
        pxe_draw_button(surf, 374, 364, 156, 26, "Reboot System", 0, 1);
    } else {
        pxe_draw_button(surf, 10,  364, 96,  26, "Exit", 0, 1);
        pxe_draw_button(surf, 374, 364, 156, 26, "Install OS", 0, inst->sel_part >= 0);
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
    int rc = run_installation(inst);
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
    strcpy(inst->stage, "Select a target partition and click Install OS.");

    refresh_partitions(inst);
    add_log(inst, "Installer ready. Ring 0 privileged disk/VFS APIs active.");

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 56;
    desc.y = 28;
    desc.w = 540;
    desc.h = 400;
    desc.min_w = 540;
    desc.min_h = 400;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_DEFAULT;
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
                    if (inst->state != STATE_INSTALLING) running = 0;
                    break;
                case PXE_EV_REDRAW:
                case PXE_EV_ACTIVATE:
                    dirty = 1;
                    break;
                case PXE_EV_MOUSE_DOWN: {
                    if (!(ev.buttons & PXE_BTN_LEFT)) break;
                    if (inst->state == STATE_INSTALLING) break;
                    int mx = ev.x, my = ev.y;

                    /* Init Disk / Refresh */
                    if (pxe_point_in_rect(mx, my, 344, 48, 90, 20)) {
                        int irc = pxe_r0_init_uninit_disk();
                        pxe_r0_vfs_remount_all();
                        refresh_partitions(inst);
                        char msg[80];
                        snprintf(msg, sizeof(msg), "Init Disk finished (rc=%d, parts=%d).",
                                 irc, inst->part_count);
                        add_log(inst, msg);
                        dirty = 1;
                        break;
                    }
                    if (pxe_point_in_rect(mx, my, 440, 48, 90, 20)) {
                        pxe_r0_vfs_remount_all();
                        refresh_partitions(inst);
                        add_log(inst, "Refreshed disk and partition list.");
                        dirty = 1;
                        break;
                    }

                    /* Partition list & scrollbar */
                    int lx = 10, ly = 72, lw = inst->surf.width - 20 - SB_W, lh = 94;
                    int vis = (lh - 4) / ROW_H;
                    int max_scr = inst->part_count - vis;
                    if (max_scr < 0) max_scr = 0;

                    if (pxe_point_in_rect(mx, my, lx + lw, ly, SB_W, lh)) {
                        if (my < ly + lh / 2) {
                            if (inst->part_scroll > 0) inst->part_scroll--;
                        } else {
                            if (inst->part_scroll < max_scr) inst->part_scroll++;
                        }
                        dirty = 1;
                        break;
                    }
                    if (pxe_point_in_rect(mx, my, lx, ly, lw, lh)) {
                        int row = (my - ly - 2) / ROW_H;
                        int idx = inst->part_scroll + row;
                        if (row >= 0 && idx >= 0 && idx < inst->part_count) {
                            inst->sel_part = idx;
                            sync_fs_choice_to_selection(inst);
                        }
                        inst->label_focus = 0;
                        dirty = 1;
                        break;
                    }

                    /* Format choice */
                    if (pxe_point_in_rect(mx, my, 80, 173, 64, 22)) {
                        inst->fs_choice = PART_FS_FAT32;
                        inst->label_focus = 0;
                        dirty = 1;
                        break;
                    }
                    if (pxe_point_in_rect(mx, my, 148, 173, 64, 22)) {
                        inst->fs_choice = PART_FS_FAT16;
                        inst->label_focus = 0;
                        dirty = 1;
                        break;
                    }
                    if (pxe_point_in_rect(mx, my, 216, 173, 92, 22)) {
                        inst->fs_choice = 0;
                        inst->label_focus = 0;
                        dirty = 1;
                        break;
                    }

                    /* Label input focus */
                    if (pxe_point_in_rect(mx, my, 366, 173, 164, 22)) {
                        inst->label_focus = 1;
                        dirty = 1;
                        break;
                    }
                    inst->label_focus = 0;

                    /* Bottom actions */
                    if (inst->state == STATE_DONE) {
                        if (pxe_point_in_rect(mx, my, 10, 364, 96, 26)) {
                            running = 0;
                        } else if (pxe_point_in_rect(mx, my, 374, 364, 156, 26)) {
                            pxe_r0_sync_filesystems();
                            pxe_r0_power_control(PXE_POWER_REBOOT);
                        }
                    } else {
                        if (pxe_point_in_rect(mx, my, 10, 364, 96, 26)) {
                            running = 0;
                        } else if (pxe_point_in_rect(mx, my, 374, 364, 156, 26) &&
                                   inst->sel_part >= 0) {
                            start_install(inst);
                            dirty = 1;
                        }
                    }
                    break;
                }
                case PXE_EV_KEY_DOWN:
                    if (inst->state == STATE_INSTALLING) break;
                    if (inst->label_focus) {
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
