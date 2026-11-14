#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/fs.h>
#include <pxe/mem.h>
#include <pxe/str.h>

#define MAX_FM_FILES 512
#define TOOLBAR_H    26
#define PATHBAR_H    18
#define COLHDR_H     18
#define HEADER_H     (TOOLBAR_H + PATHBAR_H + COLHDR_H)
#define STATUS_H     20
#define ROW_H        16
#define SB_W         14

#define MODAL_NONE        0
#define MODAL_RENAME      1
#define MODAL_MKDIR       2
#define MODAL_CONFIRM_DEL 3
#define MODAL_MSG         4

#define CTX_NONE          0
#define CTX_ITEM          1
#define CTX_BG            2
#define CTX_DRIVE         3

typedef struct {
    char     name[128];
    uint8_t  is_dir;
    uint8_t  is_parent;
    uint8_t  drv_type;
    uint8_t  is_readonly;
    uint8_t  is_free_known;
    uint64_t size;      /* bytes for files, total_kb for drives */
    uint64_t free_kb;   /* for drives */
    char     fs_name[16];
    char     label[32];
} fm_entry_t;

typedef struct {
    int        win_id;
    char       current_path[256];
    fm_entry_t entries[MAX_FM_FILES];
    int        entry_count;
    int        selected;
    int        scroll;
    int        sort_by_size;

    char       clip_path[256];
    char       clip_name[128];
    uint8_t    clip_is_dir;
    int        has_clip;

    uint64_t   last_click_ticks;
    int        last_click_idx;
    int        sb_dragging;
    int        sb_drag_start_y;
    int        sb_drag_start_off;

    int        ctx_kind;
    int        ctx_x;
    int        ctx_y;
    int        ctx_w;
    int        ctx_h;
    int        ctx_target;

    int        modal;
    int        modal_is_err;
    char       modal_title[32];
    char       modal_msg[96];
    char       modal_input[128];
    int        modal_len;
    char       modal_target_path[256];
} fm_t;

static const pxe_win_surface_t* g_prog_surf = NULL;
static int g_prog_win_id = -1;
static char g_prog_title[64];
static uint64_t g_prog_last_bytes = 0;

static void join_path(char* out, size_t cap, const char* base, const char* name) {
    size_t blen = strlen(base);
    if (blen > 0 && (base[blen - 1] == '\\' || base[blen - 1] == '/')) {
        snprintf(out, cap, "%s%s", base, name);
    } else {
        snprintf(out, cap, "%s\\%s", base, name);
    }
}

static void fmt_bytes(uint64_t bytes, char* out, size_t cap) {
    if (bytes < 1024ull) {
        snprintf(out, cap, "%u B", (unsigned)bytes);
    } else if (bytes < 1024ull * 1024ull) {
        snprintf(out, cap, "%u KB", (unsigned)(bytes / 1024ull));
    } else if (bytes < 1024ull * 1024ull * 1024ull) {
        snprintf(out, cap, "%u.%u MB",
                 (unsigned)(bytes / (1024ull * 1024ull)),
                 (unsigned)(((bytes % (1024ull * 1024ull)) * 10ull) / (1024ull * 1024ull)));
    } else {
        uint64_t gb_div = 1024ull * 1024ull * 1024ull;
        snprintf(out, cap, "%llu.%02llu GB",
                 (unsigned long long)(bytes / gb_div),
                 (unsigned long long)(((bytes % gb_div) * 100ull) / gb_div));
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

static void show_msg(fm_t* fm, const char* title, const char* msg, int is_err) {
    fm->modal = MODAL_MSG;
    fm->modal_is_err = is_err;
    snprintf(fm->modal_title, sizeof(fm->modal_title), "%s", title);
    snprintf(fm->modal_msg, sizeof(fm->modal_msg), "%s", msg);
    if (is_err) pxe_beep();
}

static int resolve_app_path(const char* app_filename, char* out, size_t cap) {
    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);
    pxe_dirent_t st;

    if (info.system_drive) {
        snprintf(out, cap, "%c:\\APPS\\%s", info.system_drive, app_filename);
        if (pxe_stat(out, &st) == 0 && !st.is_dir) return 0;
    }
    if (info.cdrom_drive) {
        snprintf(out, cap, "%c:\\APPS\\%s", info.cdrom_drive, app_filename);
        if (pxe_stat(out, &st) == 0 && !st.is_dir) return 0;
    }
    return -1;
}

static int drive_is_readonly(char letter) {
    pxe_drive_info_t drvs[26];
    int n = pxe_get_drives(drvs, 26);
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 32);
    for (int i = 0; i < n; i++) {
        char dl = drvs[i].letter;
        if (dl >= 'a' && dl <= 'z') dl = (char)(dl - 32);
        if (dl == letter) return drvs[i].is_readonly;
    }
    return 0;
}

static void sort_entries(fm_t* fm) {
    int start = 0;
    if (fm->entry_count > 0 && fm->entries[0].is_parent) start = 1;
    for (int i = start; i < fm->entry_count - 1; i++) {
        for (int j = i + 1; j < fm->entry_count; j++) {
            fm_entry_t* a = &fm->entries[i];
            fm_entry_t* b = &fm->entries[j];
            int swap = 0;
            if (a->is_dir != b->is_dir) {
                if (!a->is_dir && b->is_dir) swap = 1;
            } else if (fm->sort_by_size && !a->is_dir) {
                if (b->size > a->size) swap = 1;
                else if (b->size == a->size && strcasecmp(a->name, b->name) > 0) swap = 1;
            } else {
                if (strcasecmp(a->name, b->name) > 0) swap = 1;
            }
            if (swap) {
                fm_entry_t tmp = *a;
                *a = *b;
                *b = tmp;
            }
        }
    }
}

static void load_directory(fm_t* fm) {
    fm->entry_count = 0;
    fm->selected = -1;
    fm->scroll = 0;
    fm->ctx_kind = CTX_NONE;

    if (fm->current_path[0] == '\0') {
        pxe_drive_info_t drvs[26];
        int n = pxe_get_drives(drvs, 26);
        for (int i = 0; i < n && fm->entry_count < MAX_FM_FILES; i++) {
            if (!drvs[i].present) continue;
            fm_entry_t* e = &fm->entries[fm->entry_count++];
            memset(e, 0, sizeof(*e));
            snprintf(e->name, sizeof(e->name), "%c:\\", drvs[i].letter);
            e->is_dir = 1;
            e->drv_type = drvs[i].drv_type;
            e->is_readonly = drvs[i].is_readonly;
            e->is_free_known = drvs[i].is_free_known;
            e->size = drvs[i].total_kb;
            e->free_kb = drvs[i].free_kb;
            strncpy(e->fs_name, drvs[i].fs_name, sizeof(e->fs_name) - 1);
            strncpy(e->label, drvs[i].label, sizeof(e->label) - 1);
        }
        return;
    }

    /* Parent entry */
    fm_entry_t* up = &fm->entries[fm->entry_count++];
    memset(up, 0, sizeof(*up));
    strcpy(up->name, "..");
    up->is_dir = 1;
    up->is_parent = 1;

    pxe_dirent_t* raw = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 256);
    if (!raw) return;
    int n = pxe_listdir(fm->current_path, raw, 256);
    if (n < 0) {
        free(raw);
        show_msg(fm, "Read Error", "Cannot read drive or directory.", 1);
        if (strlen(fm->current_path) <= 3) {
            fm->current_path[0] = '\0';
            load_directory(fm);
        }
        return;
    }

    for (int i = 0; i < n && fm->entry_count < MAX_FM_FILES; i++) {
        if (!raw[i].name[0]) continue;
        if (strcmp(raw[i].name, ".") == 0 || strcmp(raw[i].name, "..") == 0) continue;
        fm_entry_t* e = &fm->entries[fm->entry_count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, raw[i].name, sizeof(e->name) - 1);
        e->is_dir = raw[i].is_dir;
        e->size = raw[i].size;
    }
    free(raw);
    sort_entries(fm);
}

static void go_up(fm_t* fm) {
    size_t len = strlen(fm->current_path);
    if (len == 0) return;
    if (len <= 3) {
        fm->current_path[0] = '\0';
    } else {
        while (len > 0 && (fm->current_path[len - 1] == '\\' || fm->current_path[len - 1] == '/')) {
            fm->current_path[--len] = '\0';
        }
        while (len > 0 && fm->current_path[len - 1] != '\\' && fm->current_path[len - 1] != '/') {
            len--;
        }
        if (len <= 3 && fm->current_path[1] == ':') {
            fm->current_path[2] = '\\';
            fm->current_path[3] = '\0';
        } else {
            fm->current_path[len] = '\0';
        }
    }
    load_directory(fm);
}

static void open_entry(fm_t* fm, int idx) {
    if (idx < 0 || idx >= fm->entry_count) return;
    const fm_entry_t* e = &fm->entries[idx];

    if (e->is_parent) {
        go_up(fm);
        return;
    }
    if (fm->current_path[0] == '\0') {
        snprintf(fm->current_path, sizeof(fm->current_path), "%s", e->name);
        load_directory(fm);
        return;
    }

    char full[256];
    join_path(full, sizeof(full), fm->current_path, e->name);
    if (e->is_dir) {
        snprintf(fm->current_path, sizeof(fm->current_path), "%s", full);
        load_directory(fm);
        return;
    }

    const char* ext = strrchr(e->name, '.');
    if (!ext) {
        show_msg(fm, "Open File", "No file association for this file.", 1);
        return;
    }

    if (strcasecmp(ext, ".pxe") == 0) {
        int pid = pxe_spawn(full, 0, NULL);
        if (pid < 0) {
            show_msg(fm, "PXE Launch Failed", "Invalid, corrupted, or blocked .pxe.", 1);
        }
        return;
    }

    if (strcasecmp(ext, ".pdv") == 0) {
        show_msg(fm, "Device Driver (.pdv)",
                 "Copy .pdv to SYSTEM:\\PDEVICE\\ or load in SysInfo.", 0);
        return;
    }

    const char* helper = NULL;
    if (strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".log") == 0 ||
        strcasecmp(ext, ".ini") == 0 || strcasecmp(ext, ".conf") == 0 ||
        strcasecmp(ext, ".cfg") == 0 || strcasecmp(ext, ".md") == 0 ||
        strcasecmp(ext, ".pxi") == 0 || strcasecmp(ext, ".c") == 0 ||
        strcasecmp(ext, ".h") == 0) {
        helper = "notepad.pxe";
    } else if (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".png") == 0 ||
               strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        helper = "viewer.pxe";
    } else if (strcasecmp(ext, ".wav") == 0 || strcasecmp(ext, ".mp3") == 0 ||
               strcasecmp(ext, ".flac") == 0) {
        helper = "player.pxe";
    }

    if (!helper) {
        show_msg(fm, "Open File", "No application associated with this type.", 1);
        return;
    }

    char app_path[128];
    if (resolve_app_path(helper, app_path, sizeof(app_path)) != 0) {
        show_msg(fm, "Missing Application", "Cannot find app in APPS\\ folder.", 1);
        return;
    }

    const char* argv[2] = { app_path, full };
    if (pxe_spawn(app_path, 2, argv) < 0) {
        show_msg(fm, "Launch Failed", "Could not start associated application.", 1);
    }
}

static void action_copy(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    if (fm->entries[idx].is_parent) return;
    join_path(fm->clip_path, sizeof(fm->clip_path), fm->current_path, fm->entries[idx].name);
    snprintf(fm->clip_name, sizeof(fm->clip_name), "%s", fm->entries[idx].name);
    fm->clip_is_dir = fm->entries[idx].is_dir;
    fm->has_clip = 1;
}

static void action_properties(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    const fm_entry_t* e = &fm->entries[idx];
    if (e->is_parent) return;
    if (e->is_dir) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Folder: %.28s", e->name);
        show_msg(fm, "Properties", msg, 0);
        return;
    }
    char hsz[24], msg[96];
    fmt_bytes(e->size, hsz, sizeof(hsz));
    snprintf(msg, sizeof(msg), "%.16s | %s (%llu B)",
             e->name, hsz, (unsigned long long)e->size);
    show_msg(fm, "File Properties", msg, 0);
}

static void copy_progress_cb(uint64_t copied, uint64_t total) {
    if (!g_prog_surf || g_prog_win_id < 0) return;
    uint64_t step = (total > 4ull * 1024ull * 1024ull) ? 524288ull : 32768ull;
    if (copied < total && copied - g_prog_last_bytes < step && copied > 0) return;
    g_prog_last_bytes = copied;

    const pxe_win_surface_t* s = g_prog_surf;
    int dw = 280, dh = 76;
    if (dw > s->width - 16) dw = s->width - 16;
    int dx = (s->width - dw) / 2;
    int dy = (s->height - dh) / 2;
    pxe_draw_bevel_box(s, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
    pxe_fill_rect(s, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
    pxe_draw_text_small(s, dx + 8, dy + 7, "Copying File...", PXE_COL_WHITE);
    pxe_draw_text_small(s, dx + 10, dy + 26, g_prog_title, PXE_COL_BLACK);

    int bar_w = dw - 20;
    pxe_draw_bevel_box(s, dx + 10, dy + 42, bar_w, 16, 1, PXE_COL_WHITE);
    int pct = (total > 0) ? (int)((copied * 100ull) / total) : 100;
    if (pct > 100) pct = 100;
    int fill = (pct * (bar_w - 4)) / 100;
    if (fill > 0) pxe_fill_rect(s, dx + 12, dy + 44, fill, 12, PXE_COL_BLUE);

    char cbuf[20], tbuf[20], msg[64];
    fmt_bytes(copied, cbuf, sizeof(cbuf));
    fmt_bytes(total, tbuf, sizeof(tbuf));
    snprintf(msg, sizeof(msg), "%d%% (%s / %s)", pct, cbuf, tbuf);
    pxe_draw_text_small(s, dx + 12, dy + 62, msg, PXE_COL_DARK_GRAY);
    pxe_win_present(g_prog_win_id);
}

static void action_paste(fm_t* fm, const pxe_win_surface_t* surf) {
    if (!fm->has_clip || fm->current_path[0] == '\0') return;
    if (fm->clip_is_dir) {
        show_msg(fm, "Paste Error", "Folder copy is not supported directly.", 1);
        return;
    }
    if (drive_is_readonly(fm->current_path[0])) {
        show_msg(fm, "Read-Only Drive", "Cannot paste onto a read-only drive.", 1);
        return;
    }

    char dst[256];
    join_path(dst, sizeof(dst), fm->current_path, fm->clip_name);
    if (strcasecmp(dst, fm->clip_path) == 0) {
        char alt[128];
        snprintf(alt, sizeof(alt), "COPY_%s", fm->clip_name);
        join_path(dst, sizeof(dst), fm->current_path, alt);
    }

    g_prog_surf = surf;
    g_prog_win_id = fm->win_id;
    snprintf(g_prog_title, sizeof(g_prog_title), "%s", fm->clip_name);
    g_prog_last_bytes = 0;

    int rc = pxe_copy_file(fm->clip_path, dst, copy_progress_cb);
    g_prog_surf = NULL;

    if (rc != 0) {
        show_msg(fm, "Paste Failed", "Copy error (disk full / >4GB FAT32 limit).", 1);
        return;
    }
    load_directory(fm);
}

static void action_begin_rename(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    if (fm->entries[idx].is_parent) return;
    if (drive_is_readonly(fm->current_path[0])) {
        show_msg(fm, "Read-Only Drive", "Cannot rename files on read-only drive.", 1);
        return;
    }
    fm->modal = MODAL_RENAME;
    snprintf(fm->modal_title, sizeof(fm->modal_title), "Rename");
    snprintf(fm->modal_msg, sizeof(fm->modal_msg), "New name:");
    snprintf(fm->modal_input, sizeof(fm->modal_input), "%s", fm->entries[idx].name);
    fm->modal_len = (int)strlen(fm->modal_input);
    join_path(fm->modal_target_path, sizeof(fm->modal_target_path),
              fm->current_path, fm->entries[idx].name);
}

static void action_begin_mkdir(fm_t* fm) {
    if (fm->current_path[0] == '\0') {
        show_msg(fm, "New Folder", "Open a writable drive first.", 1);
        return;
    }
    if (drive_is_readonly(fm->current_path[0])) {
        show_msg(fm, "Read-Only Drive", "Cannot create folder on read-only drive.", 1);
        return;
    }
    fm->modal = MODAL_MKDIR;
    snprintf(fm->modal_title, sizeof(fm->modal_title), "New Folder");
    snprintf(fm->modal_msg, sizeof(fm->modal_msg), "Folder name:");
    snprintf(fm->modal_input, sizeof(fm->modal_input), "NEW_DIR");
    fm->modal_len = (int)strlen(fm->modal_input);
}

static void action_begin_delete(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    if (fm->entries[idx].is_parent) return;
    if (drive_is_readonly(fm->current_path[0])) {
        show_msg(fm, "Read-Only Drive", "Cannot delete on read-only drive.", 1);
        return;
    }
    fm->modal = MODAL_CONFIRM_DEL;
    snprintf(fm->modal_title, sizeof(fm->modal_title), "Confirm Delete");
    snprintf(fm->modal_msg, sizeof(fm->modal_msg), "Delete \"%s\"?", fm->entries[idx].name);
    join_path(fm->modal_target_path, sizeof(fm->modal_target_path),
              fm->current_path, fm->entries[idx].name);
}

static void action_eject(fm_t* fm) {
    char letter = fm->current_path[0];
    if (!letter) {
        if (fm->selected >= 0 && fm->selected < fm->entry_count) {
            letter = fm->entries[fm->selected].name[0];
        }
    }
    if (!letter) {
        show_msg(fm, "Eject", "Select a floppy or optical drive.", 1);
        return;
    }
    if (pxe_eject_drive(letter) != 0) {
        show_msg(fm, "Eject Failed", "Drive does not support eject.", 1);
        return;
    }
    fm->current_path[0] = '\0';
    load_directory(fm);
}

static void submit_modal(fm_t* fm) {
    if (fm->modal == MODAL_RENAME) {
        if (fm->modal_len <= 0 || strchr(fm->modal_input, '\\') || strchr(fm->modal_input, '/')) {
            show_msg(fm, "Rename Failed", "Enter a valid filename without slashes.", 1);
            return;
        }
        char new_full[256];
        join_path(new_full, sizeof(new_full), fm->current_path, fm->modal_input);
        fm->modal = MODAL_NONE;
        if (pxe_rename(fm->modal_target_path, new_full) != 0) {
            show_msg(fm, "Rename Failed", "Name collision or I/O error.", 1);
            return;
        }
        load_directory(fm);
    } else if (fm->modal == MODAL_MKDIR) {
        if (fm->modal_len <= 0 || strchr(fm->modal_input, '\\') || strchr(fm->modal_input, '/')) {
            show_msg(fm, "New Folder Failed", "Enter a valid folder name.", 1);
            return;
        }
        char full[256];
        join_path(full, sizeof(full), fm->current_path, fm->modal_input);
        fm->modal = MODAL_NONE;
        if (pxe_mkdir(full) != 0) {
            show_msg(fm, "New Folder Failed", "Folder exists or disk full.", 1);
            return;
        }
        load_directory(fm);
    } else if (fm->modal == MODAL_CONFIRM_DEL) {
        fm->modal = MODAL_NONE;
        if (pxe_unlink(fm->modal_target_path) != 0) {
            show_msg(fm, "Delete Failed", "Folder non-empty or read-only.", 1);
            return;
        }
        load_directory(fm);
    } else {
        fm->modal = MODAL_NONE;
    }
}

static void render_fm(const pxe_win_surface_t* surf, fm_t* fm) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    /* 1. Toolbar */
    int bx = 4;
    pxe_draw_button(surf, bx, 3, 52, 20, "Drives", 0, 1); bx += 56;
    pxe_draw_button(surf, bx, 3, 44, 20, "^ Up", 0, fm->current_path[0] != 0); bx += 48;
    int can_open = (fm->selected >= 0 && fm->selected < fm->entry_count);
    pxe_draw_button(surf, bx, 3, 46, 20, "Open", 0, can_open); bx += 50;
    int can_copy = (fm->current_path[0] != 0 && can_open && !fm->entries[fm->selected].is_parent);
    pxe_draw_button(surf, bx, 3, 46, 20, "Copy", 0, can_copy); bx += 50;
    int can_paste = (fm->has_clip && fm->current_path[0] != 0);
    pxe_draw_button(surf, bx, 3, 52, 20, "Paste", 0, can_paste); bx += 56;
    pxe_draw_button(surf, bx, 3, 62, 20, "NewDir", 0, fm->current_path[0] != 0); bx += 66;
    pxe_draw_button(surf, bx, 3, 50, 20, "Eject", 0, 1);

    /* 2. Path bar */
    pxe_draw_bevel_box(surf, 4, 26, surf->width - 8, 17, 1, PXE_COL_WHITE);
    char pstr[264];
    snprintf(pstr, sizeof(pstr), "Path: %s",
             fm->current_path[0] ? fm->current_path : "Drives Root");
    int max_pc = (surf->width - 20) / 6;
    if (max_pc > 3 && (int)strlen(pstr) > max_pc) pstr[max_pc] = '\0';
    pxe_draw_text_small(surf, 8, 31, pstr, PXE_COL_BLACK);

    /* 3. Column header */
    int list_x = 4, list_y = HEADER_H, list_w = surf->width - 8 - SB_W;
    int list_h = surf->height - HEADER_H - STATUS_H;
    if (list_h < 20) list_h = 20;
    pxe_draw_bevel_box(surf, 4, 44, surf->width - 8, 17, 0, PXE_COL_LIGHT_GRAY);
    if (fm->current_path[0] == '\0') {
        pxe_draw_text_small(surf, 10, 49, "Drive", PXE_COL_BLACK);
        pxe_draw_text_small(surf, 76, 49, "FS", PXE_COL_BLACK);
        pxe_draw_text_small(surf, 152, 49, "Label", PXE_COL_BLACK);
        pxe_draw_text_small(surf, list_w - 134, 49, "Total", PXE_COL_BLACK);
        pxe_draw_text_small(surf, list_w - 64, 49, "Free", PXE_COL_BLACK);
    } else {
        pxe_draw_text_small(surf, 10, 49, "Name", PXE_COL_BLACK);
        pxe_draw_text_small(surf, list_w - 78, 49, "Type / Size", PXE_COL_BLACK);
    }

    /* 4. List background & entries */
    pxe_draw_bevel_box(surf, list_x, list_y, list_w, list_h, 1, PXE_COL_WHITE);
    int vis = (list_h - 4) / ROW_H;
    if (vis < 1) vis = 1;
    int max_scr = (fm->entry_count > vis) ? (fm->entry_count - vis) : 0;
    if (fm->scroll > max_scr) fm->scroll = max_scr;
    if (fm->scroll < 0) fm->scroll = 0;

    for (int i = 0; i < vis && (fm->scroll + i) < fm->entry_count; i++) {
        int idx = fm->scroll + i;
        const fm_entry_t* e = &fm->entries[idx];
        int ry = list_y + 2 + i * ROW_H;
        int sel = (idx == fm->selected);
        if (sel) {
            pxe_fill_rect(surf, list_x + 2, ry, list_w - 4, ROW_H, PXE_COL_BLUE);
        }
        uint32_t fg = sel ? PXE_COL_WHITE : (e->is_dir ? PXE_COL_DARK_BLUE : PXE_COL_BLACK);

        char dname[136];
        if (e->is_dir) {
            if (e->is_parent && strlen(fm->current_path) <= 3) {
                snprintf(dname, sizeof(dname), "[.. (Drives)]");
            } else {
                snprintf(dname, sizeof(dname), "[%s]", e->name);
            }
        } else {
            snprintf(dname, sizeof(dname), "%s", e->name);
        }
        int name_max = (fm->current_path[0] == '\0') ? 9 : ((list_w - 98) / 8);
        if (name_max > 3 && (int)strlen(dname) > name_max) {
            dname[name_max] = '\0';
            dname[name_max - 1] = '.';
            dname[name_max - 2] = '.';
        }
        pxe_draw_text(surf, list_x + 6, ry, dname, fg);

        if (fm->current_path[0] == '\0') {
            pxe_draw_text_small(surf, 76, ry + 5,
                                e->fs_name[0] ? e->fs_name : "DRIVE", fg);
            if (e->label[0]) {
                char lb[14];
                snprintf(lb, sizeof(lb), "%.12s", e->label);
                pxe_draw_text_small(surf, 152, ry + 5, lb, fg);
            }
            char ts[20], fs[20];
            fmt_kb(e->size, ts, sizeof(ts));
            pxe_draw_text_small(surf, list_w - 134, ry + 5, ts, fg);
            if (e->is_readonly) {
                pxe_draw_text_small(surf, list_w - 64, ry + 5, "RO", fg);
            } else if (e->is_free_known) {
                fmt_kb(e->free_kb, fs, sizeof(fs));
                pxe_draw_text_small(surf, list_w - 64, ry + 5, fs, fg);
            } else {
                pxe_draw_text_small(surf, list_w - 64, ry + 5, "--", fg);
            }
        } else if (e->is_dir) {
            pxe_draw_text_small(surf, list_w - 78, ry + 5, "<DIR>", fg);
        } else {
            char sz[24];
            fmt_bytes(e->size, sz, sizeof(sz));
            pxe_draw_text_small(surf, list_w - 78, ry + 5, sz, fg);
        }
    }

    /* 5. Vertical scrollbar */
    pxe_draw_scrollbar(surf, list_x + list_w, list_y, SB_W, list_h,
                       fm->scroll, vis, fm->entry_count);

    /* 6. Status bar */
    int sby = surf->height - STATUS_H;
    pxe_draw_hline(surf, 0, sby, surf->width, PXE_COL_DARK_GRAY);
    char sbuf[128];
    if (fm->has_clip) {
        snprintf(sbuf, sizeof(sbuf), "Clipboard: %s | Items: %d",
                 fm->clip_name, fm->entry_count);
    } else if (fm->current_path[0] == '\0') {
        snprintf(sbuf, sizeof(sbuf), "Drives: %d | Double-click to open | Eject unmounts CD/FD",
                 fm->entry_count);
    } else {
        snprintf(sbuf, sizeof(sbuf), "Items: %d | Right-click for Copy/Paste/Rename/Delete",
                 fm->entry_count > 0 ? fm->entry_count - 1 : 0);
    }
    int max_sc = (surf->width - 14) / 6;
    if (max_sc > 3 && (int)strlen(sbuf) > max_sc) sbuf[max_sc] = '\0';
    pxe_draw_text_small(surf, 6, sby + 6, sbuf, PXE_COL_BLACK);

    /* 7. Context Menu */
    if (fm->ctx_kind != CTX_NONE) {
        pxe_draw_bevel_box(surf, fm->ctx_x, fm->ctx_y, fm->ctx_w, fm->ctx_h, 0, PXE_COL_LIGHT_GRAY);
        if (fm->ctx_kind == CTX_ITEM) {
            const char* items[7] = {
                "Open", "Copy", "Paste", "Rename...", "Delete", "New Folder...", "Properties..."
            };
            int en[7] = { 1, 1, fm->has_clip, 1, 1, 1, 1 };
            for (int i = 0; i < 7; i++) {
                pxe_draw_text_small(surf, fm->ctx_x + 10, fm->ctx_y + 5 + i * 18,
                                    items[i], en[i] ? PXE_COL_BLACK : PXE_COL_DARK_GRAY);
            }
        } else if (fm->ctx_kind == CTX_BG) {
            const char* items[4] = { "Paste", "New Folder...", "Refresh", "Drives" };
            int en[4] = { fm->has_clip, 1, 1, 1 };
            for (int i = 0; i < 4; i++) {
                pxe_draw_text_small(surf, fm->ctx_x + 10, fm->ctx_y + 5 + i * 18,
                                    items[i], en[i] ? PXE_COL_BLACK : PXE_COL_DARK_GRAY);
            }
        } else if (fm->ctx_kind == CTX_DRIVE) {
            pxe_draw_text_small(surf, fm->ctx_x + 10, fm->ctx_y + 5, "Open", PXE_COL_BLACK);
            pxe_draw_text_small(surf, fm->ctx_x + 10, fm->ctx_y + 23, "Eject", PXE_COL_BLACK);
        }
    }

    /* 8. Modal Dialogs */
    if (fm->modal == MODAL_RENAME || fm->modal == MODAL_MKDIR) {
        int dw = 260, dh = 96;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3, fm->modal_title, PXE_COL_WHITE);
        pxe_draw_text_small(surf, dx + 10, dy + 25, fm->modal_msg, PXE_COL_BLACK);
        pxe_draw_bevel_box(surf, dx + 10, dy + 36, dw - 20, 22, 1, PXE_COL_WHITE);
        int max_c = (dw - 28) / 8;
        int start = (fm->modal_len > max_c) ? (fm->modal_len - max_c) : 0;
        pxe_draw_text(surf, dx + 14, dy + 39, fm->modal_input + start, PXE_COL_BLACK);
        pxe_draw_vline(surf, dx + 14 + (fm->modal_len - start) * 8, dy + 39, 16, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + 46,  dy + 66, 72, 22, "OK", 0, 1);
        pxe_draw_button(surf, dx + 142, dy + 66, 72, 22, "Cancel", 0, 1);
    } else if (fm->modal == MODAL_CONFIRM_DEL) {
        int dw = 260, dh = 86;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_RED);
        pxe_draw_text(surf, dx + 8, dy + 3, fm->modal_title, PXE_COL_WHITE);
        pxe_draw_text_small(surf, dx + 12, dy + 32, fm->modal_msg, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + 46,  dy + 54, 72, 22, "Delete", 0, 1);
        pxe_draw_button(surf, dx + 142, dy + 54, 72, 22, "Cancel", 0, 1);
    } else if (fm->modal == MODAL_MSG) {
        int dw = 300, dh = 88;
        if (dw > surf->width - 12) dw = surf->width - 12;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 12, dy + 12, 22, 22,
                      fm->modal_is_err ? PXE_COL_DARK_RED : PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 19, dy + 15, fm->modal_is_err ? "!" : "i", PXE_COL_WHITE);
        pxe_draw_text(surf, dx + 42, dy + 13, fm->modal_title,
                      fm->modal_is_err ? PXE_COL_DARK_RED : PXE_COL_DARK_BLUE);
        pxe_draw_text_small(surf, dx + 42, dy + 34, fm->modal_msg, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + dw / 2 - 32, dy + 56, 64, 22, "OK", 0, 1);
    }
}

static void clamp_ctx_menu(fm_t* fm, const pxe_win_surface_t* surf, int mx, int my, int items) {
    fm->ctx_w = 112;
    fm->ctx_h = items * 18 + 4;
    fm->ctx_x = mx;
    fm->ctx_y = my;
    if (fm->ctx_x + fm->ctx_w > surf->width - 4) fm->ctx_x = surf->width - 4 - fm->ctx_w;
    if (fm->ctx_y + fm->ctx_h > surf->height - 4) fm->ctx_y = surf->height - 4 - fm->ctx_h;
    if (fm->ctx_x < 4) fm->ctx_x = 4;
    if (fm->ctx_y < 4) fm->ctx_y = 4;
}

int main(int argc, char** argv) {
    fm_t* fm = (fm_t*)calloc(1, sizeof(fm_t));
    if (!fm) return 1;
    fm->selected = -1;
    fm->last_click_idx = -1;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 64;
    desc.y = 40;
    desc.w = 500;
    desc.h = 340;
    desc.min_w = 380;
    desc.min_h = 220;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_RESIZABLE;
    strcpy(desc.title, "File Manager");

    pxe_win_surface_t surf;
    fm->win_id = pxe_win_create(&desc, &surf);
    if (fm->win_id < 0) {
        free(fm);
        return 1;
    }

    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 2;
    strcpy(mb.menus[0].title, "File");
    mb.menus[0].item_count = 8;
    strcpy(mb.menus[0].items[0].label, "Open");          mb.menus[0].items[0].enabled = 1;
    strcpy(mb.menus[0].items[1].label, "Copy");          mb.menus[0].items[1].enabled = 1;
    strcpy(mb.menus[0].items[2].label, "Paste");         mb.menus[0].items[2].enabled = 1;
    strcpy(mb.menus[0].items[3].label, "Rename...");     mb.menus[0].items[3].enabled = 1;
    strcpy(mb.menus[0].items[4].label, "New Folder..."); mb.menus[0].items[4].enabled = 1;
    strcpy(mb.menus[0].items[5].label, "Delete");        mb.menus[0].items[5].enabled = 1;
    strcpy(mb.menus[0].items[6].label, "Eject");         mb.menus[0].items[6].enabled = 1;
    strcpy(mb.menus[0].items[7].label, "Exit");          mb.menus[0].items[7].enabled = 1;

    strcpy(mb.menus[1].title, "View");
    mb.menus[1].item_count = 4;
    strcpy(mb.menus[1].items[0].label, "Refresh");      mb.menus[1].items[0].enabled = 1;
    strcpy(mb.menus[1].items[1].label, "Sort by Name"); mb.menus[1].items[1].enabled = 1;
    strcpy(mb.menus[1].items[2].label, "Sort by Size"); mb.menus[1].items[2].enabled = 1;
    strcpy(mb.menus[1].items[3].label, "Drives Root");  mb.menus[1].items[3].enabled = 1;
    pxe_win_set_menu(fm->win_id, &mb);

    if (argc > 1 && argv[1] && argv[1][0]) {
        snprintf(fm->current_path, sizeof(fm->current_path), "%s", argv[1]);
    }
    load_directory(fm);

    render_fm(&surf, fm);
    pxe_win_present(fm->win_id);

    int running = 1;
    while (running) {
        pxe_event_t ev;
        int rc = pxe_win_wait_event(fm->win_id, &ev);
        if (rc < 0) break;
        if (rc == 0) continue;

        int dirty = 0;
        int list_x = 4, list_y = HEADER_H, list_w = surf.width - 8 - SB_W;
        int list_h = surf.height - HEADER_H - STATUS_H;
        if (list_h < 20) list_h = 20;
        int vis = (list_h - 4) / ROW_H;
        if (vis < 1) vis = 1;
        int max_scr = (fm->entry_count > vis) ? (fm->entry_count - vis) : 0;

        switch (ev.type) {
            case PXE_EV_CLOSE:
                running = 0;
                break;
            case PXE_EV_RESIZE:
                if (pxe_win_get_surface(fm->win_id, &surf) == 0) dirty = 1;
                break;
            case PXE_EV_REDRAW:
                dirty = 1;
                break;
            case PXE_EV_ACTIVATE:
                if (ev.text[0]) {
                    snprintf(fm->current_path, sizeof(fm->current_path), "%s", ev.text);
                    load_directory(fm);
                } else if (fm->current_path[0] == '\0') {
                    load_directory(fm);
                }
                dirty = 1;
                break;
            case PXE_EV_MENU:
                fm->ctx_kind = CTX_NONE;
                if (ev.arg0 == 0) {
                    if (ev.arg1 == 0 && fm->selected >= 0) open_entry(fm, fm->selected);
                    else if (ev.arg1 == 1 && fm->selected >= 0) action_copy(fm, fm->selected);
                    else if (ev.arg1 == 2) action_paste(fm, &surf);
                    else if (ev.arg1 == 3 && fm->selected >= 0) action_begin_rename(fm, fm->selected);
                    else if (ev.arg1 == 4) action_begin_mkdir(fm);
                    else if (ev.arg1 == 5 && fm->selected >= 0) action_begin_delete(fm, fm->selected);
                    else if (ev.arg1 == 6) action_eject(fm);
                    else if (ev.arg1 == 7) running = 0;
                } else if (ev.arg0 == 1) {
                    if (ev.arg1 == 0) load_directory(fm);
                    else if (ev.arg1 == 1) { fm->sort_by_size = 0; sort_entries(fm); }
                    else if (ev.arg1 == 2) { fm->sort_by_size = 1; sort_entries(fm); }
                    else if (ev.arg1 == 3) { fm->current_path[0] = '\0'; load_directory(fm); }
                }
                dirty = 1;
                break;
            case PXE_EV_MOUSE_UP:
                fm->sb_dragging = 0;
                break;
            case PXE_EV_MOUSE_MOVE:
                if (fm->sb_dragging && (ev.buttons & PXE_BTN_LEFT) && max_scr > 0) {
                    int track_h = list_h - 28;
                    if (track_h > 12) {
                        int dy = ev.y - fm->sb_drag_start_y;
                        int new_off = fm->sb_drag_start_off + (dy * max_scr) / (track_h - 12);
                        if (new_off < 0) new_off = 0;
                        if (new_off > max_scr) new_off = max_scr;
                        if (new_off != fm->scroll) {
                            fm->scroll = new_off;
                            dirty = 1;
                        }
                    }
                }
                break;
            case PXE_EV_MOUSE_DOWN: {
                int mx = ev.x, my = ev.y;
                if (fm->modal != MODAL_NONE) {
                    if (!(ev.buttons & PXE_BTN_LEFT)) break;
                    if (fm->modal == MODAL_RENAME || fm->modal == MODAL_MKDIR) {
                        int dx = (surf.width - 260) / 2;
                        int dy = (surf.height - 96) / 2;
                        if (pxe_point_in_rect(mx, my, dx + 46, dy + 66, 72, 22)) submit_modal(fm);
                        else if (pxe_point_in_rect(mx, my, dx + 142, dy + 66, 72, 22)) fm->modal = MODAL_NONE;
                    } else if (fm->modal == MODAL_CONFIRM_DEL) {
                        int dx = (surf.width - 260) / 2;
                        int dy = (surf.height - 86) / 2;
                        if (pxe_point_in_rect(mx, my, dx + 46, dy + 54, 72, 22)) submit_modal(fm);
                        else if (pxe_point_in_rect(mx, my, dx + 142, dy + 54, 72, 22)) fm->modal = MODAL_NONE;
                    } else if (fm->modal == MODAL_MSG) {
                        int dw = (surf.width - 12 < 300) ? surf.width - 12 : 300;
                        int dx = (surf.width - dw) / 2;
                        int dy = (surf.height - 88) / 2;
                        if (pxe_point_in_rect(mx, my, dx + dw / 2 - 32, dy + 56, 64, 22)) {
                            fm->modal = MODAL_NONE;
                        }
                    }
                    dirty = 1;
                    break;
                }

                if (fm->ctx_kind != CTX_NONE) {
                    int kind = fm->ctx_kind;
                    int tgt = fm->ctx_target;
                    int inside = pxe_point_in_rect(mx, my, fm->ctx_x, fm->ctx_y, fm->ctx_w, fm->ctx_h);
                    int item = inside ? (my - fm->ctx_y - 2) / 18 : -1;
                    fm->ctx_kind = CTX_NONE;
                    if (inside && (ev.buttons & PXE_BTN_LEFT)) {
                        if (kind == CTX_ITEM) {
                            if (item == 0) open_entry(fm, tgt);
                            else if (item == 1) action_copy(fm, tgt);
                            else if (item == 2) action_paste(fm, &surf);
                            else if (item == 3) action_begin_rename(fm, tgt);
                            else if (item == 4) action_begin_delete(fm, tgt);
                            else if (item == 5) action_begin_mkdir(fm);
                            else if (item == 6) action_properties(fm, tgt);
                        } else if (kind == CTX_BG) {
                            if (item == 0) action_paste(fm, &surf);
                            else if (item == 1) action_begin_mkdir(fm);
                            else if (item == 2) load_directory(fm);
                            else if (item == 3) { fm->current_path[0] = '\0'; load_directory(fm); }
                        } else if (kind == CTX_DRIVE) {
                            if (item == 0) open_entry(fm, tgt);
                            else if (item == 1) action_eject(fm);
                        }
                    }
                    dirty = 1;
                    break;
                }

                /* Toolbar left clicks */
                if ((ev.buttons & PXE_BTN_LEFT) && my >= 3 && my < 23) {
                    if (pxe_point_in_rect(mx, my, 4, 3, 52, 20)) {
                        fm->current_path[0] = '\0';
                        load_directory(fm);
                    } else if (pxe_point_in_rect(mx, my, 60, 3, 44, 20)) {
                        go_up(fm);
                    } else if (pxe_point_in_rect(mx, my, 108, 3, 46, 20) && fm->selected >= 0) {
                        open_entry(fm, fm->selected);
                    } else if (pxe_point_in_rect(mx, my, 158, 3, 46, 20) && fm->selected >= 0) {
                        action_copy(fm, fm->selected);
                    } else if (pxe_point_in_rect(mx, my, 208, 3, 52, 20)) {
                        action_paste(fm, &surf);
                    } else if (pxe_point_in_rect(mx, my, 264, 3, 62, 20)) {
                        action_begin_mkdir(fm);
                    } else if (pxe_point_in_rect(mx, my, 330, 3, 50, 20)) {
                        action_eject(fm);
                    }
                    dirty = 1;
                    break;
                }

                /* Scrollbar */
                int sb_x = list_x + list_w;
                if ((ev.buttons & PXE_BTN_LEFT) &&
                    pxe_point_in_rect(mx, my, sb_x, list_y, SB_W, list_h)) {
                    if (my < list_y + 14) {
                        if (fm->scroll > 0) fm->scroll--;
                    } else if (my >= list_y + list_h - 14) {
                        if (fm->scroll < max_scr) fm->scroll++;
                    } else {
                        fm->sb_dragging = 1;
                        fm->sb_drag_start_y = my;
                        fm->sb_drag_start_off = fm->scroll;
                    }
                    dirty = 1;
                    break;
                }

                /* File list click / double-click / right-click */
                if (pxe_point_in_rect(mx, my, list_x, list_y, list_w, list_h)) {
                    int row = (my - list_y - 2) / ROW_H;
                    int idx = fm->scroll + row;
                    int valid = (row >= 0 && idx >= 0 && idx < fm->entry_count);

                    if (ev.buttons & PXE_BTN_RIGHT) {
                        if (valid) {
                            fm->selected = idx;
                            fm->ctx_target = idx;
                            if (fm->current_path[0] == '\0') {
                                fm->ctx_kind = CTX_DRIVE;
                                clamp_ctx_menu(fm, &surf, mx, my, 2);
                            } else if (!fm->entries[idx].is_parent) {
                                fm->ctx_kind = CTX_ITEM;
                                clamp_ctx_menu(fm, &surf, mx, my, 7);
                            } else if (fm->current_path[0] != '\0') {
                                fm->ctx_kind = CTX_BG;
                                clamp_ctx_menu(fm, &surf, mx, my, 4);
                            }
                        } else if (fm->current_path[0] != '\0') {
                            fm->ctx_kind = CTX_BG;
                            clamp_ctx_menu(fm, &surf, mx, my, 4);
                        }
                        dirty = 1;
                        break;
                    }

                    if (ev.buttons & PXE_BTN_LEFT) {
                        if (valid) {
                            uint64_t now = pxe_get_ticks();
                            if (idx == fm->last_click_idx && (now - fm->last_click_ticks) < 45) {
                                fm->last_click_idx = -1;
                                open_entry(fm, idx);
                            } else {
                                fm->selected = idx;
                                fm->last_click_idx = idx;
                                fm->last_click_ticks = now;
                            }
                        } else {
                            fm->selected = -1;
                        }
                        dirty = 1;
                    }
                }
                break;
            }
            case PXE_EV_KEY_DOWN:
                if (fm->modal == MODAL_RENAME || fm->modal == MODAL_MKDIR) {
                    if (ev.key == PXE_KEY_ESC) {
                        fm->modal = MODAL_NONE;
                    } else if (ev.key == PXE_KEY_ENTER) {
                        submit_modal(fm);
                    } else if (ev.key == PXE_KEY_BACKSPACE) {
                        if (fm->modal_len > 0) fm->modal_input[--fm->modal_len] = '\0';
                    } else if (ev.key >= 32 && ev.key <= 126 && fm->modal_len < 120) {
                        fm->modal_input[fm->modal_len++] = (char)ev.key;
                        fm->modal_input[fm->modal_len] = '\0';
                    }
                    dirty = 1;
                    break;
                }
                if (fm->modal != MODAL_NONE) {
                    if (ev.key == PXE_KEY_ENTER) submit_modal(fm);
                    else if (ev.key == PXE_KEY_ESC || ev.key == ' ') fm->modal = MODAL_NONE;
                    dirty = 1;
                    break;
                }

                if (ev.modifiers & PXE_MOD_CTRL) {
                    if ((ev.key == 'c' || ev.key == 'C') && fm->selected >= 0) {
                        action_copy(fm, fm->selected);
                        dirty = 1;
                    } else if (ev.key == 'v' || ev.key == 'V') {
                        action_paste(fm, &surf);
                        dirty = 1;
                    } else if (ev.key == 'r' || ev.key == 'R') {
                        load_directory(fm);
                        dirty = 1;
                    } else if (ev.key == 'n' || ev.key == 'N') {
                        action_begin_mkdir(fm);
                        dirty = 1;
                    }
                    break;
                }

                if (ev.key == PXE_KEY_ESC) {
                    if (fm->ctx_kind != CTX_NONE) {
                        fm->ctx_kind = CTX_NONE;
                        dirty = 1;
                    }
                } else if (ev.key == PXE_KEY_UP && fm->entry_count > 0) {
                    if (fm->selected <= 0) fm->selected = 0;
                    else fm->selected--;
                    if (fm->selected < fm->scroll) fm->scroll = fm->selected;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_DOWN && fm->entry_count > 0) {
                    if (fm->selected < 0) fm->selected = 0;
                    else if (fm->selected < fm->entry_count - 1) fm->selected++;
                    if (fm->selected >= fm->scroll + vis) fm->scroll = fm->selected - vis + 1;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_PGUP && fm->entry_count > 0) {
                    fm->scroll -= vis;
                    if (fm->scroll < 0) fm->scroll = 0;
                    if (fm->selected >= 0) {
                        fm->selected -= vis;
                        if (fm->selected < 0) fm->selected = 0;
                    }
                    dirty = 1;
                } else if (ev.key == PXE_KEY_PGDN && fm->entry_count > 0) {
                    fm->scroll += vis;
                    if (fm->scroll > max_scr) fm->scroll = max_scr;
                    if (fm->selected >= 0) {
                        fm->selected += vis;
                        if (fm->selected >= fm->entry_count) fm->selected = fm->entry_count - 1;
                    }
                    dirty = 1;
                } else if (ev.key == PXE_KEY_HOME && fm->entry_count > 0) {
                    fm->selected = 0;
                    fm->scroll = 0;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_END && fm->entry_count > 0) {
                    fm->selected = fm->entry_count - 1;
                    fm->scroll = max_scr;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_ENTER && fm->selected >= 0) {
                    open_entry(fm, fm->selected);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_BACKSPACE) {
                    go_up(fm);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_DELETE && fm->selected >= 0) {
                    action_begin_delete(fm, fm->selected);
                    dirty = 1;
                } else if (ev.key == 'e' || ev.key == 'E') {
                    action_eject(fm);
                    dirty = 1;
                }
                break;
            default:
                break;
        }

        if (dirty && running) {
            render_fm(&surf, fm);
            pxe_win_present(fm->win_id);
        }
    }

    pxe_win_destroy(fm->win_id);
    free(fm);
    return 0;
}
