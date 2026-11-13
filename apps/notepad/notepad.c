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
    int   scroll_x;
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
} notepad_t;

static void update_title(notepad_t* ed) {
    char title[64];
    const char* name = "Untitled";
    if (ed->filename[0]) {
        const char* slash = ed->filename;
        for (const char* p = ed->filename; *p; p++) {
            if (*p == '\\' || *p == '/') slash = p + 1;
        }
        if (*slash) name = slash;
    }
    snprintf(title, sizeof(title), "Notepad - %s%s", name, ed->modified ? "*" : "");
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
        snprintf(ed->err_msg, sizeof(ed->err_msg), "Cannot open file.");
        pxe_beep();
        return;
    }
    int to_read = (st.size < (uint64_t)(EDITOR_MAX - 1)) ? (int)st.size : (EDITOR_MAX - 1);
    int n = pxe_read_file(path, ed->text, (size_t)to_read);
    if (n < 0) {
        ed->modal = MODAL_ERROR;
        snprintf(ed->err_msg, sizeof(ed->err_msg), "Read failed.");
        pxe_beep();
        return;
    }
    /* Normalize CRLF -> LF for clean cursor indexing */
    int out = 0;
    for (int i = 0; i < n; i++) {
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
        snprintf(ed->err_msg, sizeof(ed->err_msg), "Failed to save file!");
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
    if (kind == MODAL_SAVE_AS && ed->filename[0]) {
        strncpy(ed->modal_input, ed->filename, sizeof(ed->modal_input) - 1);
        ed->modal_input[sizeof(ed->modal_input) - 1] = '\0';
    } else if (kind == MODAL_OPEN && ed->filename[0]) {
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

static void insert_char(notepad_t* ed, char c) {
    if (has_selection(ed)) delete_selection(ed);
    if (ed->len >= EDITOR_MAX - 1) return;
    memmove(ed->text + ed->cursor + 1, ed->text + ed->cursor,
            (size_t)(ed->len - ed->cursor + 1));
    ed->text[ed->cursor++] = c;
    ed->len++;
    ed->modified = 1;
    update_title(ed);
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

static void get_xy_from_pos(const notepad_t* ed, int pos, int* out_x, int* out_y) {
    int x = 0, y = 0;
    for (int i = 0; i < pos && i < ed->len; i++) {
        if (ed->text[i] == '\n') {
            y++;
            x = 0;
        } else if (ed->text[i] == '\t') {
            x += 4 - (x & 3);
        } else {
            x++;
        }
    }
    *out_x = x;
    *out_y = y;
}

static int get_pos_from_xy(const notepad_t* ed, int tx, int ty) {
    if (ty < 0) return 0;
    int cx = 0, cy = 0;
    for (int i = 0; i < ed->len; i++) {
        if (cy == ty) {
            if (cx >= tx) return i;
        }
        if (ed->text[i] == '\n') {
            if (cy == ty) return i;
            cy++;
            cx = 0;
        } else if (ed->text[i] == '\t') {
            cx += 4 - (cx & 3);
        } else {
            cx++;
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
    for (int i = 0; tmp[i]; i++) {
        if (tmp[i] == '\r') continue;
        insert_char(ed, tmp[i]);
    }
}

static void ensure_cursor_visible(notepad_t* ed, const pxe_win_surface_t* surf) {
    int text_w = surf->width - 14 - 4;
    int text_h = surf->height - 22;
    int vis_lines = text_h / 16;
    int vis_cols  = text_w / 8;
    if (vis_lines < 1) vis_lines = 1;
    if (vis_cols < 1) vis_cols = 1;

    int cx, cy;
    get_xy_from_pos(ed, ed->cursor, &cx, &cy);
    if (cy < ed->scroll_y) ed->scroll_y = cy;
    if (cy >= ed->scroll_y + vis_lines) ed->scroll_y = cy - vis_lines + 1;
    if (cx < ed->scroll_x) ed->scroll_x = cx;
    if (cx >= ed->scroll_x + vis_cols) ed->scroll_x = cx - vis_cols + 1;
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

    int cur_x = 0, cur_y = 0;
    for (int i = 0; i <= ed->len; i++) {
        int px = 2 + (cur_x - ed->scroll_x) * 8;
        int py = 2 + (cur_y - ed->scroll_y) * 16;

        int char_cols = 1;
        if (i < ed->len && ed->text[i] == '\t') {
            char_cols = 4 - (cur_x & 3);
        }

        if (s1 >= 0 && i >= s1 && i < s2) {
            if (py >= 0 && py + 16 <= text_h && px + char_cols * 8 > 0 && px < text_w) {
                int hx = (px < 0) ? 0 : px;
                int hw = px + char_cols * 8 - hx;
                if (hx + hw > text_w) hw = text_w - hx;
                if (hw > 0) pxe_fill_rect(surf, hx, py, hw, 16, PXE_COL_BLUE);
            }
        }

        if (i == ed->cursor && ed->modal == MODAL_NONE) {
            if (px >= 0 && px < text_w && py >= 0 && py + 16 <= text_h) {
                pxe_draw_vline(surf, px, py, 16, PXE_COL_BLACK);
            }
        }

        if (i < ed->len) {
            char c = ed->text[i];
            if (c == '\n') {
                cur_y++;
                cur_x = 0;
            } else if (c == '\t') {
                cur_x += char_cols;
            } else {
                uint32_t fg = (s1 >= 0 && i >= s1 && i < s2) ? PXE_COL_WHITE : PXE_COL_BLACK;
                if (px >= 0 && px + 8 <= text_w && py >= 0 && py + 16 <= text_h) {
                    pxe_draw_char(surf, px, py, c, fg);
                }
                cur_x++;
            }
        }
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

    int cx, cy;
    get_xy_from_pos(ed, ed->cursor, &cx, &cy);
    char status[128];
    snprintf(status, sizeof(status), "%s%s | Ln %d, Col %d",
             ed->filename[0] ? ed->filename : "Untitled",
             ed->modified ? "*" : "",
             cy + 1, cx + 1);
    int max_c = (surf->width - 12) / 6;
    if (max_c > 0 && (int)strlen(status) > max_c) status[max_c] = '\0';
    pxe_draw_text_small(surf, 6, sby + 8, status, PXE_COL_BLACK);

    /* Modal dialogs */
    if (ed->modal == MODAL_OPEN || ed->modal == MODAL_SAVE_AS) {
        int dw = 260, dh = 96;
        if (dw > surf->width - 12) dw = surf->width - 12;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3,
                      (ed->modal == MODAL_OPEN) ? "Open File" : "Save As",
                      PXE_COL_WHITE);
        pxe_draw_text_small(surf, dx + 10, dy + 25, "Path (e.g. C:\\NOTE.TXT):", PXE_COL_BLACK);
        pxe_draw_bevel_box(surf, dx + 10, dy + 36, dw - 20, 22, 1, PXE_COL_WHITE);

        int max_vis = (dw - 28) / 8;
        if (max_vis < 1) max_vis = 1;
        int start = (ed->modal_len > max_vis) ? (ed->modal_len - max_vis) : 0;
        pxe_draw_text(surf, dx + 14, dy + 39, ed->modal_input + start, PXE_COL_BLACK);
        int cur_px = dx + 14 + (ed->modal_len - start) * 8;
        pxe_draw_vline(surf, cur_px, dy + 39, 16, PXE_COL_BLACK);

        pxe_draw_button(surf, dx + dw / 2 - 74, dy + 66, 66, 22, "OK", ed->btn_ok_pressed, 1);
        pxe_draw_button(surf, dx + dw / 2 + 8,  dy + 66, 66, 22, "Cancel", ed->btn_cancel_pressed, 1);
    } else if (ed->modal == MODAL_ERROR) {
        int dw = 240, dh = 86;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 12, dy + 12, 22, 22, PXE_COL_DARK_RED);
        pxe_draw_text(surf, dx + 19, dy + 15, "!", PXE_COL_WHITE);
        pxe_draw_text(surf, dx + 42, dy + 12, "File Error", PXE_COL_DARK_RED);
        pxe_draw_text_small(surf, dx + 42, dy + 32, ed->err_msg, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + dw / 2 - 30, dy + 56, 60, 22, "OK", ed->btn_ok_pressed, 1);
    }
}

static void handle_modal_mouse_down(notepad_t* ed, const pxe_win_surface_t* surf, int mx, int my) {
    if (ed->modal == MODAL_ERROR) {
        int dw = 240, dh = 86;
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
        int dw = 240, dh = 86;
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
    strcpy(desc.title, "Notepad - Untitled");

    pxe_win_surface_t surf;
    ed->win_id = pxe_win_create(&desc, &surf);
    if (ed->win_id < 0) {
        free(ed->text);
        free(ed);
        return 1;
    }

    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 2;
    strcpy(mb.menus[0].title, "File");
    mb.menus[0].item_count = 5;
    strcpy(mb.menus[0].items[0].label, "New");        mb.menus[0].items[0].enabled = 1;
    strcpy(mb.menus[0].items[1].label, "Open...");    mb.menus[0].items[1].enabled = 1;
    strcpy(mb.menus[0].items[2].label, "Save");       mb.menus[0].items[2].enabled = 1;
    strcpy(mb.menus[0].items[3].label, "Save As..."); mb.menus[0].items[3].enabled = 1;
    strcpy(mb.menus[0].items[4].label, "Exit");       mb.menus[0].items[4].enabled = 1;

    strcpy(mb.menus[1].title, "Edit");
    mb.menus[1].item_count = 5;
    strcpy(mb.menus[1].items[0].label, "Cut");        mb.menus[1].items[0].enabled = 1;
    strcpy(mb.menus[1].items[1].label, "Copy");       mb.menus[1].items[1].enabled = 1;
    strcpy(mb.menus[1].items[2].label, "Paste");      mb.menus[1].items[2].enabled = 1;
    strcpy(mb.menus[1].items[3].label, "Delete");     mb.menus[1].items[3].enabled = 1;
    strcpy(mb.menus[1].items[4].label, "Select All"); mb.menus[1].items[4].enabled = 1;
    pxe_win_set_menu(ed->win_id, &mb);

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
            case PXE_EV_REDRAW:
                dirty = 1;
                break;
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
                        int tx = ed->scroll_x + (ev.x - 2) / 8;
                        int ty = ed->scroll_y + (ev.y - 2) / 16;
                        if (tx < 0) tx = 0;
                        if (ty < 0) ty = 0;
                        ed->cursor = get_pos_from_xy(ed, tx, ty);
                        ed->sel_start = ed->cursor;
                        ed->sel_end = ed->cursor;
                        ed->dragging = 1;
                        dirty = 1;
                    }
                }
                break;
            case PXE_EV_MOUSE_MOVE:
                if (ed->dragging && (ev.buttons & PXE_BTN_LEFT) && ed->modal == MODAL_NONE) {
                    int tx = ed->scroll_x + (ev.x - 2) / 8;
                    int ty = ed->scroll_y + (ev.y - 2) / 16;
                    if (tx < 0) tx = 0;
                    if (ty < 0) ty = 0;
                    int pos = get_pos_from_xy(ed, tx, ty);
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
                            ed->modal_input[--ed->modal_len] = '\0';
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
                    int cx, cy;
                    get_xy_from_pos(ed, ed->cursor, &cx, &cy);
                    if (cy > 0) ed->cursor = get_pos_from_xy(ed, cx, cy - 1);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_DOWN) {
                    int cx, cy;
                    get_xy_from_pos(ed, ed->cursor, &cx, &cy);
                    ed->cursor = get_pos_from_xy(ed, cx, cy + 1);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_LEFT) {
                    if (ed->cursor > 0) ed->cursor--;
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_RIGHT) {
                    if (ed->cursor < ed->len) ed->cursor++;
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_HOME) {
                    ed->cursor = get_line_start(ed, ed->cursor);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_END) {
                    ed->cursor = get_line_end(ed, ed->cursor);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_PGUP) {
                    int cx, cy;
                    int step = (text_h / 16 > 1) ? (text_h / 16) : 1;
                    get_xy_from_pos(ed, ed->cursor, &cx, &cy);
                    int ny = cy - step;
                    if (ny < 0) ny = 0;
                    ed->cursor = get_pos_from_xy(ed, cx, ny);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_PGDN) {
                    int cx, cy;
                    int step = (text_h / 16 > 1) ? (text_h / 16) : 1;
                    get_xy_from_pos(ed, ed->cursor, &cx, &cy);
                    ed->cursor = get_pos_from_xy(ed, cx, cy + step);
                    ed->sel_start = ed->sel_end = -1;
                } else if (ev.key == PXE_KEY_BACKSPACE) {
                    if (has_selection(ed)) {
                        delete_selection(ed);
                    } else if (ed->cursor > 0) {
                        memmove(ed->text + ed->cursor - 1, ed->text + ed->cursor,
                                (size_t)(ed->len - ed->cursor + 1));
                        ed->cursor--;
                        ed->len--;
                        ed->modified = 1;
                        update_title(ed);
                    }
                } else if (ev.key == PXE_KEY_DELETE) {
                    if (has_selection(ed)) {
                        delete_selection(ed);
                    } else if (ed->cursor < ed->len) {
                        memmove(ed->text + ed->cursor, ed->text + ed->cursor + 1,
                                (size_t)(ed->len - ed->cursor));
                        ed->len--;
                        ed->modified = 1;
                        update_title(ed);
                    }
                } else if (ev.key == PXE_KEY_ENTER) {
                    insert_char(ed, '\n');
                } else if (ev.key == PXE_KEY_TAB) {
                    insert_char(ed, '\t');
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
