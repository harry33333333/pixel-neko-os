#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/fs.h>
#include <pxe/mem.h>
#include <pxe/str.h>
#include <pxe/format.h>

#define MAX_FM_FILES 512
#define MAX_OW_APPS  32
#define OW_VISIBLE_ROWS 6
#define TOOLBAR_H    26
#define PATHBAR_H    18
#define COLHDR_H     18
#define HEADER_H     (TOOLBAR_H + PATHBAR_H + COLHDR_H)
#define STATUS_H     20
#define ROW_H        18
#define SB_W         14

#define MODAL_NONE        0
#define MODAL_RENAME      1
#define MODAL_MKDIR       2
#define MODAL_CONFIRM_DEL 3
#define MODAL_MSG         4
#define MODAL_OPEN_WITH   5

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
    char filename[64];
    char path[128];
    char title[48];
} fm_ow_app_t;

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
    uint8_t    clip_is_cut;
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
    char       modal_title[48];
    char       modal_msg[128];
    char       modal_input[128];
    int        modal_len;
    char       modal_target_path[256];
    fm_ow_app_t ow_apps[MAX_OW_APPS];
    int        ow_app_count;
    int        ow_sel;
    int        ow_scroll;
    int        ow_save_default;
    int        ow_can_save;
    char       ow_ext[32];
    int        zh;
} fm_t;

#define FM_TR(fm, z, e) ((fm)->zh ? (z) : (e))

static void sync_fm_menu(fm_t* fm) {
    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 2;
    strcpy(mb.menus[0].title, FM_TR(fm, "文件", "File"));
    mb.menus[0].item_count = 9;
    strcpy(mb.menus[0].items[0].label, FM_TR(fm, "打开", "Open"));             mb.menus[0].items[0].enabled = 1;
    strcpy(mb.menus[0].items[1].label, FM_TR(fm, "剪切", "Cut"));              mb.menus[0].items[1].enabled = 1;
    strcpy(mb.menus[0].items[2].label, FM_TR(fm, "复制", "Copy"));             mb.menus[0].items[2].enabled = 1;
    strcpy(mb.menus[0].items[3].label, FM_TR(fm, "粘贴", "Paste"));            mb.menus[0].items[3].enabled = 1;
    strcpy(mb.menus[0].items[4].label, FM_TR(fm, "重命名...", "Rename..."));   mb.menus[0].items[4].enabled = 1;
    strcpy(mb.menus[0].items[5].label, FM_TR(fm, "新建目录...", "New Folder...")); mb.menus[0].items[5].enabled = 1;
    strcpy(mb.menus[0].items[6].label, FM_TR(fm, "删除", "Delete"));           mb.menus[0].items[6].enabled = 1;
    strcpy(mb.menus[0].items[7].label, FM_TR(fm, "弹出", "Eject"));            mb.menus[0].items[7].enabled = 1;
    strcpy(mb.menus[0].items[8].label, FM_TR(fm, "退出", "Exit"));             mb.menus[0].items[8].enabled = 1;

    strcpy(mb.menus[1].title, FM_TR(fm, "视图", "View"));
    mb.menus[1].item_count = 4;
    strcpy(mb.menus[1].items[0].label, FM_TR(fm, "刷新", "Refresh"));          mb.menus[1].items[0].enabled = 1;
    strcpy(mb.menus[1].items[1].label, FM_TR(fm, "按名称排序", "Sort by Name")); mb.menus[1].items[1].enabled = 1;
    strcpy(mb.menus[1].items[2].label, FM_TR(fm, "按大小排序", "Sort by Size")); mb.menus[1].items[2].enabled = 1;
    strcpy(mb.menus[1].items[3].label, FM_TR(fm, "磁盘列表", "Drives Root"));  mb.menus[1].items[3].enabled = 1;
    pxe_win_set_menu(fm->win_id, &mb);
}

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

static int text_contains_ci(const char* text, const char* needle) {
    if (!text || !needle || !*needle) return 0;
    size_t needle_len = strlen(needle);
    for (const char* cursor = text; *cursor; cursor++) {
        size_t index = 0;
        while (index < needle_len && cursor[index]) {
            char left = cursor[index];
            char right = needle[index];
            if (left >= 'A' && left <= 'Z') left = (char)(left + 32);
            if (right >= 'A' && right <= 'Z') right = (char)(right + 32);
            if (left != right) break;
            index++;
        }
        if (index == needle_len) return 1;
    }
    return 0;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
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

static void extract_ext_no_dot(const char* name, char* out, size_t cap) {
    out[0] = '\0';
    if (!name || cap == 0) return;
    const char* slash = strrchr(name, '\\');
    const char* slash2 = strrchr(name, '/');
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;
    const char* base = slash ? (slash + 1) : name;
    const char* dot = strrchr(base, '.');
    if (!dot || !dot[1]) return;
    size_t i = 0;
    dot++;
    while (dot[i] && i + 1 < cap) {
        out[i] = ascii_lower(dot[i]);
        i++;
    }
    out[i] = '\0';
}

static int read_pxe_meta(const char* path, uint8_t* out_subsys,
                         char* out_title, size_t title_cap,
                         char* out_assoc, size_t assoc_cap) {
    if (out_subsys) *out_subsys = 0;
    if (out_title && title_cap > 0) out_title[0] = '\0';
    if (out_assoc && assoc_cap > 0) out_assoc[0] = '\0';

    int fd = pxe_open(path);
    if (fd < 0) return -1;

    pxe_header_t hdr;
    int r = pxe_read(fd, &hdr, sizeof(hdr));
    if (r != (int)sizeof(hdr) || hdr.magic != PXE_MAGIC) {
        pxe_close(fd);
        return -1;
    }
    if (out_subsys) *out_subsys = hdr.subsystem;

    pxe_resource_t rsrc;
    memset(&rsrc, 0, sizeof(rsrc));
    if (hdr.rsrc_size >= sizeof(rsrc) && hdr.rsrc_offset >= sizeof(hdr)) {
        if (pxe_seek(fd, hdr.rsrc_offset) == 0) {
            (void)pxe_read(fd, &rsrc, sizeof(rsrc));
        }
    }
    pxe_close(fd);

    if (out_title && title_cap > 0) {
        rsrc.title[sizeof(rsrc.title) - 1] = '\0';
        hdr.app_name[sizeof(hdr.app_name) - 1] = '\0';
        const char* src = rsrc.title[0] ? rsrc.title : hdr.app_name;
        snprintf(out_title, title_cap, "%s", src);
    }
    if (out_assoc && assoc_cap > 0) {
        rsrc.ext_assoc[sizeof(rsrc.ext_assoc) - 1] = '\0';
        snprintf(out_assoc, assoc_cap, "%s", rsrc.ext_assoc);
    }
    return 0;
}

static int ext_list_contains(const char* comma_list, const char* ext_no_dot) {
    if (!comma_list || !ext_no_dot || !ext_no_dot[0]) return 0;
    const char* p = comma_list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '.') p++;
        if (!*p) break;
        char tok[32];
        size_t ti = 0;
        while (*p && *p != ',' && ti + 1 < sizeof(tok)) {
            if (*p != ' ' && *p != '\t') tok[ti++] = *p;
            p++;
        }
        tok[ti] = '\0';
        while (*p && *p != ',') p++;
        if (tok[0] && strcasecmp(tok, ext_no_dot) == 0) return 1;
    }
    return 0;
}

static int find_assoc_ini_path(char* out, size_t cap, int for_write) {
    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);
    pxe_dirent_t st;

    if (for_write) {
        if (!info.settings_writable || !info.system_drive ||
            drive_is_readonly(info.system_drive)) {
            return -1;
        }
        snprintf(out, cap, "%c:\\system\\assoc.ini", info.system_drive);
        return 0;
    }
    if (info.system_drive) {
        snprintf(out, cap, "%c:\\system\\assoc.ini", info.system_drive);
        if (pxe_stat(out, &st) == 0 && !st.is_dir) return 0;
    }
    if (info.cdrom_drive && info.cdrom_drive != info.system_drive) {
        snprintf(out, cap, "%c:\\system\\assoc.ini", info.cdrom_drive);
        if (pxe_stat(out, &st) == 0 && !st.is_dir) return 0;
    }
    return -1;
}

static int parse_ini_kv(const char* line, size_t len, char* key_out, size_t key_cap,
                        char* val_out, size_t val_cap) {
    size_t s = 0;
    while (s < len && (line[s] == ' ' || line[s] == '\t')) s++;
    if (s >= len || line[s] == '#' || line[s] == ';' || line[s] == '[') return 0;
    size_t eq = s;
    while (eq < len && line[eq] != '=') eq++;
    if (eq >= len) return 0;

    size_t ke = eq;
    while (ke > s && (line[ke - 1] == ' ' || line[ke - 1] == '\t')) ke--;
    size_t klen = ke - s;
    if (klen == 0 || klen >= key_cap) return 0;
    memcpy(key_out, line + s, klen);
    key_out[klen] = '\0';

    size_t vs = eq + 1;
    while (vs < len && (line[vs] == ' ' || line[vs] == '\t')) vs++;
    size_t ve = len;
    if (vs < ve && line[vs] == '"') {
        vs++;
        size_t q = vs;
        while (q < ve && line[q] != '"') q++;
        ve = q;
    } else {
        for (size_t i = vs; i < ve; i++) {
            if (line[i] == '#' || line[i] == ';') { ve = i; break; }
        }
        while (ve > vs && (line[ve - 1] == ' ' || line[ve - 1] == '\t')) ve--;
    }
    size_t vlen = ve - vs;
    if (vlen >= val_cap) vlen = val_cap - 1;
    memcpy(val_out, line + vs, vlen);
    val_out[vlen] = '\0';
    return vlen > 0 ? 1 : 0;
}

static int lookup_assoc_ini(const char* ext_no_dot, char* out_spec, size_t cap) {
    if (!ext_no_dot || !ext_no_dot[0]) return -1;
    char ini_path[64];
    if (find_assoc_ini_path(ini_path, sizeof(ini_path), 0) != 0) return -1;

    char* buf = (char*)malloc(8192);
    if (!buf) return -1;
    int n = pxe_read_file(ini_path, buf, 8191);
    if (n <= 0) {
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    int in_assoc = 1;
    int found = -1;
    const char* p = buf;
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\r' && *eol != '\n') eol++;
        size_t len = (size_t)(eol - p);
        size_t s = 0;
        while (s < len && (p[s] == ' ' || p[s] == '\t')) s++;
        if (s < len && p[s] == '[') {
            size_t e = len;
            while (e > s && (p[e - 1] == ' ' || p[e - 1] == '\t')) e--;
            if (e - s == 7 && strncmp(p + s, "[assoc]", 7) == 0) in_assoc = 1;
            else in_assoc = 0;
        } else if (in_assoc) {
            char k[32], v[128];
            if (parse_ini_kv(p, len, k, sizeof(k), v, sizeof(v)) &&
                strcasecmp(k, ext_no_dot) == 0) {
                snprintf(out_spec, cap, "%s", v);
                found = 0;
                break;
            }
        }
        p = eol;
        while (*p == '\r' || *p == '\n') p++;
    }
    free(buf);
    return found;
}

static int resolve_app_spec(const char* spec, char* out_path, size_t cap) {
    if (!spec || !spec[0]) return -1;
    pxe_dirent_t st;
    if (strchr(spec, ':') || strchr(spec, '\\') || strchr(spec, '/')) {
        if (pxe_stat(spec, &st) == 0 && !st.is_dir) {
            snprintf(out_path, cap, "%s", spec);
            return 0;
        }
        return -1;
    }
    return resolve_app_path(spec, out_path, cap);
}

static int save_assoc_ini(const char* ext_no_dot, const char* app_spec) {
    if (!ext_no_dot || !ext_no_dot[0] || !app_spec || !app_spec[0]) return -1;
    char dst_ini[64];
    if (find_assoc_ini_path(dst_ini, sizeof(dst_ini), 1) != 0) return -1;

    /* Ensure <drive>:\system exists */
    char sys_dir[32];
    snprintf(sys_dir, sizeof(sys_dir), "%c:\\system", dst_ini[0]);
    pxe_dirent_t st;
    if (pxe_stat(sys_dir, &st) != 0) {
        (void)pxe_mkdir(sys_dir);
    }

    /* Normalize C:\APPS\foo.pxe -> foo.pxe for portability */
    const char* save_spec = app_spec;
    if (strlen(app_spec) > 8 && app_spec[1] == ':' &&
        (app_spec[2] == '\\' || app_spec[2] == '/')) {
        if (text_contains_ci(app_spec + 2, "\\APPS\\") ||
            text_contains_ci(app_spec + 2, "/APPS/")) {
            const char* base = strrchr(app_spec, '\\');
            const char* base2 = strrchr(app_spec, '/');
            if (base2 && (!base || base2 > base)) base = base2;
            if (base && base[1]) save_spec = base + 1;
        }
    }

    char src_ini[64];
    src_ini[0] = '\0';
    char* in_buf = (char*)malloc(8192);
    char* out_buf = (char*)malloc(12288);
    if (!in_buf || !out_buf) {
        free(in_buf);
        free(out_buf);
        return -1;
    }
    in_buf[0] = '\0';
    if (find_assoc_ini_path(src_ini, sizeof(src_ini), 0) == 0) {
        int r = pxe_read_file(src_ini, in_buf, 8191);
        if (r > 0) in_buf[r] = '\0';
        else in_buf[0] = '\0';
    }

    size_t out_pos = 0;
    size_t out_cap = 12288;
    int replaced = 0;
    int has_assoc_hdr = 0;
    int in_assoc = 0;
    const char* p = in_buf;
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\r' && *eol != '\n') eol++;
        size_t len = (size_t)(eol - p);
        size_t s = 0;
        while (s < len && (p[s] == ' ' || p[s] == '\t')) s++;

        if (s < len && p[s] == '[') {
            size_t e = len;
            while (e > s && (p[e - 1] == ' ' || p[e - 1] == '\t')) e--;
            if (in_assoc && !replaced) {
                int w = snprintf(out_buf + out_pos, out_cap - out_pos,
                                 "%s=%s\n", ext_no_dot, save_spec);
                if (w > 0) out_pos += (size_t)w;
                replaced = 1;
            }
            if (e - s == 7 && strncmp(p + s, "[assoc]", 7) == 0) {
                has_assoc_hdr = 1;
                in_assoc = 1;
            } else {
                in_assoc = 0;
            }
            if (out_pos + len + 2 < out_cap) {
                memcpy(out_buf + out_pos, p, len);
                out_pos += len;
                out_buf[out_pos++] = '\n';
            }
        } else if (in_assoc) {
            char k[32], v[128];
            if (parse_ini_kv(p, len, k, sizeof(k), v, sizeof(v)) &&
                strcasecmp(k, ext_no_dot) == 0) {
                if (!replaced) {
                    int w = snprintf(out_buf + out_pos, out_cap - out_pos,
                                     "%s=%s\n", ext_no_dot, save_spec);
                    if (w > 0) out_pos += (size_t)w;
                    replaced = 1;
                }
            } else if (out_pos + len + 2 < out_cap) {
                memcpy(out_buf + out_pos, p, len);
                out_pos += len;
                out_buf[out_pos++] = '\n';
            }
        } else if (out_pos + len + 2 < out_cap) {
            memcpy(out_buf + out_pos, p, len);
            out_pos += len;
            out_buf[out_pos++] = '\n';
        }
        p = eol;
        while (*p == '\r' || *p == '\n') p++;
    }

    if (!replaced) {
        if (!has_assoc_hdr) {
            int w = snprintf(out_buf + out_pos, out_cap - out_pos, "[assoc]\n");
            if (w > 0) out_pos += (size_t)w;
        }
        int w = snprintf(out_buf + out_pos, out_cap - out_pos,
                         "%s=%s\n", ext_no_dot, save_spec);
        if (w > 0) out_pos += (size_t)w;
    }

    int wrc = pxe_write_file(dst_ini, out_buf, out_pos);
    free(in_buf);
    free(out_buf);
    return wrc == 0 ? 0 : -1;
}

static int scan_apps_ext_assoc_on_drive(char drive, const char* ext_no_dot,
                                        char* out_app, size_t cap) {
    if (!drive) return -1;
    char dir[32];
    snprintf(dir, sizeof(dir), "%c:\\APPS", drive);
    pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 64);
    if (!ents) return -1;
    int n = pxe_listdir(dir, ents, 64);
    int found = -1;
    for (int i = 0; i < n; i++) {
        if (ents[i].is_dir || !ents[i].name[0]) continue;
        const char* dot = strrchr(ents[i].name, '.');
        if (!dot || strcasecmp(dot, ".pxe") != 0) continue;
        char full[128], assoc[32];
        uint8_t subsys = 0;
        snprintf(full, sizeof(full), "%s\\%s", dir, ents[i].name);
        if (read_pxe_meta(full, &subsys, NULL, 0, assoc, sizeof(assoc)) == 0 &&
            subsys == PXE_SUBSYS_GUI && ext_list_contains(assoc, ext_no_dot)) {
            snprintf(out_app, cap, "%s", full);
            found = 0;
            break;
        }
    }
    free(ents);
    return found;
}

static int resolve_associated_app(const char* ext_no_dot, char* out_app, size_t cap) {
    if (!ext_no_dot || !ext_no_dot[0]) return -1;

    char spec[128];
    if (lookup_assoc_ini(ext_no_dot, spec, sizeof(spec)) == 0) {
        if (resolve_app_spec(spec, out_app, cap) == 0) return 0;
        return -2; /* Configured app is missing */
    }

    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);
    if (info.system_drive &&
        scan_apps_ext_assoc_on_drive(info.system_drive, ext_no_dot, out_app, cap) == 0) {
        return 0;
    }
    if (info.cdrom_drive && info.cdrom_drive != info.system_drive &&
        scan_apps_ext_assoc_on_drive(info.cdrom_drive, ext_no_dot, out_app, cap) == 0) {
        return 0;
    }
    return -1;
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
        show_msg(fm, FM_TR(fm, "读取错误", "Read Error"),
                 FM_TR(fm, "无法读取该驱动器或目录。", "Cannot read drive or directory."), 1);
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

static void collect_ow_apps_on_drive(fm_t* fm, char drive, int boot_mode) {
    if (!drive || fm->ow_app_count >= MAX_OW_APPS) return;
    char dir[32];
    snprintf(dir, sizeof(dir), "%c:\\APPS", drive);
    pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 64);
    if (!ents) return;
    int n = pxe_listdir(dir, ents, 64);
    for (int i = 0; i < n && fm->ow_app_count < MAX_OW_APPS; i++) {
        if (ents[i].is_dir || !ents[i].name[0]) continue;
        const char* dot = strrchr(ents[i].name, '.');
        if (!dot || strcasecmp(dot, ".pxe") != 0) continue;
        if (boot_mode != 1 && strcasecmp(ents[i].name, "install.pxe") == 0) continue;
        int dup = 0;
        for (int k = 0; k < fm->ow_app_count; k++) {
            if (strcasecmp(fm->ow_apps[k].filename, ents[i].name) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;
        char full[128], title[48];
        uint8_t subsys = 0;
        snprintf(full, sizeof(full), "%s\\%s", dir, ents[i].name);
        if (read_pxe_meta(full, &subsys, title, sizeof(title), NULL, 0) != 0) continue;
        if (subsys != PXE_SUBSYS_GUI) continue;

        fm_ow_app_t* item = &fm->ow_apps[fm->ow_app_count++];
        snprintf(item->filename, sizeof(item->filename), "%s", ents[i].name);
        snprintf(item->path, sizeof(item->path), "%s", full);
        snprintf(item->title, sizeof(item->title), "%s", title[0] ? title : ents[i].name);
    }
    free(ents);
}

static void action_begin_open_with(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    const fm_entry_t* e = &fm->entries[idx];
    if (e->is_dir || e->is_parent) return;

    join_path(fm->modal_target_path, sizeof(fm->modal_target_path), fm->current_path, e->name);
    snprintf(fm->modal_title, sizeof(fm->modal_title), "%s", FM_TR(fm, "打开方式", "Open With"));
    snprintf(fm->modal_msg, sizeof(fm->modal_msg), "%s", e->name);

    extract_ext_no_dot(e->name, fm->ow_ext, sizeof(fm->ow_ext));

    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);

    fm->ow_app_count = 0;
    if (info.system_drive) collect_ow_apps_on_drive(fm, info.system_drive, info.boot_mode);
    if (info.cdrom_drive && info.cdrom_drive != info.system_drive) {
        collect_ow_apps_on_drive(fm, info.cdrom_drive, info.boot_mode);
    }
    for (int i = 0; i < fm->ow_app_count - 1; i++) {
        for (int j = i + 1; j < fm->ow_app_count; j++) {
            if (strcasecmp(fm->ow_apps[i].filename, fm->ow_apps[j].filename) > 0) {
                fm_ow_app_t tmp = fm->ow_apps[i];
                fm->ow_apps[i] = fm->ow_apps[j];
                fm->ow_apps[j] = tmp;
            }
        }
    }

    fm->ow_sel = fm->ow_app_count > 0 ? 0 : -1;
    fm->ow_scroll = 0;
    fm->modal_input[0] = '\0';

    char cur_assoc[128];
    if (fm->ow_ext[0] && resolve_associated_app(fm->ow_ext, cur_assoc, sizeof(cur_assoc)) == 0) {
        const char* base = strrchr(cur_assoc, '\\');
        base = base ? (base + 1) : cur_assoc;
        for (int i = 0; i < fm->ow_app_count; i++) {
            if (strcasecmp(fm->ow_apps[i].filename, base) == 0) {
                fm->ow_sel = i;
                break;
            }
        }
        snprintf(fm->modal_input, sizeof(fm->modal_input), "%s",
                 fm->ow_sel >= 0 ? fm->ow_apps[fm->ow_sel].filename : cur_assoc);
    } else if (fm->ow_sel >= 0) {
        snprintf(fm->modal_input, sizeof(fm->modal_input), "%s", fm->ow_apps[fm->ow_sel].filename);
    }
    fm->modal_len = (int)strlen(fm->modal_input);
    if (fm->ow_sel >= OW_VISIBLE_ROWS) {
        fm->ow_scroll = fm->ow_sel - OW_VISIBLE_ROWS + 1;
    }

    char ini_wr[64];
    fm->ow_can_save = (fm->ow_ext[0] && find_assoc_ini_path(ini_wr, sizeof(ini_wr), 1) == 0) ? 1 : 0;
    fm->ow_save_default = 0;
    fm->modal = MODAL_OPEN_WITH;
}

static void submit_open_with(fm_t* fm) {
    char spec[128];
    snprintf(spec, sizeof(spec), "%s", fm->modal_input);
    int len = (int)strlen(spec);
    while (len > 0 && (spec[len - 1] == ' ' || spec[len - 1] == '\t')) spec[--len] = '\0';
    const char* s = spec;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) {
        show_msg(fm, FM_TR(fm, "打开方式", "Open With"),
                 FM_TR(fm, "请选择或输入一个有效的 .pxe 程序路径。",
                       "Select or enter a valid .pxe program path."), 1);
        return;
    }

    char app_path[128];
    if (resolve_app_spec(s, app_path, sizeof(app_path)) != 0) {
        show_msg(fm, FM_TR(fm, "缺少应用程序", "Missing Application"),
                 FM_TR(fm, "找不到指定的 .pxe 应用程序。",
                       "Cannot find the specified .pxe application."), 1);
        return;
    }

    const char* base = strrchr(app_path, '\\');
    const char* base2 = strrchr(app_path, '/');
    if (base2 && (!base || base2 > base)) base = base2;
    base = base ? (base + 1) : app_path;
    if (strcasecmp(base, "install.pxe") == 0) {
        pxe_basic_sysinfo_t info;
        memset(&info, 0, sizeof(info));
        if (pxe_sys_get_basic_info(&info) == 0 && info.boot_mode != 1) {
            show_msg(fm, FM_TR(fm, "安装程序已禁用", "Installer Disabled"),
                     FM_TR(fm, "本地或实时模式下无法运行系统安装程序。",
                           "INSTALL cannot be launched in Local or Live mode."), 1);
            return;
        }
    }

    if (fm->ow_save_default && fm->ow_can_save && fm->ow_ext[0]) {
        (void)save_assoc_ini(fm->ow_ext, app_path);
    }

    fm->modal = MODAL_NONE;
    const char* argv[2] = { app_path, fm->modal_target_path };
    if (pxe_spawn(app_path, 2, argv) < 0) {
        show_msg(fm, FM_TR(fm, "启动失败", "Launch Failed"),
                 FM_TR(fm, "无法启动选定的应用程序。",
                       "Could not launch the selected application."), 1);
    }
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
        action_begin_open_with(fm, idx);
        return;
    }

    if (strcasecmp(ext, ".pxe") == 0) {
        if (strcasecmp(e->name, "install.pxe") == 0) {
            pxe_basic_sysinfo_t info;
            memset(&info, 0, sizeof(info));
            if (pxe_sys_get_basic_info(&info) == 0 && info.boot_mode != 1) {
                show_msg(fm, FM_TR(fm, "安装程序已禁用", "Installer Disabled"),
                         FM_TR(fm, "本地或实时模式下无法运行系统安装程序。",
                               "INSTALL cannot be launched in Local or Live mode."), 1);
                return;
            }
        }
        int pid = pxe_spawn(full, 0, NULL);
        if (pid < 0) {
            show_msg(fm, FM_TR(fm, "启动失败", "PXE Launch Failed"),
                     FM_TR(fm, "无效、已损坏或被拦截的 .pxe 程序。",
                           "Invalid, corrupted, or blocked .pxe."), 1);
        }
        return;
    }

    if (strcasecmp(ext, ".pnk") == 0) {
        unsigned char data[512];
        memset(data, 0, sizeof(data));
        int length = pxe_read_file(full, data, sizeof(data) - 1);
        char target[256];
        target[0] = '\0';
        if (length > 0) {
            data[length] = '\0';
            const char* cursor = (const char*)data;
            while (*cursor) {
                if (strncmp(cursor, "target=", 7) == 0) {
                    const char* end = cursor + 7;
                    while (*end && *end != '\r' && *end != '\n') end++;
                    size_t count = (size_t)(end - (cursor + 7));
                    if (count >= sizeof(target)) count = sizeof(target) - 1;
                    memcpy(target, cursor + 7, count);
                    target[count] = '\0';
                    break;
                }
                while (*cursor && *cursor != '\r' && *cursor != '\n') cursor++;
                while (*cursor == '\r' || *cursor == '\n') cursor++;
            }
        }
        if (!target[0]) {
            show_msg(fm, FM_TR(fm, "快捷方式错误", "Shortcut Error"),
                     FM_TR(fm, "缺少 .pnk 快捷方式目标路径。",
                           "The .pnk target is missing."), 1);
            return;
        }
        if (target[0] && target[1] != ':') {
            pxe_basic_sysinfo_t info;
            memset(&info, 0, sizeof(info));
            pxe_sys_get_basic_info(&info);
            char resolved[256];
            char boot_drive = fm->current_path[0] ? fm->current_path[0] :
                              (info.system_drive ? info.system_drive :
                               info.cdrom_drive);
            if (!boot_drive) {
                show_msg(fm, FM_TR(fm, "快捷方式错误", "Shortcut Error"),
                         FM_TR(fm, "无法解析快捷方式所在系统盘符。",
                               "Cannot resolve the shortcut system drive."), 1);
                return;
            }
            snprintf(resolved, sizeof(resolved), "%c:%s", boot_drive,
                     target);
            strncpy(target, resolved, sizeof(target) - 1);
            target[sizeof(target) - 1] = '\0';
        }
        pxe_dirent_t target_stat;
        if (pxe_stat(target, &target_stat) != 0) {
            show_msg(fm, FM_TR(fm, "快捷方式错误", "Shortcut Error"),
                     FM_TR(fm, "快捷方式指向的目标已不存在。",
                           "The shortcut target no longer exists."), 1);
            return;
        }
        if (target_stat.is_dir) {
            char files_path[128];
            if (resolve_app_path("files.pxe", files_path,
                                 sizeof(files_path)) != 0) {
                show_msg(fm, FM_TR(fm, "快捷方式错误", "Shortcut Error"),
                         FM_TR(fm, "文件管理程序不可用。",
                               "File Manager is unavailable."), 1);
                return;
            }
            const char* argv[2] = { files_path, target };
            if (pxe_spawn(files_path, 2, argv) < 0) {
                show_msg(fm, FM_TR(fm, "快捷方式错误", "Shortcut Error"),
                         FM_TR(fm, "无法打开目标文件夹。",
                               "Folder could not be opened."), 1);
            }
        } else {
            char canonical[128];
            const char* base = strrchr(target, '\\');
            if (!base && !(base = strrchr(target, '/'))) base = target;
            else base++;
            if (text_contains_ci(target, "\\APPS\\")) {
                if (resolve_app_path(base, canonical, sizeof(canonical)) != 0) {
                    show_msg(fm, FM_TR(fm, "快捷方式错误", "Shortcut Error"),
                             FM_TR(fm, "系统应用程序不可用。",
                                   "The system application is unavailable."), 1);
                    return;
                }
            } else {
                snprintf(canonical, sizeof(canonical), "%s", target);
            }
            if (strcasecmp(base, "install.pxe") == 0) {
                pxe_basic_sysinfo_t info;
                memset(&info, 0, sizeof(info));
                if (pxe_sys_get_basic_info(&info) == 0 && info.boot_mode != 1) {
                    show_msg(fm, FM_TR(fm, "安装程序已禁用", "Installer Disabled"),
                             FM_TR(fm, "本地或实时模式下无法运行系统安装程序。",
                                   "INSTALL cannot be launched in Local or Live mode."), 1);
                    return;
                }
            }
            if (pxe_spawn(canonical, 0, NULL) < 0) {
                show_msg(fm, FM_TR(fm, "快捷方式错误", "Shortcut Error"),
                         FM_TR(fm, "无法启动 .pnk 快捷方式目标。",
                               "PXE shortcut could not be launched."), 1);
            }
        }
        return;
    }

    if (strcasecmp(ext, ".pdv") == 0) {
        show_msg(fm, FM_TR(fm, "设备驱动程序 (.pdv)", "Device Driver (.pdv)"),
                 FM_TR(fm, "请复制到 SYSTEM:\\PDEVICE\\ 或在系统信息中加载。",
                       "Copy .pdv to SYSTEM:\\PDEVICE\\ or load in SysInfo."), 0);
        return;
    }

    char ext_no_dot[32];
    extract_ext_no_dot(e->name, ext_no_dot, sizeof(ext_no_dot));

    char app_path[128];
    int assoc_rc = resolve_associated_app(ext_no_dot, app_path, sizeof(app_path));
    if (assoc_rc == -2) {
        show_msg(fm, FM_TR(fm, "缺少应用程序", "Missing Application"),
                 FM_TR(fm, "关联的程序在磁盘上已不存在，请重新指定打开方式。",
                       "Associated app is missing. Use Open With to reassign."), 1);
        return;
    }
    if (assoc_rc != 0) {
        action_begin_open_with(fm, idx);
        return;
    }

    const char* argv[2] = { app_path, full };
    if (pxe_spawn(app_path, 2, argv) < 0) {
        show_msg(fm, FM_TR(fm, "启动失败", "Launch Failed"),
                 FM_TR(fm, "无法启动关联的应用程序。",
                       "Could not start associated application."), 1);
    }
}

static void action_copy(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    if (fm->entries[idx].is_parent) return;
    join_path(fm->clip_path, sizeof(fm->clip_path), fm->current_path, fm->entries[idx].name);
    snprintf(fm->clip_name, sizeof(fm->clip_name), "%s", fm->entries[idx].name);
    fm->clip_is_dir = fm->entries[idx].is_dir;
    fm->clip_is_cut = 0;
    fm->has_clip = 1;
}

static void action_cut(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    if (fm->entries[idx].is_parent) return;
    join_path(fm->clip_path, sizeof(fm->clip_path), fm->current_path,
              fm->entries[idx].name);
    snprintf(fm->clip_name, sizeof(fm->clip_name), "%s",
             fm->entries[idx].name);
    fm->clip_is_dir = fm->entries[idx].is_dir;
    fm->clip_is_cut = 1;
    fm->has_clip = 1;
}

static void action_properties(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    const fm_entry_t* e = &fm->entries[idx];
    if (e->is_parent) return;
    if (e->is_dir) {
        char msg[128];
        snprintf(msg, sizeof(msg), FM_TR(fm, "文件夹: %.48s", "Folder: %.28s"), e->name);
        show_msg(fm, FM_TR(fm, "属性", "Properties"), msg, 0);
        return;
    }
    char hsz[24], msg[128];
    fmt_bytes(e->size, hsz, sizeof(hsz));
    snprintf(msg, sizeof(msg), "%.24s | %s (%llu B)",
             e->name, hsz, (unsigned long long)e->size);
    show_msg(fm, FM_TR(fm, "文件属性", "File Properties"), msg, 0);
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
    pxe_draw_text_small(s, dx + 8, dy + 7,
                        pxe_is_zh() ? "正在复制文件..." : "Copying File...",
                        PXE_COL_WHITE);
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

typedef struct {
    char src[256];
    char dst[256];
} fm_dir_pair_t;

static int path_is_same_or_subpath_ci(const char* candidate, const char* parent) {
    if (!candidate || !parent) return 0;
    size_t plen = strlen(parent);
    while (plen > 3 && (parent[plen - 1] == '\\' || parent[plen - 1] == '/')) plen--;
    size_t clen = strlen(candidate);
    if (clen < plen) return 0;
    for (size_t i = 0; i < plen; i++) {
        char a = ascii_lower(candidate[i]);
        char b = ascii_lower(parent[i]);
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (a != b) return 0;
    }
    return candidate[plen] == '\0' || candidate[plen] == '\\' || candidate[plen] == '/';
}

static int copy_directory_tree(fm_t* fm, const char* src_root, const char* dst_root,
                               const pxe_win_surface_t* surf) {
    int cap = 32;
    int head = 0;
    int tail = 0;
    fm_dir_pair_t* queue = (fm_dir_pair_t*)malloc(sizeof(fm_dir_pair_t) * (size_t)cap);
    pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 256);
    if (!queue || !ents) {
        free(queue);
        free(ents);
        return -1;
    }

    snprintf(queue[tail].src, sizeof(queue[tail].src), "%s", src_root);
    snprintf(queue[tail].dst, sizeof(queue[tail].dst), "%s", dst_root);
    tail++;

    g_prog_surf = surf;
    g_prog_win_id = fm->win_id;

    int rc = 0;
    while (head < tail && rc == 0) {
        char cur_src[256];
        char cur_dst[256];
        snprintf(cur_src, sizeof(cur_src), "%s", queue[head].src);
        snprintf(cur_dst, sizeof(cur_dst), "%s", queue[head].dst);
        head++;

        pxe_dirent_t st;
        if (pxe_stat(cur_dst, &st) == 0) {
            if (!st.is_dir) { rc = -1; break; }
        } else if (pxe_mkdir(cur_dst) != 0) {
            rc = -1;
            break;
        }

        int cnt = pxe_listdir(cur_src, ents, 256);
        if (cnt < 0) { rc = -1; break; }

        for (int i = 0; i < cnt; i++) {
            if (!ents[i].name[0] ||
                strcmp(ents[i].name, ".") == 0 ||
                strcmp(ents[i].name, "..") == 0) {
                continue;
            }
            char child_src[256], child_dst[256];
            join_path(child_src, sizeof(child_src), cur_src, ents[i].name);
            join_path(child_dst, sizeof(child_dst), cur_dst, ents[i].name);

            if (ents[i].is_dir) {
                if (tail >= cap) {
                    int ncap = cap * 2;
                    fm_dir_pair_t* nq = (fm_dir_pair_t*)realloc(
                        queue, sizeof(fm_dir_pair_t) * (size_t)ncap);
                    if (!nq) { rc = -1; break; }
                    queue = nq;
                    cap = ncap;
                }
                snprintf(queue[tail].src, sizeof(queue[tail].src), "%s", child_src);
                snprintf(queue[tail].dst, sizeof(queue[tail].dst), "%s", child_dst);
                tail++;
            } else {
                snprintf(g_prog_title, sizeof(g_prog_title), "%s", ents[i].name);
                g_prog_last_bytes = 0;
                if (pxe_copy_file(child_src, child_dst, copy_progress_cb) != 0) {
                    rc = -1;
                    break;
                }
            }
        }
    }

    g_prog_surf = NULL;
    free(ents);
    free(queue);
    return rc;
}

static void action_paste(fm_t* fm, const pxe_win_surface_t* surf) {
    if (!fm->has_clip || fm->current_path[0] == '\0') return;
    if (drive_is_readonly(fm->current_path[0])) {
        show_msg(fm, FM_TR(fm, "只读驱动器", "Read-Only Drive"),
                 FM_TR(fm, "无法粘贴到只读驱动器上。",
                       "Cannot paste onto a read-only drive."), 1);
        return;
    }

    char dst[256];
    join_path(dst, sizeof(dst), fm->current_path, fm->clip_name);
    if (strcasecmp(dst, fm->clip_path) == 0) {
        if (fm->clip_is_cut) {
            fm->has_clip = 0;
            show_msg(fm, FM_TR(fm, "移动", "Move"),
                     FM_TR(fm, "该项目已经位于当前目录中。",
                           "The item is already in this folder."), 0);
            return;
        }
        char alt[128];
        snprintf(alt, sizeof(alt), "COPY_%s", fm->clip_name);
        join_path(dst, sizeof(dst), fm->current_path, alt);
    }

    if (fm->clip_is_cut) {
        pxe_dirent_t existing;
        if (pxe_stat(dst, &existing) == 0) {
            show_msg(fm, FM_TR(fm, "移动失败", "Move Failed"),
                     FM_TR(fm, "目标目录已存在同名文件或文件夹。",
                           "A file or folder with that name already exists."), 1);
            return;
        }
        if (pxe_rename(fm->clip_path, dst) != 0) {
            show_msg(fm, FM_TR(fm, "移动失败", "Move Failed"),
                     FM_TR(fm, "移动失败（只读或受保护的系统项目）。",
                           "Cut/paste failed (read-only or protected item)."), 1);
            return;
        }
        fm->has_clip = 0;
        fm->clip_is_cut = 0;
        load_directory(fm);
        show_msg(fm, FM_TR(fm, "移动完成", "Move Complete"),
                 FM_TR(fm, "项目已成功移动到当前目录。",
                       "Item moved to this folder."), 0);
        return;
    }

    if (fm->clip_is_dir) {
        if (path_is_same_or_subpath_ci(dst, fm->clip_path)) {
            show_msg(fm, FM_TR(fm, "粘贴错误", "Paste Error"),
                     FM_TR(fm, "目标文件夹是源文件夹的子目录。",
                           "Cannot copy a folder into itself or a subfolder."), 1);
            return;
        }
        int rc = copy_directory_tree(fm, fm->clip_path, dst, surf);
        if (rc != 0) {
            show_msg(fm, FM_TR(fm, "粘贴失败", "Paste Failed"),
                     FM_TR(fm, "复制文件夹失败（磁盘已满或权限受限）。",
                           "Folder copy error (disk full or I/O error)."), 1);
            return;
        }
        fm->clip_is_cut = 0;
        load_directory(fm);
        return;
    }

    g_prog_surf = surf;
    g_prog_win_id = fm->win_id;
    snprintf(g_prog_title, sizeof(g_prog_title), "%s", fm->clip_name);
    g_prog_last_bytes = 0;

    int rc = pxe_copy_file(fm->clip_path, dst, copy_progress_cb);
    g_prog_surf = NULL;

    if (rc != 0) {
        show_msg(fm, FM_TR(fm, "粘贴失败", "Paste Failed"),
                 FM_TR(fm, "复制失败（磁盘已满或超出 FAT32 单文件上限）。",
                       "Copy error (disk full / >4GB FAT32 limit)."), 1);
        return;
    }
    fm->clip_is_cut = 0;
    load_directory(fm);
}

static void action_begin_rename(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    if (fm->entries[idx].is_parent) return;
    if (drive_is_readonly(fm->current_path[0])) {
        show_msg(fm, FM_TR(fm, "只读驱动器", "Read-Only Drive"),
                 FM_TR(fm, "无法在只读驱动器上重命名文件。", "Cannot rename files on read-only drive."), 1);
        return;
    }
    fm->modal = MODAL_RENAME;
    snprintf(fm->modal_title, sizeof(fm->modal_title), "%s", FM_TR(fm, "重命名", "Rename"));
    snprintf(fm->modal_msg, sizeof(fm->modal_msg), "%s", FM_TR(fm, "新名称:", "New name:"));
    snprintf(fm->modal_input, sizeof(fm->modal_input), "%s", fm->entries[idx].name);
    fm->modal_len = (int)strlen(fm->modal_input);
    join_path(fm->modal_target_path, sizeof(fm->modal_target_path),
              fm->current_path, fm->entries[idx].name);
}

static void action_begin_mkdir(fm_t* fm) {
    if (fm->current_path[0] == '\0') {
        show_msg(fm, FM_TR(fm, "新建文件夹", "New Folder"),
                 FM_TR(fm, "请先打开一个可写磁盘分区。", "Open a writable drive first."), 1);
        return;
    }
    if (drive_is_readonly(fm->current_path[0])) {
        show_msg(fm, FM_TR(fm, "只读驱动器", "Read-Only Drive"),
                 FM_TR(fm, "无法在只读驱动器上创建文件夹。", "Cannot create folder on read-only drive."), 1);
        return;
    }
    fm->modal = MODAL_MKDIR;
    snprintf(fm->modal_title, sizeof(fm->modal_title), "%s", FM_TR(fm, "新建文件夹", "New Folder"));
    snprintf(fm->modal_msg, sizeof(fm->modal_msg), "%s", FM_TR(fm, "文件夹名称:", "Folder name:"));
    snprintf(fm->modal_input, sizeof(fm->modal_input), "%s", FM_TR(fm, "新建文件夹", "NEW_DIR"));
    fm->modal_len = (int)strlen(fm->modal_input);
}

static void action_begin_delete(fm_t* fm, int idx) {
    if (fm->current_path[0] == '\0' || idx < 0 || idx >= fm->entry_count) return;
    if (fm->entries[idx].is_parent) return;
    if (drive_is_readonly(fm->current_path[0])) {
        show_msg(fm, FM_TR(fm, "只读驱动器", "Read-Only Drive"),
                 FM_TR(fm, "无法在只读驱动器上执行删除。", "Cannot delete on read-only drive."), 1);
        return;
    }
    fm->modal = MODAL_CONFIRM_DEL;
    snprintf(fm->modal_title, sizeof(fm->modal_title), "%s", FM_TR(fm, "确认删除", "Confirm Delete"));
    snprintf(fm->modal_msg, sizeof(fm->modal_msg),
             FM_TR(fm, "确定删除 \"%s\" 吗?", "Delete \"%s\"?"), fm->entries[idx].name);
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
        show_msg(fm, FM_TR(fm, "弹出", "Eject"),
                 FM_TR(fm, "请选择一个软驱或光驱设备。", "Select a floppy or optical drive."), 1);
        return;
    }
    if (pxe_eject_drive(letter) != 0) {
        show_msg(fm, FM_TR(fm, "弹出失败", "Eject Failed"),
                 FM_TR(fm, "该驱动器不支持弹出操作。", "Drive does not support eject."), 1);
        return;
    }
    fm->current_path[0] = '\0';
    load_directory(fm);
}

static void submit_modal(fm_t* fm) {
    if (fm->modal == MODAL_RENAME) {
        if (fm->modal_len <= 0 || strchr(fm->modal_input, '\\') || strchr(fm->modal_input, '/')) {
            show_msg(fm, FM_TR(fm, "重命名失败", "Rename Failed"),
                     FM_TR(fm, "请输入不含斜杠的有效文件名。",
                           "Enter a valid filename without slashes."), 1);
            return;
        }
        char new_full[256];
        join_path(new_full, sizeof(new_full), fm->current_path, fm->modal_input);
        fm->modal = MODAL_NONE;
        int renamed = pxe_rename(fm->modal_target_path, new_full);
        if (renamed != 0) {
            if (renamed == -8) {
                show_msg(fm, FM_TR(fm, "受保护的系统文件", "Protected System File"),
                         FM_TR(fm, "系统关键文件与锁定快捷方式不可重命名。",
                               "Required system files and locked shortcuts cannot be renamed."), 1);
            } else {
                show_msg(fm, FM_TR(fm, "重命名失败", "Rename Failed"),
                         FM_TR(fm, "文件名冲突或磁盘 I/O 错误。",
                               "Name collision or I/O error."), 1);
            }
            return;
        }
        load_directory(fm);
    } else if (fm->modal == MODAL_MKDIR) {
        if (fm->modal_len <= 0 || strchr(fm->modal_input, '\\') || strchr(fm->modal_input, '/')) {
            show_msg(fm, FM_TR(fm, "新建失败", "New Folder Failed"),
                     FM_TR(fm, "请输入有效的文件夹名称。", "Enter a valid folder name."), 1);
            return;
        }
        char full[256];
        join_path(full, sizeof(full), fm->current_path, fm->modal_input);
        fm->modal = MODAL_NONE;
        if (pxe_mkdir(full) != 0) {
            show_msg(fm, FM_TR(fm, "新建失败", "New Folder Failed"),
                     FM_TR(fm, "文件夹已存在或磁盘空间已满。",
                           "Folder exists or disk full."), 1);
            return;
        }
        load_directory(fm);
    } else if (fm->modal == MODAL_CONFIRM_DEL) {
        fm->modal = MODAL_NONE;
        int deleted = pxe_unlink(fm->modal_target_path);
        if (deleted != 0) {
            if (deleted == -8) {
                show_msg(fm, FM_TR(fm, "受保护的系统文件", "Protected System File"),
                         FM_TR(fm, "系统关键程序与锁定快捷方式不可删除。",
                               "Required system software and locked shortcuts cannot be deleted."), 1);
            } else {
                show_msg(fm, FM_TR(fm, "删除失败", "Delete Failed"),
                         FM_TR(fm, "目录非空、驱动器只读或项目受保护。",
                               "Folder non-empty, read-only, or protected."), 1);
            }
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
    pxe_draw_button(surf, bx, 3, 46, 20, FM_TR(fm, "磁盘", "Drives"), 0, 1); bx += 50;
    pxe_draw_button(surf, bx, 3, 36, 20, FM_TR(fm, "上级", "Up"), 0, fm->current_path[0] != 0); bx += 40;
    int can_open = (fm->selected >= 0 && fm->selected < fm->entry_count);
    pxe_draw_button(surf, bx, 3, 42, 20, FM_TR(fm, "打开", "Open"), 0, can_open); bx += 46;
    int can_copy = (fm->current_path[0] != 0 && can_open && !fm->entries[fm->selected].is_parent);
    pxe_draw_button(surf, bx, 3, 38, 20, FM_TR(fm, "复制", "Copy"), 0, can_copy); bx += 42;
    pxe_draw_button(surf, bx, 3, 32, 20, FM_TR(fm, "剪切", "Cut"), 0, can_copy); bx += 36;
    int can_paste = (fm->has_clip && fm->current_path[0] != 0);
    pxe_draw_button(surf, bx, 3, 42, 20, FM_TR(fm, "粘贴", "Paste"), 0, can_paste); bx += 46;
    pxe_draw_button(surf, bx, 3, 50, 20, FM_TR(fm, "新建", "NewDir"), 0, fm->current_path[0] != 0); bx += 54;
    pxe_draw_button(surf, bx, 3, 42, 20, FM_TR(fm, "弹出", "Eject"), 0, 1);

    /* 2. Path bar */
    pxe_draw_bevel_box(surf, 4, 26, surf->width - 8, 17, 1, PXE_COL_WHITE);
    char pstr[272], pclip[272];
    snprintf(pstr, sizeof(pstr), "%s: %s",
             FM_TR(fm, "路径", "Path"),
             fm->current_path[0] ? fm->current_path : FM_TR(fm, "驱动器列表", "Drives Root"));
    utf8_clip_by_pixels(pstr, surf->width - 18, 1, pclip, sizeof(pclip));
    pxe_draw_text_small(surf, 8, 29, pclip, PXE_COL_BLACK);

    /* 3. Column header */
    int list_x = 4, list_y = HEADER_H, list_w = surf->width - 8 - SB_W;
    int list_h = surf->height - HEADER_H - STATUS_H;
    if (list_h < 20) list_h = 20;
    pxe_draw_bevel_box(surf, 4, 44, surf->width - 8, 17, 0, PXE_COL_LIGHT_GRAY);
    if (fm->current_path[0] == '\0') {
        pxe_draw_text_small(surf, 10, 47, FM_TR(fm, "驱动器", "Drive"), PXE_COL_BLACK);
        pxe_draw_text_small(surf, 76, 47, FM_TR(fm, "格式", "FS"), PXE_COL_BLACK);
        pxe_draw_text_small(surf, 152, 47, FM_TR(fm, "卷标", "Label"), PXE_COL_BLACK);
        pxe_draw_text_small(surf, list_w - 134, 47, FM_TR(fm, "总计", "Total"), PXE_COL_BLACK);
        pxe_draw_text_small(surf, list_w - 64, 47, FM_TR(fm, "可用", "Free"), PXE_COL_BLACK);
    } else {
        pxe_draw_text_small(surf, 10, 47, FM_TR(fm, "名称", "Name"), PXE_COL_BLACK);
        pxe_draw_text_small(surf, list_w - 78, 47, FM_TR(fm, "类型 / 大小", "Type / Size"), PXE_COL_BLACK);
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

        char dname[136], dclip[136];
        if (e->is_dir) {
            if (e->is_parent && strlen(fm->current_path) <= 3) {
                snprintf(dname, sizeof(dname), "[.. (Drives)]");
            } else {
                snprintf(dname, sizeof(dname), "[%s]", e->name);
            }
        } else {
            snprintf(dname, sizeof(dname), "%s", e->name);
        }
        int name_px = (fm->current_path[0] == '\0') ? 68 : (list_w - 94);
        if (name_px < 24) name_px = 24;
        utf8_clip_by_pixels(dname, name_px, 0, dclip, sizeof(dclip));
        pxe_draw_text(surf, list_x + 6, ry + 1, dclip, fg);

        if (fm->current_path[0] == '\0') {
            pxe_draw_text_small(surf, 76, ry + 5,
                                e->fs_name[0] ? e->fs_name : "DRIVE", fg);
            if (e->label[0]) {
                char lb[32];
                utf8_clip_by_pixels(e->label, 72, 1, lb, sizeof(lb));
                pxe_draw_text_small(surf, 152, ry + 3, lb, fg);
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
    char sbuf[160], sclip[160];
    if (fm->has_clip) {
        snprintf(sbuf, sizeof(sbuf), "%s: %s | %s: %d",
                 fm->clip_is_cut ? FM_TR(fm, "已剪切", "Cut") : FM_TR(fm, "已复制", "Copied"),
                 fm->clip_name, FM_TR(fm, "项目数", "Items"), fm->entry_count);
    } else if (fm->current_path[0] == '\0') {
        snprintf(sbuf, sizeof(sbuf),
                 fm->zh ? "驱动器: %d | 双击进入分区 | 弹出可卸载光盘/软盘"
                        : "Drives: %d | Double-click to open | Eject unmounts CD/FD",
                 fm->entry_count);
    } else {
        snprintf(sbuf, sizeof(sbuf),
                 fm->zh ? "项目: %d | 右键可剪切、复制、粘贴、重命名或删除"
                        : "Items: %d | Right-click for Cut/Copy/Paste/Rename/Delete",
                 fm->entry_count > 0 ? fm->entry_count - 1 : 0);
    }
    utf8_clip_by_pixels(sbuf, surf->width - 14, 1, sclip, sizeof(sclip));
    pxe_draw_text_small(surf, 6, sby + 5, sclip, PXE_COL_BLACK);

    /* 7. Context Menu */
    if (fm->ctx_kind != CTX_NONE) {
        pxe_draw_bevel_box(surf, fm->ctx_x, fm->ctx_y, fm->ctx_w, fm->ctx_h, 0, PXE_COL_LIGHT_GRAY);
        if (fm->ctx_kind == CTX_ITEM) {
            int tgt_is_dir = (fm->ctx_target >= 0 && fm->ctx_target < fm->entry_count)
                             ? fm->entries[fm->ctx_target].is_dir : 1;
            const char* items_en[9] = {
                "Open", "Open With...", "Cut", "Copy", "Paste", "Rename...",
                "Delete", "New Folder...", "Properties..."
            };
            const char* items_zh[9] = {
                "打开", "打开方式...", "剪切", "复制", "粘贴", "重命名...",
                "删除", "新建目录...", "属性..."
            };
            int en[9] = { 1, !tgt_is_dir, 1, 1, fm->has_clip, 1, 1, 1, 1 };
            for (int i = 0; i < 9; i++) {
                pxe_draw_text_small(surf, fm->ctx_x + 10, fm->ctx_y + 4 + i * 18,
                                    fm->zh ? items_zh[i] : items_en[i],
                                    en[i] ? PXE_COL_BLACK : PXE_COL_DARK_GRAY);
            }
        } else if (fm->ctx_kind == CTX_BG) {
            const char* items_en[4] = { "Paste", "New Folder...", "Refresh", "Drives" };
            const char* items_zh[4] = { "粘贴", "新建目录...", "刷新", "驱动器" };
            int en[4] = { fm->has_clip, 1, 1, 1 };
            for (int i = 0; i < 4; i++) {
                pxe_draw_text_small(surf, fm->ctx_x + 10, fm->ctx_y + 4 + i * 18,
                                    fm->zh ? items_zh[i] : items_en[i],
                                    en[i] ? PXE_COL_BLACK : PXE_COL_DARK_GRAY);
            }
        } else if (fm->ctx_kind == CTX_DRIVE) {
            pxe_draw_text_small(surf, fm->ctx_x + 10, fm->ctx_y + 4, FM_TR(fm, "打开", "Open"), PXE_COL_BLACK);
            pxe_draw_text_small(surf, fm->ctx_x + 10, fm->ctx_y + 22, FM_TR(fm, "弹出", "Eject"), PXE_COL_BLACK);
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
        pxe_draw_text_small(surf, dx + 10, dy + 23, fm->modal_msg, PXE_COL_BLACK);
        pxe_draw_bevel_box(surf, dx + 10, dy + 36, dw - 20, 22, 1, PXE_COL_WHITE);
        const char* vis_in = fm->modal_input;
        while (*vis_in && pxe_text_width(vis_in) > dw - 28) {
            (void)utf8_decode_next(&vis_in);
        }
        pxe_draw_text(surf, dx + 14, dy + 39, vis_in, PXE_COL_BLACK);
        pxe_draw_vline(surf, dx + 14 + pxe_text_width(vis_in), dy + 39, 16, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + 46,  dy + 66, 72, 22, FM_TR(fm, "确定", "OK"), 0, 1);
        pxe_draw_button(surf, dx + 142, dy + 66, 72, 22, FM_TR(fm, "取消", "Cancel"), 0, 1);
    } else if (fm->modal == MODAL_CONFIRM_DEL) {
        int dw = 280, dh = 86;
        if (dw > surf->width - 12) dw = surf->width - 12;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_RED);
        pxe_draw_text(surf, dx + 8, dy + 3, fm->modal_title, PXE_COL_WHITE);
        char mclip[128];
        utf8_clip_by_pixels(fm->modal_msg, dw - 24, 1, mclip, sizeof(mclip));
        pxe_draw_text_small(surf, dx + 12, dy + 30, mclip, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + 46,  dy + 54, 72, 22, FM_TR(fm, "删除", "Delete"), 0, 1);
        pxe_draw_button(surf, dx + 142, dy + 54, 72, 22, FM_TR(fm, "取消", "Cancel"), 0, 1);
    } else if (fm->modal == MODAL_MSG) {
        int dw = 340, dh = 88;
        if (dw > surf->width - 12) dw = surf->width - 12;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 12, dy + 12, 22, 22,
                      fm->modal_is_err ? PXE_COL_DARK_RED : PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 19, dy + 15, fm->modal_is_err ? "!" : "i", PXE_COL_WHITE);
        pxe_draw_text(surf, dx + 42, dy + 13, fm->modal_title,
                      fm->modal_is_err ? PXE_COL_DARK_RED : PXE_COL_DARK_BLUE);
        char mclip[128];
        utf8_clip_by_pixels(fm->modal_msg, dw - 52, 1, mclip, sizeof(mclip));
        pxe_draw_text_small(surf, dx + 42, dy + 34, mclip, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + dw / 2 - 32, dy + 56, 64, 22, FM_TR(fm, "确定", "OK"), 0, 1);
    } else if (fm->modal == MODAL_OPEN_WITH) {
        int dw = 340, dh = 236;
        if (dw > surf->width - 12) dw = surf->width - 12;
        if (dh > surf->height - 12) dh = surf->height - 12;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3, fm->modal_title, PXE_COL_WHITE);

        char hdr[128], hclip[128];
        snprintf(hdr, sizeof(hdr), "%s: %s",
                 FM_TR(fm, "选择打开程序", "Choose program for"), fm->modal_msg);
        utf8_clip_by_pixels(hdr, dw - 20, 1, hclip, sizeof(hclip));
        pxe_draw_text_small(surf, dx + 10, dy + 24, hclip, PXE_COL_BLACK);

        int lx = dx + 10, ly = dy + 38, lw = dw - 20, lh = OW_VISIBLE_ROWS * 16 + 4;
        pxe_draw_bevel_box(surf, lx, ly, lw - SB_W, lh, 1, PXE_COL_WHITE);
        int max_ow_scr = (fm->ow_app_count > OW_VISIBLE_ROWS)
                         ? (fm->ow_app_count - OW_VISIBLE_ROWS) : 0;
        if (fm->ow_scroll > max_ow_scr) fm->ow_scroll = max_ow_scr;
        if (fm->ow_scroll < 0) fm->ow_scroll = 0;

        for (int i = 0; i < OW_VISIBLE_ROWS && (fm->ow_scroll + i) < fm->ow_app_count; i++) {
            int aidx = fm->ow_scroll + i;
            int ry = ly + 2 + i * 16;
            int sel = (aidx == fm->ow_sel);
            if (sel) pxe_fill_rect(surf, lx + 2, ry, lw - SB_W - 4, 16, PXE_COL_BLUE);
            char row_lbl[128], row_clip[128];
            snprintf(row_lbl, sizeof(row_lbl), "%s (%s)",
                     fm->ow_apps[aidx].title, fm->ow_apps[aidx].filename);
            utf8_clip_by_pixels(row_lbl, lw - SB_W - 12, 1, row_clip, sizeof(row_clip));
            pxe_draw_text_small(surf, lx + 6, ry + 4, row_clip,
                                sel ? PXE_COL_WHITE : PXE_COL_BLACK);
        }
        pxe_draw_scrollbar(surf, lx + lw - SB_W, ly, SB_W, lh,
                           fm->ow_scroll, OW_VISIBLE_ROWS, fm->ow_app_count);

        int in_lbl_y = ly + lh + 6;
        pxe_draw_text_small(surf, dx + 10, in_lbl_y,
                            FM_TR(fm, "自定义程序路径 (.pxe):", "Custom .pxe path / name:"),
                            PXE_COL_BLACK);
        int in_y = in_lbl_y + 13;
        pxe_draw_bevel_box(surf, dx + 10, in_y, dw - 20, 20, 1, PXE_COL_WHITE);
        const char* vis_in = fm->modal_input;
        while (*vis_in && pxe_text_width_small(vis_in) > dw - 30) {
            (void)utf8_decode_next(&vis_in);
        }
        pxe_draw_text_small(surf, dx + 14, in_y + 6, vis_in, PXE_COL_BLACK);
        pxe_draw_vline(surf, dx + 14 + pxe_text_width_small(vis_in), in_y + 4, 12, PXE_COL_BLACK);

        int chk_y = in_y + 26;
        pxe_draw_bevel_box(surf, dx + 10, chk_y, 12, 12, 1,
                           fm->ow_can_save ? PXE_COL_WHITE : PXE_COL_LIGHT_GRAY);
        if (fm->ow_save_default && fm->ow_can_save) {
            pxe_fill_rect(surf, dx + 13, chk_y + 3, 6, 6, PXE_COL_DARK_BLUE);
        }
        char chk_lbl[96], chk_clip[96];
        if (fm->ow_can_save) {
            snprintf(chk_lbl, sizeof(chk_lbl),
                     FM_TR(fm, "始终使用此程序打开 .%s 文件", "Always use for .%s files"),
                     fm->ow_ext);
        } else if (fm->ow_ext[0]) {
            snprintf(chk_lbl, sizeof(chk_lbl),
                     FM_TR(fm, "始终使用 (.%s 仅可写系统盘可保存)", "Always use (read-only media)"),
                     fm->ow_ext);
        } else {
            snprintf(chk_lbl, sizeof(chk_lbl),
                     FM_TR(fm, "无扩展名文件仅支持单次打开", "No extension (one-time launch)"));
        }
        utf8_clip_by_pixels(chk_lbl, dw - 36, 1, chk_clip, sizeof(chk_clip));
        pxe_draw_text_small(surf, dx + 27, chk_y + 2, chk_clip,
                            fm->ow_can_save ? PXE_COL_BLACK : PXE_COL_DARK_GRAY);

        int btn_y = dy + dh - 28;
        pxe_draw_button(surf, dx + dw / 2 - 80, btn_y, 72, 22, FM_TR(fm, "确定", "OK"), 0, 1);
        pxe_draw_button(surf, dx + dw / 2 + 8,  btn_y, 72, 22, FM_TR(fm, "取消", "Cancel"), 0, 1);
    }
}

static void clamp_ctx_menu(fm_t* fm, const pxe_win_surface_t* surf, int mx, int my, int items) {
    fm->ctx_w = 124;
    fm->ctx_h = items * 18 + 4;
    fm->ctx_x = mx;
    fm->ctx_y = my;
    if (fm->ctx_x + fm->ctx_w > surf->width - 4) fm->ctx_x = surf->width - 4 - fm->ctx_w;
    if (fm->ctx_y + fm->ctx_h > surf->height - 4) fm->ctx_y = surf->height - 4 - fm->ctx_h;
    if (fm->ctx_x < 4) fm->ctx_x = 4;
    if (fm->ctx_y < 4) fm->ctx_y = 4;
}

static void open_initial_target(fm_t* fm, const char* path) {
    if (!path || !path[0]) {
        load_directory(fm);
        return;
    }
    pxe_dirent_t st;
    if (pxe_stat(path, &st) == 0 && !st.is_dir) {
        const char* slash = strrchr(path, '\\');
        const char* slash2 = strrchr(path, '/');
        if (slash2 && (!slash || slash2 > slash)) slash = slash2;
        if (slash) {
            size_t dlen = (size_t)(slash - path);
            if (dlen == 2 && path[1] == ':') dlen = 3;
            if (dlen >= sizeof(fm->current_path)) dlen = sizeof(fm->current_path) - 1;
            memcpy(fm->current_path, path, dlen);
            fm->current_path[dlen] = '\0';
            load_directory(fm);
            const char* base = slash + 1;
            for (int i = 0; i < fm->entry_count; i++) {
                if (strcasecmp(fm->entries[i].name, base) == 0) {
                    fm->selected = i;
                    open_entry(fm, i);
                    return;
                }
            }
            return;
        }
    }
    snprintf(fm->current_path, sizeof(fm->current_path), "%s", path);
    load_directory(fm);
}

int main(int argc, char** argv) {
    fm_t* fm = (fm_t*)calloc(1, sizeof(fm_t));
    if (!fm) return 1;
    fm->selected = -1;
    fm->last_click_idx = -1;
    fm->zh = pxe_is_zh();

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
    strcpy(desc.title, FM_TR(fm, "文件管理", "File Manager"));

    pxe_win_surface_t surf;
    fm->win_id = pxe_win_create(&desc, &surf);
    if (fm->win_id < 0) {
        free(fm);
        return 1;
    }

    sync_fm_menu(fm);

    if (argc > 1 && argv[1] && argv[1][0]) {
        open_initial_target(fm, argv[1]);
    } else {
        load_directory(fm);
    }

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
            case PXE_EV_REDRAW: {
                int new_zh = pxe_is_zh();
                if (new_zh != fm->zh) {
                    fm->zh = new_zh;
                    sync_fm_menu(fm);
                    pxe_win_set_title(fm->win_id, FM_TR(fm, "文件管理", "File Manager"));
                }
                dirty = 1;
                break;
            }
            case PXE_EV_ACTIVATE:
                if (ev.text[0]) {
                    open_initial_target(fm, ev.text);
                } else if (fm->current_path[0] == '\0') {
                    load_directory(fm);
                }
                dirty = 1;
                break;
            case PXE_EV_MOUSE_WHEEL:
                if (fm->modal == MODAL_OPEN_WITH && ev.arg0 != 0) {
                    int max_ow = (fm->ow_app_count > OW_VISIBLE_ROWS)
                                 ? (fm->ow_app_count - OW_VISIBLE_ROWS) : 0;
                    fm->ow_scroll -= ev.arg0;
                    if (fm->ow_scroll < 0) fm->ow_scroll = 0;
                    if (fm->ow_scroll > max_ow) fm->ow_scroll = max_ow;
                    dirty = 1;
                } else if (fm->modal == MODAL_NONE && fm->ctx_kind == CTX_NONE &&
                    ev.arg0 != 0) {
                    fm->scroll -= ev.arg0 * 3;
                    if (fm->scroll < 0) fm->scroll = 0;
                    if (fm->scroll > max_scr) fm->scroll = max_scr;
                    dirty = 1;
                }
                break;
            case PXE_EV_MENU:
                fm->ctx_kind = CTX_NONE;
                if (ev.arg0 == 0) {
                    if (ev.arg1 == 0 && fm->selected >= 0) open_entry(fm, fm->selected);
                    else if (ev.arg1 == 1 && fm->selected >= 0) action_cut(fm, fm->selected);
                    else if (ev.arg1 == 2 && fm->selected >= 0) action_copy(fm, fm->selected);
                    else if (ev.arg1 == 3) action_paste(fm, &surf);
                    else if (ev.arg1 == 4 && fm->selected >= 0) action_begin_rename(fm, fm->selected);
                    else if (ev.arg1 == 5) action_begin_mkdir(fm);
                    else if (ev.arg1 == 6 && fm->selected >= 0) action_begin_delete(fm, fm->selected);
                    else if (ev.arg1 == 7) action_eject(fm);
                    else if (ev.arg1 == 8) running = 0;
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
                        int dw = 280;
                        if (dw > surf.width - 12) dw = surf.width - 12;
                        int dx = (surf.width - dw) / 2;
                        int dy = (surf.height - 86) / 2;
                        if (pxe_point_in_rect(mx, my, dx + 46, dy + 54, 72, 22)) submit_modal(fm);
                        else if (pxe_point_in_rect(mx, my, dx + 142, dy + 54, 72, 22)) fm->modal = MODAL_NONE;
                    } else if (fm->modal == MODAL_MSG) {
                        int dw = 340;
                        if (dw > surf.width - 12) dw = surf.width - 12;
                        int dx = (surf.width - dw) / 2;
                        int dy = (surf.height - 88) / 2;
                        if (pxe_point_in_rect(mx, my, dx + dw / 2 - 32, dy + 56, 64, 22)) {
                            fm->modal = MODAL_NONE;
                        }
                    } else if (fm->modal == MODAL_OPEN_WITH) {
                        int dw = 340, dh = 236;
                        if (dw > surf.width - 12) dw = surf.width - 12;
                        if (dh > surf.height - 12) dh = surf.height - 12;
                        int dx = (surf.width - dw) / 2;
                        int dy = (surf.height - dh) / 2;
                        int lx = dx + 10, ly = dy + 38, lw = dw - 20, lh = OW_VISIBLE_ROWS * 16 + 4;
                        int max_ow = (fm->ow_app_count > OW_VISIBLE_ROWS)
                                     ? (fm->ow_app_count - OW_VISIBLE_ROWS) : 0;
                        int chk_y = ly + lh + 45;
                        int btn_y = dy + dh - 28;
                        if (pxe_point_in_rect(mx, my, lx + lw - SB_W, ly, SB_W, lh)) {
                            if (my < ly + lh / 2) {
                                if (fm->ow_scroll > 0) fm->ow_scroll--;
                            } else {
                                if (fm->ow_scroll < max_ow) fm->ow_scroll++;
                            }
                        } else if (pxe_point_in_rect(mx, my, lx, ly + 2, lw - SB_W, lh - 4)) {
                            int r = (my - (ly + 2)) / 16;
                            int idx = fm->ow_scroll + r;
                            if (idx >= 0 && idx < fm->ow_app_count) {
                                fm->ow_sel = idx;
                                snprintf(fm->modal_input, sizeof(fm->modal_input), "%s",
                                         fm->ow_apps[idx].filename);
                                fm->modal_len = (int)strlen(fm->modal_input);
                            }
                        } else if (fm->ow_can_save &&
                                   pxe_point_in_rect(mx, my, dx + 10, chk_y - 2, dw - 20, 16)) {
                            fm->ow_save_default = !fm->ow_save_default;
                        } else if (pxe_point_in_rect(mx, my, dx + dw / 2 - 80, btn_y, 72, 22)) {
                            submit_open_with(fm);
                        } else if (pxe_point_in_rect(mx, my, dx + dw / 2 + 8, btn_y, 72, 22)) {
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
                            else if (item == 1) action_begin_open_with(fm, tgt);
                            else if (item == 2) action_cut(fm, tgt);
                            else if (item == 3) action_copy(fm, tgt);
                            else if (item == 4) action_paste(fm, &surf);
                            else if (item == 5) action_begin_rename(fm, tgt);
                            else if (item == 6) action_begin_delete(fm, tgt);
                            else if (item == 7) action_begin_mkdir(fm);
                            else if (item == 8) action_properties(fm, tgt);
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
                    if (pxe_point_in_rect(mx, my, 4, 3, 46, 20)) {
                        fm->current_path[0] = '\0';
                        load_directory(fm);
                    } else if (pxe_point_in_rect(mx, my, 54, 3, 36, 20)) {
                        go_up(fm);
                    } else if (pxe_point_in_rect(mx, my, 94, 3, 42, 20) && fm->selected >= 0) {
                        open_entry(fm, fm->selected);
                    } else if (pxe_point_in_rect(mx, my, 140, 3, 38, 20) && fm->selected >= 0) {
                        action_copy(fm, fm->selected);
                    } else if (pxe_point_in_rect(mx, my, 182, 3, 32, 20) && fm->selected >= 0) {
                        action_cut(fm, fm->selected);
                    } else if (pxe_point_in_rect(mx, my, 218, 3, 42, 20)) {
                        action_paste(fm, &surf);
                    } else if (pxe_point_in_rect(mx, my, 264, 3, 50, 20)) {
                        action_begin_mkdir(fm);
                    } else if (pxe_point_in_rect(mx, my, 318, 3, 42, 20)) {
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
                                clamp_ctx_menu(fm, &surf, mx, my, 9);
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
                if (fm->modal == MODAL_OPEN_WITH) {
                    if (ev.key == PXE_KEY_ESC) {
                        fm->modal = MODAL_NONE;
                    } else if (ev.key == PXE_KEY_ENTER) {
                        submit_open_with(fm);
                    } else if (ev.key == PXE_KEY_UP && fm->ow_app_count > 0) {
                        if (fm->ow_sel <= 0) fm->ow_sel = 0;
                        else fm->ow_sel--;
                        if (fm->ow_sel < fm->ow_scroll) fm->ow_scroll = fm->ow_sel;
                        snprintf(fm->modal_input, sizeof(fm->modal_input), "%s",
                                 fm->ow_apps[fm->ow_sel].filename);
                        fm->modal_len = (int)strlen(fm->modal_input);
                    } else if (ev.key == PXE_KEY_DOWN && fm->ow_app_count > 0) {
                        if (fm->ow_sel < 0) fm->ow_sel = 0;
                        else if (fm->ow_sel < fm->ow_app_count - 1) fm->ow_sel++;
                        if (fm->ow_sel >= fm->ow_scroll + OW_VISIBLE_ROWS) {
                            fm->ow_scroll = fm->ow_sel - OW_VISIBLE_ROWS + 1;
                        }
                        snprintf(fm->modal_input, sizeof(fm->modal_input), "%s",
                                 fm->ow_apps[fm->ow_sel].filename);
                        fm->modal_len = (int)strlen(fm->modal_input);
                    } else if (ev.key == PXE_KEY_BACKSPACE) {
                        if (fm->modal_len > 0) {
                            int d = utf8_prev_char_bytes(fm->modal_input, fm->modal_len);
                            fm->modal_len -= d;
                            fm->modal_input[fm->modal_len] = '\0';
                        }
                    } else if (ev.key == PXE_KEY_UTF8 && ev.text[0]) {
                        int tl = (int)strlen(ev.text);
                        if (fm->modal_len + tl < 120) {
                            memcpy(fm->modal_input + fm->modal_len, ev.text, (size_t)tl);
                            fm->modal_len += tl;
                            fm->modal_input[fm->modal_len] = '\0';
                        }
                    } else if (ev.key >= 32 && ev.key <= 126 && fm->modal_len < 120) {
                        fm->modal_input[fm->modal_len++] = (char)ev.key;
                        fm->modal_input[fm->modal_len] = '\0';
                    }
                    dirty = 1;
                    break;
                }
                if (fm->modal == MODAL_RENAME || fm->modal == MODAL_MKDIR) {
                    if (ev.key == PXE_KEY_ESC) {
                        fm->modal = MODAL_NONE;
                    } else if (ev.key == PXE_KEY_ENTER) {
                        submit_modal(fm);
                    } else if (ev.key == PXE_KEY_BACKSPACE) {
                        if (fm->modal_len > 0) {
                            int d = utf8_prev_char_bytes(fm->modal_input, fm->modal_len);
                            fm->modal_len -= d;
                            fm->modal_input[fm->modal_len] = '\0';
                        }
                    } else if (ev.key == PXE_KEY_UTF8 && ev.text[0]) {
                        int tl = (int)strlen(ev.text);
                        if (fm->modal_len + tl < 120) {
                            memcpy(fm->modal_input + fm->modal_len, ev.text, (size_t)tl);
                            fm->modal_len += tl;
                            fm->modal_input[fm->modal_len] = '\0';
                        }
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
                    } else if ((ev.key == 'x' || ev.key == 'X') &&
                               fm->selected >= 0) {
                        action_cut(fm, fm->selected);
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
