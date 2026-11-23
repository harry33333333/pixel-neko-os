#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/fs.h>
#include <pxe/mem.h>
#include <pxe/str.h>

#define EDITOR_MAX 65536
#define MODAL_NONE    0
#define MODAL_OPEN    1
#define MODAL_SAVE_AS 2
#define MODAL_ERROR   3

typedef struct {
    char* text;
    int   len;
    int   cursor;
    int   scroll_x; /* in 8px horizontal units */
    int   scroll_y;
    int   sel_start;
    int   sel_end;
    int   dragging;
    int   modified;
    char  filename[256];

    int   modal;
    char  modal_input[256];
    int   modal_len;
    char  err_msg[64];
    int   btn_ok_pressed;
    int   btn_cancel_pressed;
    int   win_id;
    int   zh;
} notepad_t;

#define NP_TR(ed, zh_s, en_s) ((ed)->zh ? (zh_s) : (en_s))

static void sync_notepad_menu(notepad_t* ed) {
    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 2;
    strcpy(mb.menus[0].title, NP_TR(ed, "文件", "File"));
    mb.menus[0].item_count = 5;
    strcpy(mb.menus[0].items[0].label, NP_TR(ed, "新建", "New"));          mb.menus[0].items[0].enabled = 1;
    strcpy(mb.menus[0].items[1].label, NP_TR(ed, "打开...", "Open..."));   mb.menus[0].items[1].enabled = 1;
    strcpy(mb.menus[0].items[2].label, NP_TR(ed, "保存", "Save"));         mb.menus[0].items[2].enabled = 1;
    strcpy(mb.menus[0].items[3].label, NP_TR(ed, "另存为...", "Save As...")); mb.menus[0].items[3].enabled = 1;
    strcpy(mb.menus[0].items[4].label, NP_TR(ed, "退出", "Exit"));         mb.menus[0].items[4].enabled = 1;

    strcpy(mb.menus[1].title, NP_TR(ed, "编辑", "Edit"));
    mb.menus[1].item_count = 5;
    strcpy(mb.menus[1].items[0].label, NP_TR(ed, "剪切", "Cut"));          mb.menus[1].items[0].enabled = 1;
    strcpy(mb.menus[1].items[1].label, NP_TR(ed, "复制", "Copy"));         mb.menus[1].items[1].enabled = 1;
    strcpy(mb.menus[1].items[2].label, NP_TR(ed, "粘贴", "Paste"));        mb.menus[1].items[2].enabled = 1;
    strcpy(mb.menus[1].items[3].label, NP_TR(ed, "删除", "Delete"));       mb.menus[1].items[3].enabled = 1;
    strcpy(mb.menus[1].items[4].label, NP_TR(ed, "全选", "Select All"));   mb.menus[1].items[4].enabled = 1;
    pxe_win_set_menu(ed->win_id, &mb);
}

static void update_title(notepad_t* ed) {
    char title[48];
    const char* name = NP_TR(ed, "未命名", "Untitled");
    if (ed->filename[0]) {
        const char* slash = ed->filename;
        for (const char* p = ed->filename; *p; p++) {
            if (*p == '\\' || *p == '/') slash = p + 1;
        }
        if (*slash) name = slash;
    }
    char prefix[36];
    snprintf(prefix, sizeof(prefix), "%s - %s%s",
             NP_TR(ed, "记事本", "Notepad"), name, ed->modified ? "*" : "");
    utf8_clip_by_pixels(prefix, 280, 0, title, sizeof(title));
    pxe_win_set_title(ed->win_id, title);
}

static void ed_clear_doc(notepad_t* ed) {
    ed->len = 0;
    ed->text[0] = '\0';
    ed->cursor = 0;
    ed->scroll_x = 0;
    ed->scroll_y = 0;
    ed->sel_start = -1;
    ed->sel_end = -1;
    ed->modified = 0;
    ed->filename[0] = '\0';
    update_title(ed);
}

static void ed_open_file(notepad_t* ed, const char* path) {
    if (!path || !path[0]) return;
    pxe_dirent_t st;
    if (pxe_stat(path, &st) != 0 || st.is_dir) {
        ed->modal = MODAL_ERROR;
        snprintf(ed->err_msg, sizeof(ed->err_msg), "%s",
                 NP_TR(ed, "无法打开指定文件。", "Cannot open file."));
        pxe_beep();
        return;
    }
    int to_read = (st.size < (uint64_t)(EDITOR_MAX - 1)) ? (int)st.size : (EDITOR_MAX - 1);
    int n = pxe_read_file(path, ed->text, (size_t)to_read);
    if (n < 0) {
        ed->modal = MODAL_ERROR;
        snprintf(ed->err_msg, sizeof(ed->err_msg), "%s",
                 NP_TR(ed, "读取文件失败。", "Read failed."));
        pxe_beep();
        return;
    }
    /* Strip UTF-8 BOM if present and normalize CRLF -> LF */
    int start = 0;
    if (n >= 3 && (unsigned char)ed->text[0] == 0xEFu &&
        (unsigned char)ed->text[1] == 0xBBu && (unsigned char)ed->text[2] == 0xBFu) {
        start = 3;
    }
    int out = 0;
    for (int i = start; i < n; i++) {
        if (ed->text[i] == '\r') {
            if (i + 1 < n && ed->text[i + 1] == '\n') continue;
            ed->text[out++] = '\n';
        } else {
            ed->text[out++] = ed->text[i];
        }
    }
    ed->len = out;
    ed->text[out] = '\0';
    ed->cursor = 0;
    ed->scroll_x = 0;
    ed->scroll_y = 0;
    ed->sel_start = -1;
    ed->sel_end = -1;
    ed->modified = 0;
    strncpy(ed->filename, path, sizeof(ed->filename) - 1);
    ed->filename[sizeof(ed->filename) - 1] = '\0';
    ed->modal = MODAL_NONE;
    update_title(ed);
}

static void ed_save_to(notepad_t* ed, const char* path) {
    if (!path || !path[0]) return;
    int wr = pxe_write_file(path, ed->text, (size_t)ed->len);
    if (wr < 0) {
        ed->modal = MODAL_ERROR;
        if (wr == -8) {
            snprintf(ed->err_msg, sizeof(ed->err_msg), "%s",
                     NP_TR(ed, "系统运行日志为只读文件！", "Active log is read-only!"));
        } else {
            snprintf(ed->err_msg, sizeof(ed->err_msg), "%s",
                     NP_TR(ed, "保存文件失败！", "Failed to save file!"));
        }
        pxe_beep();
        return;
    }
    strncpy(ed->filename, path, sizeof(ed->filename) - 1);
    ed->filename[sizeof(ed->filename) - 1] = '\0';
    ed->modified = 0;
    ed->modal = MODAL_NONE;
    update_title(ed);
}

static void ed_open_modal_path(notepad_t* ed, int kind) {
    ed->modal = kind;
    ed->btn_ok_pressed = 0;
    ed->btn_cancel_pressed = 0;
    if ((kind == MODAL_SAVE_AS || kind == MODAL_OPEN) && ed->filename[0]) {
        strncpy(ed->modal_input, ed->filename, sizeof(ed->modal_input) - 1);
        ed->modal_input[sizeof(ed->modal_input) - 1] = '\0';
    } else {
        pxe_basic_sysinfo_t info;
        char drv = 'C';
        if (pxe_sys_get_basic_info(&info) == 0 && info.system_drive) {
            drv = info.system_drive;
        }
        snprintf(ed->modal_input, sizeof(ed->modal_input), "%c:\\NOTE.TXT", drv);
    }
    ed->modal_len = (int)strlen(ed->modal_input);
}

static int has_selection(const notepad_t* ed) {
    return ed->sel_start >= 0 && ed->sel_end >= 0 && ed->sel_start != ed->sel_end;
}

static void delete_selection(notepad_t* ed) {
    if (!has_selection(ed)) return;
    int s = (ed->sel_start < ed->sel_end) ? ed->sel_start : ed->sel_end;
    int e = (ed->sel_start > ed->sel_end) ? ed->sel_start : ed->sel_end;
    int d = e - s;
    memmove(ed->text + s, ed->text + e, (size_t)(ed->len - e + 1));
    ed->len -= d;
    ed->cursor = s;
    ed->sel_start = -1;
    ed->sel_end = -1;
    ed->modified = 1;
    update_title(ed);
}

static void insert_utf8_text(notepad_t* ed, const char* s) {
    if (!s || !s[0]) return;
    if (has_selection(ed)) delete_selection(ed);
    int slen = (int)strlen(s);
    if (ed->len + slen >= EDITOR_MAX - 1) return;
    memmove(ed->text + ed->cursor + slen, ed->text + ed->cursor,
            (size_t)(ed->len - ed->cursor + 1));
    memcpy(ed->text + ed->cursor, s, (size_t)slen);
    ed->cursor += slen;
    ed->len += slen;
    ed->modified = 1;
    update_title(ed);
}

static void insert_char(notepad_t* ed, char c) {
    char tmp[2] = { c, '\0' };
    insert_utf8_text(ed, tmp);
}

static int get_line_start(const notepad_t* ed, int pos) {
    while (pos > 0 && ed->text[pos - 1] != '\n') pos--;
    return pos;
}

static int get_line_end(const notepad_t* ed, int pos) {
    while (pos < ed->len && ed->text[pos] != '\n') pos++;
    return pos;
}

static int count_total_lines(const notepad_t* ed) {
    int lines = 1;
    for (int i = 0; i < ed->len; i++) {
        if (ed->text[i] == '\n') lines++;
    }
    return lines;
}

static void get_px_line_from_pos(const notepad_t* ed, int pos, int* out_px, int* out_y, int* out_col) {
    int px = 0, y = 0, col = 0;
    const char* base = ed->text;
    const char* limit = ed->text + ((pos < ed->len) ? pos : ed->len);
    const char* p = base;
    while (p < limit && *p) {
        if (*p == '\n') {
            y++;
            px = 0;
            col = 0;
            p++;
        } else if (*p == '\t') {
            int cols = 4 - (col & 3);
            col += cols;
            px += cols * 8;
            p++;
        } else {
            uint32_t cp = utf8_decode_next(&p);
            int w = (cp < 0x80u) ? 8 : 16;
            px += w;
            col += (cp < 0x80u) ? 1 : 2;
        }
    }
    if (out_px) *out_px = px;
    if (out_y) *out_y = y;
    if (out_col) *out_col = col;
}

static int get_pos_from_px_line(const notepad_t* ed, int target_px, int ty) {
    if (ty < 0) return 0;
    int px = 0, cy = 0, col = 0;
    const char* base = ed->text;
    const char* end = ed->text + ed->len;
    const char* p = base;
    while (p < end && *p) {
        int byte_idx = (int)(p - base);
        if (cy == ty && px >= target_px) return byte_idx;
        if (*p == '\n') {
            if (cy == ty) return byte_idx;
            cy++;
            px = 0;
            col = 0;
            p++;
        } else if (*p == '\t') {
            int cols = 4 - (col & 3);
            int w = cols * 8;
            if (cy == ty && px + w / 2 >= target_px) return byte_idx;
            col += cols;
            px += w;
            p++;
        } else {
            const char* prev = p;
            uint32_t cp = utf8_decode_next(&p);
            int w = (cp < 0x80u) ? 8 : 16;
            if (cy == ty && px + w / 2 >= target_px) return (int)(prev - base);
            px += w;
            col += (cp < 0x80u) ? 1 : 2;
        }
    }
    return ed->len;
}

static void ed_copy(const notepad_t* ed) {
    if (!has_selection(ed)) return;
    int s = (ed->sel_start < ed->sel_end) ? ed->sel_start : ed->sel_end;
    int e = (ed->sel_start > ed->sel_end) ? ed->sel_start : ed->sel_end;
    int len = e - s;
    char tmp[4096];
    if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
    memcpy(tmp, ed->text + s, (size_t)len);
    tmp[len] = '\0';
    pxe_clipboard_set(tmp);
}

static void ed_cut(notepad_t* ed) {
    if (!has_selection(ed)) return;
    ed_copy(ed);
    delete_selection(ed);
}

static void ed_paste(notepad_t* ed) {
    char tmp[4096];
    if (pxe_clipboard_get(tmp, sizeof(tmp)) <= 0) return;
    if (has_selection(ed)) delete_selection(ed);
    int out = 0;
    for (int i = 0; tmp[i]; i++) {
        if (tmp[i] != '\r') tmp[out++] = tmp[i];
    }
    tmp[out] = '\0';
    insert_utf8_text(ed, tmp);
}

static void ensure_cursor_visible(notepad_t* ed, const pxe_win_surface_t* surf) {
    int text_w = surf->width - 14 - 4;
    int text_h = surf->height - 22;
    int vis_lines = text_h / 16;
    if (vis_lines < 1) vis_lines = 1;

    int cpx, cy;
    get_px_line_from_pos(ed, ed->cursor, &cpx, &cy, NULL);
    if (cy < ed->scroll_y) ed->scroll_y = cy;
    if (cy >= ed->scroll_y + vis_lines) ed->scroll_y = cy - vis_lines + 1;
    int sc_px = ed->scroll_x * 8;
    if (cpx < sc_px) ed->scroll_x = cpx / 8;
    if (cpx + 16 > sc_px + text_w) {
        ed->scroll_x = (cpx + 16 - text_w + 7) / 8;
        if (ed->scroll_x < 0) ed->scroll_x = 0;
    }
}

static void render_notepad(const pxe_win_surface_t* surf, const notepad_t* ed) {
    int sb_w = 14;
    int text_w = surf->width - sb_w;
    int text_h = surf->height - 22;
    if (text_w < 16) text_w = 16;
    if (text_h < 16) text_h = 16;

    pxe_fill_rect(surf, 0, 0, text_w, text_h, PXE_COL_WHITE);

    int s1 = -1, s2 = -1;
    if (has_selection(ed)) {
        s1 = (ed->sel_start < ed->sel_end) ? ed->sel_start : ed->sel_end;
        s2 = (ed->sel_start > ed->sel_end) ? ed->sel_start : ed->sel_end;
    }

    int cur_px = 0, cur_y = 0, cur_col = 0;
    int i = 0;
    while (i <= ed->len) {
        int px = 2 + cur_px - ed->scroll_x * 8;
        int py = 2 + (cur_y - ed->scroll_y) * 16;

        int byte_adv = 1;
        int glyph_w = 8;
        uint32_t cp = 0;
        if (i < ed->len) {
            if (ed->text[i] == '\t') {
                int cols = 4 - (cur_col & 3);
                glyph_w = cols * 8;
                byte_adv = 1;
            } else if (ed->text[i] == '\n') {
                glyph_w = 8;
                byte_adv = 1;
            } else {
                const char* p = ed->text + i;
                cp = utf8_decode_next(&p);
                byte_adv = (int)(p - (ed->text + i));
                if (byte_adv < 1) byte_adv = 1;
                glyph_w = (cp < 0x80u) ? 8 : 16;
            }
        }

        if (i < ed->len && s1 >= 0 && i >= s1 && i < s2) {
            if (py >= 0 && py + 16 <= text_h && px + glyph_w > 0 && px < text_w) {
                int hx = (px < 0) ? 0 : px;
                int hw = px + glyph_w - hx;
                if (hx + hw > text_w) hw = text_w - hx;
                if (hw > 0) pxe_fill_rect(surf, hx, py, hw, 16, PXE_COL_BLUE);
            }
        }

        if (i == ed->cursor && ed->modal == MODAL_NONE) {
            if (px >= 0 && px < text_w && py >= 0 && py + 16 <= text_h) {
                pxe_draw_vline(surf, px, py, 16, PXE_COL_BLACK);
            }
        }

        if (i >= ed->len) break;

        char c = ed->text[i];
        if (c == '\n') {
            cur_y++;
            cur_px = 0;
            cur_col = 0;
        } else if (c == '\t') {
            cur_col += glyph_w / 8;
            cur_px += glyph_w;
        } else {
            uint32_t fg = (s1 >= 0 && i >= s1 && i < s2) ? PXE_COL_WHITE : PXE_COL_BLACK;
            if (px >= 0 && px + glyph_w <= text_w && py >= 0 && py + 16 <= text_h) {
                if (cp < 0x80u) {
                    pxe_draw_char(surf, px, py, (char)cp, fg);
                } else {
                    char ubuf[5];
                    int ul = utf8_encode_one(cp, ubuf);
                    ubuf[ul] = '\0';
                    pxe_draw_text(surf, px, py, ubuf, fg);
                }
            }
            cur_px += glyph_w;
            cur_col += (cp < 0x80u) ? 1 : 2;
        }
        i += byte_adv;
    }

    /* Vertical scrollbar */
    int vis_lines = text_h / 16;
    if (vis_lines < 1) vis_lines = 1;
    int total_lines = count_total_lines(ed);
    pxe_draw_scrollbar(surf, text_w, 0, sb_w, text_h, ed->scroll_y, vis_lines, total_lines);

    /* Status bar */
    int sby = text_h;
    pxe_fill_rect(surf, 0, sby, surf->width, surf->height - sby, PXE_COL_LIGHT_GRAY);
    pxe_draw_hline(surf, 0, sby, surf->width, PXE_COL_DARK_GRAY);
    pxe_draw_hline(surf, 0, sby + 1, surf->width, PXE_COL_WHITE);

    int cpx, cy, col;
    get_px_line_from_pos(ed, ed->cursor, &cpx, &cy, &col);
    char status[160], sclip[160];
    if (ed->zh) {
        snprintf(status, sizeof(status), "%s%s | 第 %d 行, 第 %d 列",
                 ed->filename[0] ? ed->filename : "未命名",
                 ed->modified ? "*" : "", cy + 1, col + 1);
    } else {
        snprintf(status, sizeof(status), "%s%s | Ln %d, Col %d",
                 ed->filename[0] ? ed->filename : "Untitled",
                 ed->modified ? "*" : "", cy + 1, col + 1);
    }
    utf8_clip_by_pixels(status, surf->width - 12, 1, sclip, sizeof(sclip));
    pxe_draw_text_small(surf, 6, sby + 5, sclip, PXE_COL_BLACK);

    /* Modal dialogs */
    if (ed->modal == MODAL_OPEN || ed->modal == MODAL_SAVE_AS) {
        int dw = 260, dh = 96;
        if (dw > surf->width - 12) dw = surf->width - 12;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3,
                      (ed->modal == MODAL_OPEN) ? NP_TR(ed, "打开文件", "Open File")
                                                : NP_TR(ed, "另存为", "Save As"),
                      PXE_COL_WHITE);
        pxe_draw_text_small(surf, dx + 10, dy + 23,
                            NP_TR(ed, "文件路径 (例如 C:\\NOTE.TXT):", "Path (e.g. C:\\NOTE.TXT):"),
                            PXE_COL_BLACK);
        pxe_draw_bevel_box(surf, dx + 10, dy + 36, dw - 20, 22, 1, PXE_COL_WHITE);

        const char* vis_src = ed->modal_input;
        int box_px = dw - 28;
        while (*vis_src && pxe_text_width(vis_src) > box_px) {
            (void)utf8_decode_next(&vis_src);
        }
        pxe_draw_text(surf, dx + 14, dy + 39, vis_src, PXE_COL_BLACK);
        int cur_draw_px = dx + 14 + pxe_text_width(vis_src);
        pxe_draw_vline(surf, cur_draw_px, dy + 39, 16, PXE_COL_BLACK);

        pxe_draw_button(surf, dx + dw / 2 - 74, dy + 66, 66, 22,
                        NP_TR(ed, "确定", "OK"), ed->btn_ok_pressed, 1);
        pxe_draw_button(surf, dx + dw / 2 + 8,  dy + 66, 66, 22,
                        NP_TR(ed, "取消", "Cancel"), ed->btn_cancel_pressed, 1);
    } else if (ed->modal == MODAL_ERROR) {
        int dw = 260, dh = 86;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 12, dy + 12, 22, 22, PXE_COL_DARK_RED);
        pxe_draw_text(surf, dx + 19, dy + 15, "!", PXE_COL_WHITE);
        pxe_draw_text(surf, dx + 42, dy + 12, NP_TR(ed, "文件错误", "File Error"), PXE_COL_DARK_RED);
        pxe_draw_text_small(surf, dx + 42, dy + 34, ed->err_msg, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + dw / 2 - 30, dy + 56, 60, 22,
                        NP_TR(ed, "确定", "OK"), ed->btn_ok_pressed, 1);
    }
}

static void handle_modal_mouse_down(notepad_t* ed, const pxe_win_surface_t* surf, int mx, int my) {
    if (ed->modal == MODAL_ERROR) {
        int dw = 260, dh = 86;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        if (pxe_point_in_rect(mx, my, dx + dw / 2 - 30, dy + 56, 60, 22)) {
            ed->btn_ok_pressed = 1;
        }
        return;
    }
    int dw = 260, dh = 96;
    if (dw > surf->width - 12) dw = surf->width - 12;
    int dx = (surf->width - dw) / 2;
    int dy = (surf->height - dh) / 2;
    if (pxe_point_in_rect(mx, my, dx + dw / 2 - 74, dy + 66, 66, 22)) {
        ed->btn_ok_pressed = 1;
    } else if (pxe_point_in_rect(mx, my, dx + dw / 2 + 8, dy + 66, 66, 22)) {
        ed->btn_cancel_pressed = 1;
    }
}

static void handle_modal_mouse_up(notepad_t* ed, const pxe_win_surface_t* surf, int mx, int my) {
    if (ed->modal == MODAL_ERROR) {
        int dw = 260, dh = 86;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        if (ed->btn_ok_pressed && pxe_point_in_rect(mx, my, dx + dw / 2 - 30, dy + 56, 60, 22)) {
            ed->modal = MODAL_NONE;
        }
        ed->btn_ok_pressed = 0;
        return;
    }
    int dw = 260, dh = 96;
    if (dw > surf->width - 12) dw = surf->width - 12;
    int dx = (surf->width - dw) / 2;
    int dy = (surf->height - dh) / 2;
    int ok_hit = ed->btn_ok_pressed && pxe_point_in_rect(mx, my, dx + dw / 2 - 74, dy + 66, 66, 22);
    int cancel_hit = ed->btn_cancel_pressed && pxe_point_in_rect(mx, my, dx + dw / 2 + 8, dy + 66, 66, 22);
    ed->btn_ok_pressed = 0;
    ed->btn_cancel_pressed = 0;

    if (cancel_hit) {
        ed->modal = MODAL_NONE;
    } else if (ok_hit && ed->modal_len > 0) {
        int kind = ed->modal;
        ed->modal = MODAL_NONE;
        if (kind == MODAL_OPEN) ed_open_file(ed, ed->modal_input);
        else if (kind == MODAL_SAVE_AS) ed_save_to(ed, ed->modal_input);
    }
}

int main(int argc, char** argv) {
    notepad_t* ed = (notepad_t*)calloc(1, sizeof(notepad_t));
    if (!ed) return 1;
    ed->text = (char*)malloc(EDITOR_MAX);
    if (!ed->text) {
        free(ed);
        return 1;
    }
    ed->text[0] = '\0';
    ed->sel_start = -1;
    ed->sel_end = -1;
    ed->zh = pxe_is_zh();

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 90;
    desc.y = 48;
    desc.w = 420;
    desc.h = 300;
    desc.min_w = 240;
    desc.min_h = 180;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_RESIZABLE;
    strcpy(desc.title, NP_TR(ed, "记事本 - 未命名", "Notepad - Untitled"));

    pxe_win_surface_t surf;
    ed->win_id = pxe_win_create(&desc, &surf);
    if (ed->win_id < 0) {
        free(ed->text);
        free(ed);
        return 1;
    }

    sync_notepad_menu(ed);

    if (argc > 1 && argv[1] && argv[1][0]) {
        ed_open_file(ed, argv[1]);
    } else {
        update_title(ed);
    }

    render_notepad(&surf, ed);
    pxe_win_present(ed->win_id);

    int running = 1;
    while (running) {
        pxe_event_t ev;
        int rc = pxe_win_wait_event(ed->win_id, &ev);
        if (rc < 0) break;
        if (rc == 0) continue;

        int dirty = 0;
        int text_w = surf.width - 14;
        int text_h = surf.height - 22;

        switch (ev.type) {
            case PXE_EV_CLOSE:
                running = 0;
                break;
            case PXE_EV_RESIZE:
                if (pxe_win_get_surface(ed->win_id, &surf) == 0) {
                    ensure_cursor_visible(ed, &surf);
                    dirty = 1;
                }
                break;
            case PXE_EV_REDRAW: {
                int new_zh = pxe_is_zh();
                if (new_zh != ed->zh) {
                    ed->zh = new_zh;
                    sync_notepad_menu(ed);
                    update_title(ed);
                }
                dirty = 1;
                break;
            }
            case PXE_EV_ACTIVATE:
                if (ev.text[0]) ed_open_file(ed, ev.text);
                dirty = 1;
                break;
            case PXE_EV_MENU:
                if (ev.arg0 == 0) {
                    if (ev.arg1 == 0) ed_clear_doc(ed);
                    else if (ev.arg1 == 1) ed_open_modal_path(ed, MODAL_OPEN);
                    else if (ev.arg1 == 2) {
                        if (ed->filename[0]) ed_save_to(ed, ed->filename);
                        else ed_open_modal_path(ed, MODAL_SAVE_AS);
                    } else if (ev.arg1 == 3) ed_open_modal_path(ed, MODAL_SAVE_AS);
                    else if (ev.arg1 == 4) running = 0;
                } else if (ev.arg0 == 1) {
                    if (ev.arg1 == 0) ed_cut(ed);
                    else if (ev.arg1 == 1) ed_copy(ed);
                    else if (ev.arg1 == 2) ed_paste(ed);
                    else if (ev.arg1 == 3) delete_selection(ed);
                    else if (ev.arg1 == 4) {
                        ed->sel_start = 0;
                        ed->sel_end = ed->len;
                        ed->cursor = ed->len;
                    }
                    ensure_cursor_visible(ed, &surf);
                }
                dirty = 1;
                break;
            case PXE_EV_MOUSE_DOWN:
                if (ev.buttons & PXE_BTN_LEFT) {
                    if (ed->modal != MODAL_NONE) {
                        handle_modal_mouse_down(ed, &surf, ev.x, ev.y);
                        dirty = 1;
                    } else if (ev.x >= text_w && ev.y < text_h) {
                        int vis_lines = text_h / 16;
                        if (vis_lines < 1) vis_lines = 1;
                        int total_lines = count_total_lines(ed);
                        int max_scr = (total_lines > vis_lines) ? (total_lines - vis_lines) : 0;
                        if (ev.y < 14) {
                            if (ed->scroll_y > 0) ed->scroll_y--;
                        } else if (ev.y >= text_h - 14) {
                            if (ed->scroll_y < max_scr) ed->scroll_y++;
                        } else {
                            if (ev.y < text_h / 2) ed->scroll_y -= vis_lines;
                            else ed->scroll_y += vis_lines;
                            if (ed->scroll_y < 0) ed->scroll_y = 0;
                            if (ed->scroll_y > max_scr) ed->scroll_y = max_scr;
                        }
                        dirty = 1;
                    } else if (ev.y < text_h) {
                        int tpx = ed->scroll_x * 8 + (ev.x - 2);
                        int ty = ed->scroll_y + (ev.y - 2) / 16;
                        if (tpx < 0) tpx = 0;
                        if (ty < 0) ty = 0;
                        ed->cursor = get_pos_from_px_line(ed, tpx, ty);
                        ed->sel_start = ed->cursor;
                        ed->sel_end = ed->cursor;
                        ed->dragging = 1;
                        dirty = 1;
                    }
                }
                break;
            case PXE_EV_MOUSE_MOVE:
                if (ed->dragging && (ev.buttons & PXE_BTN_LEFT) && ed->modal == MODAL_NONE) {
                    int tpx = ed->scroll_x * 8 + (ev.x - 2);
                    int ty = ed->scroll_y + (ev.y - 2) / 16;
                    if (tpx < 0) tpx = 0;
                    if (ty < 0) ty = 0;
                    int pos = get_pos_from_px_line(ed, tpx, ty);
                    if (pos != ed->sel_end) {
                        ed->sel_end = pos;
                        ed->cursor = pos;
                        dirty = 1;
                    }
                }
                break;
            case PXE_EV_MOUSE_UP:
                if (ed->modal != MODAL_NONE) {
                    handle_modal_mouse_up(ed, &surf, ev.x, ev.y);
                    dirty = 1;
                } else if (ed->dragging) {
                    ed->dragging = 0;
                    if (ed->sel_start == ed->sel_end) {
                        ed->sel_start = -1;
                        ed->sel_end = -1;
                    }
                    dirty = 1;
                }
                break;
            case PXE_EV_MOUSE_WHEEL:
                if (ed->modal == MODAL_NONE && ev.arg0 != 0) {
                    int vis_lines = text_h / 16;
                    if (vis_lines < 1) vis_lines = 1;
                    int total_lines = count_total_lines(ed);
                    int max_scr = (total_lines > vis_lines) ?
                                  (total_lines - vis_lines) : 0;
                    ed->scroll_y -= ev.arg0 * 3;
                    if (ed->scroll_y < 0) ed->scroll_y = 0;
                    if (ed->scroll_y > max_scr) ed->scroll_y = max_scr;
                    dirty = 1;
                }
                break;
            case PXE_EV_KEY_DOWN:
                if (ed->modal == MODAL_ERROR) {
                    if (ev.key == PXE_KEY_ESC || ev.key == PXE_KEY_ENTER || ev.key == ' ') {
                        ed->modal = MODAL_NONE;
                        dirty = 1;
                    }
                    break;
                }
                if (ed->modal != MODAL_NONE) {
                    if (ev.key == PXE_KEY_ESC) {
                        ed->modal = MODAL_NONE;
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_ENTER) {
                        if (ed->modal_len > 0) {
                            int kind = ed->modal;
                            ed->modal = MODAL_NONE;
                            if (kind == MODAL_OPEN) ed_open_file(ed, ed->modal_input);
                            else if (kind == MODAL_SAVE_AS) ed_save_to(ed, ed->modal_input);
                            dirty = 1;
                        }
                    } else if (ev.key == PXE_KEY_BACKSPACE) {
                        if (ed->modal_len > 0) {
                            int d = utf8_prev_char_bytes(ed->modal_input, ed->modal_len);
                            ed->modal_len -= d;
                            ed->modal_input[ed->modal_len] = '\0';
                            dirty = 1;
                        }
                    } else if (ev.key == PXE_KEY_UTF8 && ev.text[0]) {
                        int tl = (int)strlen(ev.text);
                        if (ed->modal_len + tl < 250) {
                            memcpy(ed->modal_input + ed->modal_len, ev.text, (size_t)tl);
                            ed->modal_len += tl;
                            ed->modal_input[ed->modal_len] = '\0';
                            dirty = 1;
                        }
                    } else if (ev.key >= 32 && ev.key <= 126 && ed->modal_len < 250) {
                        ed->modal_input[ed->modal_len++] = (char)ev.key;
                        ed->modal_input[ed->modal_len] = '\0';
                        dirty = 1;
                    }
                    break;
                }

                if (ev.modifiers & PXE_MOD_CTRL) {
                    if (ev.key == 's' || ev.key == 'S') {
                        if (ed->filename[0]) ed_save_to(ed, ed->filename);
                        else ed_open_modal_path(ed, MODAL_SAVE_AS);
                        dirty = 1;
                    } else if (ev.key == 'o' || ev.key == 'O') {
                        ed_open_modal_path(ed, MODAL_OPEN);
                        dirty = 1;
                    } else if (ev.key == 'n' || ev.key == 'N') {
                        ed_clear_doc(ed);
                        dirty = 1;
                    } else if (ev.key == 'a' || ev.key == 'A') {
                        ed->sel_start = 0;
                        ed->sel_end = ed->len;
                        ed->cursor = ed->len;
                        dirty = 1;
                    } else if (ev.key == 'c' || ev.key == 'C') {
                        ed_copy(ed);
                    } else if (ev.key == 'x' || ev.key == 'X') {
                        ed_cut(ed);
                        ensure_cursor_visible(ed, &surf);
                        dirty = 1;
                    } else if (ev.key == 'v' || ev.key == 'V') {
                        ed_paste(ed);
                        ensure_cursor_visible(ed, &surf);
                        dirty = 1;
                    }
                    break;
                }

                if (ev.key == PXE_KEY_UP) {
                    int cpx, cy;
                    get_px_line_from_pos(ed, ed->cursor, &cpx, &cy, NULL);
                    if (cy > 0) ed->cursor = get_pos_from_px_line(ed, cpx, cy - 1);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_DOWN) {
                    int cpx, cy;
                    get_px_line_from_pos(ed, ed->cursor, &cpx, &cy, NULL);
                    ed->cursor = get_pos_from_px_line(ed, cpx, cy + 1);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_LEFT) {
                    if (ed->cursor > 0) ed->cursor -= utf8_prev_char_bytes(ed->text, ed->cursor);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_RIGHT) {
                    if (ed->cursor < ed->len) ed->cursor += utf8_next_char_bytes(ed->text, ed->cursor, ed->len);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_HOME) {
                    ed->cursor = get_line_start(ed, ed->cursor);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_END) {
                    ed->cursor = get_line_end(ed, ed->cursor);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_PGUP) {
                    int cpx, cy;
                    int step = (text_h / 16 > 1) ? (text_h / 16) : 1;
                    get_px_line_from_pos(ed, ed->cursor, &cpx, &cy, NULL);
                    int ny = cy - step;
                    if (ny < 0) ny = 0;
                    ed->cursor = get_pos_from_px_line(ed, cpx, ny);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_PGDN) {
                    int cpx, cy;
                    int step = (text_h / 16 > 1) ? (text_h / 16) : 1;
                    get_px_line_from_pos(ed, ed->cursor, &cpx, &cy, NULL);
                    ed->cursor = get_pos_from_px_line(ed, cpx, cy + step);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_BACKSPACE) {
                    if (has_selection(ed)) {
                        delete_selection(ed);
                    } else if (ed->cursor > 0) {
                        int d = utf8_prev_char_bytes(ed->text, ed->cursor);
                        memmove(ed->text + ed->cursor - d, ed->text + ed->cursor,
                                (size_t)(ed->len - ed->cursor + 1));
                        ed->cursor -= d;
                        ed->len -= d;
                        ed->modified = 1;
                        update_title(ed);
                    }
                } else if (ev.key == PXE_KEY_DELETE) {
                    if (has_selection(ed)) {
                        delete_selection(ed);
                    } else if (ed->cursor < ed->len) {
                        int d = utf8_next_char_bytes(ed->text, ed->cursor, ed->len);
                        memmove(ed->text + ed->cursor, ed->text + ed->cursor + d,
                                (size_t)(ed->len - ed->cursor - d + 1));
                        ed->len -= d;
                        ed->modified = 1;
                        update_title(ed);
                    }
                } else if (ev.key == PXE_KEY_ENTER) {
                    insert_char(ed, '\n');
                } else if (ev.key == PXE_KEY_TAB) {
                    insert_char(ed, '\t');
                } else if (ev.key == PXE_KEY_UTF8 && ev.text[0]) {
                    insert_utf8_text(ed, ev.text);
                } else if (ev.key >= 32 && ev.key <= 126) {
                    insert_char(ed, (char)ev.key);
                } else {
                    break;
                }
                ensure_cursor_visible(ed, &surf);
                dirty = 1;
                break;
            default:
                break;
        }

        if (dirty && running) {
            render_notepad(&surf, ed);
            pxe_win_present(ed->win_id);
        }
    }

    pxe_win_destroy(ed->win_id);
    free(ed->text);
    free(ed);
    return 0;
}
