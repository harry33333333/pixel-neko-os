#include "cmd.h"

static cmd_state_t* g_cmd = NULL;

static int cp_is_wide(uint32_t cp) {
    return cp >= 0x80u;
}

static uint8_t sgr_to_vga_fg(int code, int bright) {
    static const uint8_t k_map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
    if (code < 0 || code > 7) return 7;
    return (uint8_t)(k_map[code] + (bright ? 8 : 0));
}

static term_cell_t* cell_at(cmd_state_t* st, int row, int col) {
    if (!st || !st->cells || row < 0 || row >= st->buf_rows ||
        col < 0 || col >= st->buf_cols) {
        return NULL;
    }
    return &st->cells[(size_t)row * (size_t)st->buf_cols + (size_t)col];
}

static void clear_row(cmd_state_t* st, int row) {
    term_cell_t* r = cell_at(st, row, 0);
    if (!r) return;
    for (int c = 0; c < st->buf_cols; c++) {
        r[c].codepoint = ' ';
        r[c].fg = st->cur_fg;
        r[c].bg = st->cur_bg;
        r[c].flags = CELL_FLAG_NORMAL;
    }
}

static void clear_half_conflict(cmd_state_t* st, int row, int col) {
    term_cell_t* cur = cell_at(st, row, col);
    if (!cur) return;
    if (cur->flags & CELL_FLAG_WIDE_LEAD) {
        term_cell_t* next = cell_at(st, row, col + 1);
        if (next && (next->flags & CELL_FLAG_WIDE_TRAIL)) {
            next->codepoint = ' ';
            next->flags = CELL_FLAG_NORMAL;
        }
        cur->codepoint = ' ';
        cur->flags = CELL_FLAG_NORMAL;
    } else if (cur->flags & CELL_FLAG_WIDE_TRAIL) {
        term_cell_t* prev = cell_at(st, row, col - 1);
        if (prev && (prev->flags & CELL_FLAG_WIDE_LEAD)) {
            prev->codepoint = ' ';
            prev->flags = CELL_FLAG_NORMAL;
        }
        cur->codepoint = ' ';
        cur->flags = CELL_FLAG_NORMAL;
    }
}

static int max_view_row(const cmd_state_t* st) {
    int m = st->cursor_row - st->vis_rows + 1;
    if (m < st->top_row) m = st->top_row;
    return m;
}

static void term_scroll_down_one(cmd_state_t* st) {
    int follow = (st->view_row >= max_view_row(st));
    st->cursor_row++;
    if (st->cursor_row >= st->buf_rows) {
        memmove(st->cells, st->cells + st->buf_cols,
                (size_t)(st->buf_rows - 1) * (size_t)st->buf_cols * sizeof(term_cell_t));
        st->cursor_row = st->buf_rows - 1;
        clear_row(st, st->cursor_row);
        if (st->prompt_start_row > 0) st->prompt_start_row--;
        if (st->view_row > 0) st->view_row--;
        if (st->sel_active) {
            if (st->sel_r0 > 0) st->sel_r0--;
            if (st->sel_r1 > 0) st->sel_r1--;
        }
    } else {
        clear_row(st, st->cursor_row);
    }
    if (follow) st->view_row = max_view_row(st);
}

static void term_newline(cmd_state_t* st) {
    st->cursor_col = 0;
    term_scroll_down_one(st);
    st->dirty = 1;
}

static void term_append_capture(cmd_state_t* st, uint32_t cp) {
    if (!st->capturing) return;
    char tmp[8];
    int n = utf8_encode_one(cp, tmp);
    if (n <= 0) return;
    if (!st->cap_buf) {
        st->cap_cap = CMD_CAP_INIT;
        st->cap_buf = (char*)malloc(st->cap_cap);
        if (!st->cap_buf) return;
        st->cap_len = 0;
    }
    if (st->cap_len + (size_t)n + 1 > st->cap_cap) {
        size_t ncap = st->cap_cap * 2u;
        if (ncap > CMD_CAP_MAX) ncap = CMD_CAP_MAX;
        if (st->cap_len + (size_t)n + 1 > ncap) return;
        char* nb = (char*)realloc(st->cap_buf, ncap);
        if (!nb) return;
        st->cap_buf = nb;
        st->cap_cap = ncap;
    }
    memcpy(st->cap_buf + st->cap_len, tmp, (size_t)n);
    st->cap_len += (size_t)n;
    st->cap_buf[st->cap_len] = '\0';
}

void term_clear(cmd_state_t* st) {
    if (!st || !st->cells) return;
    for (int r = 0; r < st->buf_rows; r++) clear_row(st, r);
    st->top_row = 0;
    st->view_row = 0;
    st->cursor_row = 0;
    st->cursor_col = 0;
    st->sel_active = 0;
    st->dirty = 1;
}

void term_set_attr(cmd_state_t* st, uint8_t bg, uint8_t fg) {
    st->def_bg = bg & 0x0Fu;
    st->def_fg = fg & 0x0Fu;
    st->cur_bg = st->def_bg;
    st->cur_fg = st->def_fg;
    if (st->cells) {
        size_t total = (size_t)st->buf_rows * (size_t)st->buf_cols;
        for (size_t i = 0; i < total; i++) {
            st->cells[i].bg = st->def_bg;
            st->cells[i].fg = st->def_fg;
        }
    }
    st->dirty = 1;
}

void term_putc_cp(cmd_state_t* st, uint32_t cp) {
    if (!st) return;
    if (st->capturing) {
        term_append_capture(st, cp);
        return;
    }
    if (cp == '\r') {
        st->cursor_col = 0;
        st->dirty = 1;
        return;
    }
    if (cp == '\n') {
        term_newline(st);
        return;
    }
    if (cp == '\b') {
        if (st->cursor_col > 0) {
            st->cursor_col--;
            clear_half_conflict(st, st->cursor_row, st->cursor_col);
        }
        st->dirty = 1;
        return;
    }
    if (cp == '\t') {
        int next_tab = (st->cursor_col + 4) & ~3;
        while (st->cursor_col < next_tab && st->cursor_col < st->vis_cols) {
            term_putc_cp(st, ' ');
        }
        return;
    }

    int wide = cp_is_wide(cp);
    if (wide) {
        if (st->cursor_col >= st->vis_cols - 1) {
            if (st->cursor_col == st->vis_cols - 1) {
                clear_half_conflict(st, st->cursor_row, st->cursor_col);
                term_cell_t* pad = cell_at(st, st->cursor_row, st->cursor_col);
                if (pad) {
                    pad->codepoint = ' ';
                    pad->fg = st->cur_fg;
                    pad->bg = st->cur_bg;
                    pad->flags = CELL_FLAG_NORMAL;
                }
            }
            term_newline(st);
        }
        clear_half_conflict(st, st->cursor_row, st->cursor_col);
        clear_half_conflict(st, st->cursor_row, st->cursor_col + 1);
        term_cell_t* lead = cell_at(st, st->cursor_row, st->cursor_col);
        term_cell_t* trail = cell_at(st, st->cursor_row, st->cursor_col + 1);
        if (lead && trail) {
            lead->codepoint = cp;
            lead->fg = st->cur_fg;
            lead->bg = st->cur_bg;
            lead->flags = (uint8_t)(CELL_FLAG_WIDE_LEAD | (st->bold ? CELL_FLAG_BOLD : 0));
            trail->codepoint = 0;
            trail->fg = st->cur_fg;
            trail->bg = st->cur_bg;
            trail->flags = CELL_FLAG_WIDE_TRAIL;
        }
        st->cursor_col += 2;
    } else {
        if (st->cursor_col >= st->vis_cols) {
            term_newline(st);
        }
        clear_half_conflict(st, st->cursor_row, st->cursor_col);
        term_cell_t* c = cell_at(st, st->cursor_row, st->cursor_col);
        if (c) {
            c->codepoint = (cp < 32u) ? ' ' : cp;
            c->fg = st->cur_fg;
            c->bg = st->cur_bg;
            c->flags = (uint8_t)(st->bold ? CELL_FLAG_BOLD : CELL_FLAG_NORMAL);
        }
        st->cursor_col++;
    }
    if (st->cursor_col >= st->vis_cols) {
        term_newline(st);
    }
    st->dirty = 1;
}

static void apply_ansi_sgr(cmd_state_t* st, const char* seq, int len) {
    int val = 0;
    int have = 0;
    for (int i = 0; i <= len; i++) {
        char ch = (i < len) ? seq[i] : ';';
        if (ch >= '0' && ch <= '9') {
            val = val * 10 + (ch - '0');
            have = 1;
        } else if (ch == ';') {
            if (!have) val = 0;
            if (val == 0) {
                st->cur_fg = st->def_fg;
                st->cur_bg = st->def_bg;
                st->bold = 0;
            } else if (val == 1) {
                st->bold = 1;
                if (st->cur_fg < 8) st->cur_fg = (uint8_t)(st->cur_fg + 8);
            } else if (val >= 30 && val <= 37) {
                st->cur_fg = sgr_to_vga_fg(val - 30, st->bold);
            } else if (val == 39) {
                st->cur_fg = st->def_fg;
            } else if (val >= 40 && val <= 47) {
                st->cur_bg = sgr_to_vga_fg(val - 40, 0);
            } else if (val == 49) {
                st->cur_bg = st->def_bg;
            } else if (val >= 90 && val <= 97) {
                st->cur_fg = sgr_to_vga_fg(val - 90, 1);
            } else if (val >= 100 && val <= 107) {
                st->cur_bg = sgr_to_vga_fg(val - 100, 1);
            }
            val = 0;
            have = 0;
        }
    }
}

void term_puts(cmd_state_t* st, const char* utf8) {
    if (!st || !utf8) return;
    const char* p = utf8;
    while (*p) {
        if (!st->capturing && p[0] == '\x1b' && p[1] == '[') {
            const char* q = p + 2;
            while (*q && *q != 'm' && *q != 'J' && *q != 'K' && (q - p) < 32) q++;
            if (*q == 'm') {
                apply_ansi_sgr(st, p + 2, (int)(q - (p + 2)));
                p = q + 1;
                continue;
            } else if (*q == 'J' || *q == 'K') {
                if (*q == 'J') term_clear(st);
                p = q + 1;
                continue;
            }
        }
        uint32_t cp = utf8_decode_next(&p);
        if (cp == 0u) break;
        term_putc_cp(st, cp);
        if (cp == '\n' && !st->capturing && st->running && !st->waiting_input) {
            term_flush_if_due(st, 0);
        }
    }
}

void term_printf(cmd_state_t* st, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(st, buf);
}

static void compute_vis_grid(cmd_state_t* st, const pxe_win_surface_t* surf) {
    int cw = surf->width - TERM_PAD_X * 2 - TERM_SB_W;
    int ch = surf->height - TERM_PAD_Y * 2;
    int vc = cw / TERM_CELL_W;
    int vr = ch / TERM_CELL_H;
    if (vc < 20) vc = 20;
    if (vc > TERM_MAX_COLS) vc = TERM_MAX_COLS;
    if (vr < 5) vr = 5;
    if (vr > TERM_MAX_ROWS) vr = TERM_MAX_ROWS;
    st->vis_cols = vc;
    st->vis_rows = vr;
    if (st->cursor_col >= st->vis_cols) st->cursor_col = st->vis_cols - 1;
    int max_v = max_view_row(st);
    if (st->view_row > max_v) st->view_row = max_v;
    if (st->view_row < st->top_row) st->view_row = st->top_row;
}

static int cell_selected(const cmd_state_t* st, int r, int c) {
    if (!st->sel_active) return 0;
    int r0 = st->sel_r0, c0 = st->sel_c0;
    int r1 = st->sel_r1, c1 = st->sel_c1;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr = r0, tc = c0;
        r0 = r1; c0 = c1; r1 = tr; c1 = tc;
    }
    if (r < r0 || r > r1) return 0;
    if (r == r0 && c < c0) return 0;
    if (r == r1 && c > c1) return 0;
    return 1;
}

static void render_cmd_into(cmd_state_t* st, const pxe_win_surface_t* surf) {
    compute_vis_grid(st, surf);
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, st->def_bg);

    for (int vr = 0; vr < st->vis_rows; vr++) {
        int lrow = st->view_row + vr;
        int py = TERM_PAD_Y + vr * TERM_CELL_H;
        if (lrow >= st->buf_rows) break;
        for (int vc = 0; vc < st->vis_cols; vc++) {
            const term_cell_t* cell = cell_at(st, lrow, vc);
            if (!cell) continue;
            if (cell->flags & CELL_FLAG_WIDE_TRAIL) continue;
            int px = TERM_PAD_X + vc * TERM_CELL_W;
            int is_wide = (cell->flags & CELL_FLAG_WIDE_LEAD) ? 1 : 0;
            int cw = is_wide ? (TERM_CELL_W * 2) : TERM_CELL_W;
            int sel = cell_selected(st, lrow, vc) ||
                      (is_wide && cell_selected(st, lrow, vc + 1));
            uint8_t bg = sel ? 15u : cell->bg;
            uint8_t fg = sel ? 0u : cell->fg;
            if (bg != st->def_bg || sel) {
                pxe_fill_rect(surf, px, py, cw, TERM_CELL_H, bg);
            }
            if (cell->codepoint > ' ') {
                pxe_draw_codepoint(surf, px, py, cell->codepoint, fg);
            }
        }
    }

    /* Blinking underline cursor */
    if (st->blink_phase == 0 &&
        st->cursor_row >= st->view_row &&
        st->cursor_row < st->view_row + st->vis_rows) {
        int cx = TERM_PAD_X + st->cursor_col * TERM_CELL_W;
        int cy = TERM_PAD_Y + (st->cursor_row - st->view_row) * TERM_CELL_H + 13;
        if (cx + TERM_CELL_W <= surf->width - TERM_SB_W) {
            pxe_fill_rect(surf, cx, cy, TERM_CELL_W, 2, st->cur_fg);
        }
    }

    /* Scrollbar */
    int total_lines = st->cursor_row - st->top_row + 1;
    if (total_lines < st->vis_rows) total_lines = st->vis_rows;
    pxe_draw_scrollbar(surf, surf->width - TERM_SB_W, 0, TERM_SB_W, surf->height,
                       st->view_row - st->top_row, st->vis_rows, total_lines);

    /* Right-click context menu */
    if (st->ctx_open) {
        int mw = 112, mh = 76;
        int mx = st->ctx_x, my = st->ctx_y;
        if (mx + mw > surf->width - TERM_SB_W) mx = surf->width - TERM_SB_W - mw;
        if (my + mh > surf->height) my = surf->height - mh;
        if (mx < 2) mx = 2;
        if (my < 2) my = 2;
        pxe_draw_bevel_box(surf, mx, my, mw, mh, 0, PXE_COL_LIGHT_GRAY);
        pxe_draw_text(surf, mx + 8, my + 4,  CMD_TR(st, "复制", "Copy"), st->sel_active ? PXE_COL_BLACK : PXE_COL_DARK_GRAY);
        pxe_draw_text(surf, mx + 8, my + 22, CMD_TR(st, "粘贴", "Paste"), PXE_COL_BLACK);
        pxe_draw_text(surf, mx + 8, my + 40, CMD_TR(st, "全选", "Select All"), PXE_COL_BLACK);
        pxe_draw_text(surf, mx + 8, my + 58, CMD_TR(st, "清屏", "Clear"), PXE_COL_BLACK);
    }
}

void term_render_now(cmd_state_t* st) {
    if (!st || st->win_id < 0) return;
    pxe_win_surface_t surf;
    if (pxe_win_get_surface(st->win_id, &surf) != 0 || !surf.pixels) return;

    size_t need = (size_t)surf.height * (size_t)surf.pitch;
    if (need > st->back_cap) {
        uint8_t* nb = (uint8_t*)malloc(need);
        if (nb) {
            if (st->back_pixels) free(st->back_pixels);
            st->back_pixels = nb;
            st->back_cap = need;
        }
    }

    if (st->back_pixels) {
        pxe_win_surface_t off = surf;
        off.pixels = st->back_pixels;
        render_cmd_into(st, &off);
        size_t row_bytes = (size_t)surf.width;
        for (int y = 0; y < surf.height; y++) {
            memcpy((uint8_t*)surf.pixels + (size_t)y * (size_t)surf.pitch,
                   st->back_pixels + (size_t)y * (size_t)surf.pitch, row_bytes);
        }
    } else {
        render_cmd_into(st, &surf);
    }
    pxe_win_present(st->win_id);
    st->dirty = 0;
    st->last_flush_tick = pxe_get_ticks();

    /* Update kernel IME candidate anchor near blinking cursor */
    if (st->cursor_row >= st->view_row &&
        st->cursor_row < st->view_row + st->vis_rows) {
        int ax = TERM_PAD_X + st->cursor_col * TERM_CELL_W;
        int ay = TERM_PAD_Y + (st->cursor_row - st->view_row) * TERM_CELL_H + 18;
        pxe_ime_set_cursor_pos(ax, ay);
    }
}

void term_flush_if_due(cmd_state_t* st, int force) {
    if (!st || st->win_id < 0 || st->capturing) return;
    if (!st->dirty && !force) return;
    uint64_t now = pxe_get_ticks();
    if (!force && st->last_flush_tick != 0 && (now - st->last_flush_tick) < 3u) {
        return;
    }
    term_render_now(st);
    pxe_sleep_ms(force ? 5 : 1);
}

static void erase_cells_from(cmd_state_t* st, int r, int c) {
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    for (int row = r; row <= st->cursor_row && row < st->buf_rows; row++) {
        int cstart = (row == r) ? c : 0;
        for (int col = cstart; col < st->vis_cols; col++) {
            term_cell_t* cell = cell_at(st, row, col);
            if (cell) {
                cell->codepoint = ' ';
                cell->fg = st->cur_fg;
                cell->bg = st->cur_bg;
                cell->flags = CELL_FLAG_NORMAL;
            }
        }
    }
}

static void redraw_prompt_line(cmd_state_t* st) {
    erase_cells_from(st, st->prompt_start_row, st->prompt_start_col);
    st->cursor_row = st->prompt_start_row;
    st->cursor_col = st->prompt_start_col;

    int target_r = st->prompt_start_row;
    int target_c = st->prompt_start_col;
    int byte_i = 0;
    const char* p = st->line;
    while (*p) {
        if (byte_i == st->line_cursor) {
            target_r = st->cursor_row;
            target_c = st->cursor_col;
        }
        const char* prev_p = p;
        uint32_t cp = utf8_decode_next(&p);
        byte_i += (int)(p - prev_p);
        if (cp == 0u) break;
        term_putc_cp(st, cp);
    }
    if (byte_i == st->line_cursor) {
        target_r = st->cursor_row;
        target_c = st->cursor_col;
    }
    st->cursor_row = target_r;
    st->cursor_col = target_c;
    st->view_row = max_view_row(st);
    st->dirty = 1;
}

static void print_prompt(cmd_state_t* st) {
    shell_sync_cwd_env(st);
    st->cur_fg = st->def_fg;
    st->cur_bg = st->def_bg;
    st->bold = 0;
    const char* p = shell_get_env(st, "PROMPT");
    if (!p || !p[0]) p = "$P$G";
    for (int i = 0; p[i]; i++) {
        if (p[i] == '$' && p[i + 1]) {
            char code = p[++i];
            if (code >= 'a' && code <= 'z') code = (char)(code - 32);
            if (code == 'P') term_puts(st, st->cwd);
            else if (code == 'G') term_putc_cp(st, '>');
            else if (code == 'L') term_putc_cp(st, '<');
            else if (code == 'S') term_putc_cp(st, ' ');
            else if (code == 'N') term_putc_cp(st, st->cwd[0] ? st->cwd[0] : 'C');
            else term_putc_cp(st, (uint32_t)code);
        } else {
            term_putc_cp(st, (uint32_t)(unsigned char)p[i]);
        }
    }
    st->prompt_start_row = st->cursor_row;
    st->prompt_start_col = st->cursor_col;
    st->line[0] = '\0';
    st->line_len = 0;
    st->line_cursor = 0;
    st->hist_nav = -1;
    st->waiting_input = 1;
    shell_tab_reset(st);
    st->dirty = 1;
}

static void copy_selection_to_clipboard(cmd_state_t* st) {
    if (!st->sel_active) return;
    int r0 = st->sel_r0, c0 = st->sel_c0;
    int r1 = st->sel_r1, c1 = st->sel_c1;
    if (r0 > r1 || (r0 == r1 && c0 > c1)) {
        int tr = r0, tc = c0;
        r0 = r1; c0 = c1; r1 = tr; c1 = tc;
    }
    char out[3072];
    size_t pos = 0;
    for (int r = r0; r <= r1 && pos + 8 < sizeof(out); r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : (st->vis_cols - 1);
        size_t line_start = pos;
        for (int c = cs; c <= ce && pos + 8 < sizeof(out); c++) {
            const term_cell_t* cell = cell_at(st, r, c);
            if (!cell || (cell->flags & CELL_FLAG_WIDE_TRAIL)) continue;
            uint32_t cp = cell->codepoint ? cell->codepoint : ' ';
            int n = utf8_encode_one(cp, out + pos);
            if (n > 0) pos += (size_t)n;
        }
        while (pos > line_start && out[pos - 1] == ' ') pos--;
        if (r < r1 && pos + 2 < sizeof(out)) out[pos++] = '\n';
    }
    out[pos] = '\0';
    if (pos > 0) pxe_clipboard_set(out);
}

static void line_insert_utf8(cmd_state_t* st, const char* text) {
    if (!text || !text[0]) return;
    size_t tl = strlen(text);
    if ((size_t)st->line_len + tl >= CMD_MAX_LINE) return;
    memmove(st->line + st->line_cursor + tl,
            st->line + st->line_cursor,
            (size_t)(st->line_len - st->line_cursor + 1));
    memcpy(st->line + st->line_cursor, text, tl);
    st->line_len += (int)tl;
    st->line_cursor += (int)tl;
    shell_tab_reset(st);
    redraw_prompt_line(st);
}

static void paste_clipboard_to_line(cmd_state_t* st) {
    char cb[512];
    int n = pxe_clipboard_get(cb, sizeof(cb));
    if (n <= 0) return;
    char clean[512];
    int ci = 0;
    for (int i = 0; i < n && ci + 1 < (int)sizeof(clean); i++) {
        if (cb[i] == '\r' || cb[i] == '\n') break;
        clean[ci++] = cb[i];
    }
    clean[ci] = '\0';
    line_insert_utf8(st, clean);
}

static void handle_mouse_event(cmd_state_t* st, const pxe_event_t* ev) {
    pxe_win_surface_t surf;
    if (pxe_win_get_surface(st->win_id, &surf) != 0) return;
    compute_vis_grid(st, &surf);

    if (ev->type == PXE_EV_MOUSE_WHEEL) {
        st->view_row -= ev->arg0 * 3;
        if (st->view_row < st->top_row) st->view_row = st->top_row;
        int mv = max_view_row(st);
        if (st->view_row > mv) st->view_row = mv;
        st->dirty = 1;
        return;
    }

    if (ev->type == PXE_EV_MOUSE_DOWN && (ev->buttons & PXE_BTN_RIGHT)) {
        st->ctx_open = 1;
        st->ctx_x = ev->x;
        st->ctx_y = ev->y;
        st->dirty = 1;
        return;
    }

    if (ev->type == PXE_EV_MOUSE_DOWN && (ev->buttons & PXE_BTN_LEFT)) {
        if (st->ctx_open) {
            int mw = 112, mh = 76;
            int mx = st->ctx_x, my = st->ctx_y;
            if (mx + mw > surf.width - TERM_SB_W) mx = surf.width - TERM_SB_W - mw;
            if (my + mh > surf.height) my = surf.height - mh;
            if (mx < 2) mx = 2;
            if (my < 2) my = 2;
            if (pxe_point_in_rect(ev->x, ev->y, mx, my, mw, mh)) {
                int item = (ev->y - my - 2) / 18;
                st->ctx_open = 0;
                if (item == 0) copy_selection_to_clipboard(st);
                else if (item == 1 && st->waiting_input) paste_clipboard_to_line(st);
                else if (item == 2) {
                    st->sel_active = 1;
                    st->sel_r0 = st->top_row; st->sel_c0 = 0;
                    st->sel_r1 = st->cursor_row; st->sel_c1 = st->vis_cols - 1;
                } else if (item == 3) {
                    term_clear(st);
                    if (st->waiting_input) {
                        print_prompt(st);
                    }
                }
                st->dirty = 1;
                return;
            }
            st->ctx_open = 0;
            st->dirty = 1;
        }

        /* Scrollbar click */
        if (ev->x >= surf.width - TERM_SB_W) {
            if (ev->y < 14) st->view_row--;
            else if (ev->y >= surf.height - 14) st->view_row++;
            else {
                st->sb_dragging = 1;
                st->sb_drag_y = ev->y;
                st->sb_drag_view = st->view_row;
            }
            if (st->view_row < st->top_row) st->view_row = st->top_row;
            int mv = max_view_row(st);
            if (st->view_row > mv) st->view_row = mv;
            st->dirty = 1;
            return;
        }

        /* Text drag selection start */
        int col = (ev->x - TERM_PAD_X) / TERM_CELL_W;
        int row = st->view_row + (ev->y - TERM_PAD_Y) / TERM_CELL_H;
        if (col < 0) col = 0;
        if (col >= st->vis_cols) col = st->vis_cols - 1;
        if (row < 0) row = 0;
        if (row >= st->buf_rows) row = st->buf_rows - 1;
        st->sel_dragging = 1;
        st->sel_active = 0;
        st->sel_r0 = row; st->sel_c0 = col;
        st->sel_r1 = row; st->sel_c1 = col;
        st->dirty = 1;
    } else if (ev->type == PXE_EV_MOUSE_MOVE) {
        if (st->sb_dragging) {
            int track_h = surf.height - 28;
            if (track_h < 1) track_h = 1;
            int mv = max_view_row(st);
            int range = mv - st->top_row;
            int dy = ev->y - st->sb_drag_y;
            st->view_row = st->sb_drag_view + (dy * (range + 1)) / track_h;
            if (st->view_row < st->top_row) st->view_row = st->top_row;
            if (st->view_row > mv) st->view_row = mv;
            st->dirty = 1;
        } else if (st->sel_dragging) {
            int col = (ev->x - TERM_PAD_X) / TERM_CELL_W;
            int row = st->view_row + (ev->y - TERM_PAD_Y) / TERM_CELL_H;
            if (col < 0) col = 0;
            if (col >= st->vis_cols) col = st->vis_cols - 1;
            if (row < 0) row = 0;
            if (row >= st->buf_rows) row = st->buf_rows - 1;
            st->sel_r1 = row;
            st->sel_c1 = col;
            st->sel_active = (st->sel_r0 != st->sel_r1 || st->sel_c0 != st->sel_c1);
            st->dirty = 1;
        }
    } else if (ev->type == PXE_EV_MOUSE_UP) {
        st->sb_dragging = 0;
        st->sel_dragging = 0;
    }
}

int term_poll_interrupt(cmd_state_t* st) {
    if (!st || !st->running) return 1;
    pxe_event_t ev;
    while (pxe_win_poll_event(st->win_id, &ev) > 0) {
        if (ev.type == PXE_EV_CLOSE) {
            st->running = 0;
            st->interrupted = 1;
            return 1;
        } else if (ev.type == PXE_EV_RESIZE || ev.type == PXE_EV_REDRAW) {
            st->dirty = 1;
        } else if (ev.type == PXE_EV_MOUSE_DOWN || ev.type == PXE_EV_MOUSE_UP ||
                   ev.type == PXE_EV_MOUSE_MOVE || ev.type == PXE_EV_MOUSE_WHEEL) {
            handle_mouse_event(st, &ev);
        } else if (ev.type == PXE_EV_KEY_DOWN) {
            if ((ev.modifiers & PXE_MOD_CTRL) &&
                (ev.key == 'c' || ev.key == 'C' || ev.key == 3)) {
                st->interrupted = 1;
                return 1;
            }
        }
    }
    if (st->dirty) {
        term_render_now(st);
        pxe_yield();
    }
    return st->interrupted;
}

int term_wait_key_blocking(cmd_state_t* st, pxe_event_t* out_ev) {
    while (st && st->running) {
        pxe_event_t ev;
        int rc = pxe_win_poll_event(st->win_id, &ev);
        if (rc < 0) { st->running = 0; return -1; }
        if (rc > 0) {
            if (ev.type == PXE_EV_CLOSE) {
                st->running = 0;
                st->interrupted = 1;
                return -1;
            } else if (ev.type == PXE_EV_RESIZE || ev.type == PXE_EV_REDRAW) {
                st->dirty = 1;
                term_render_now(st);
            } else if (ev.type == PXE_EV_MOUSE_DOWN || ev.type == PXE_EV_MOUSE_UP ||
                       ev.type == PXE_EV_MOUSE_MOVE || ev.type == PXE_EV_MOUSE_WHEEL) {
                handle_mouse_event(st, &ev);
                if (st->dirty) term_render_now(st);
            } else if (ev.type == PXE_EV_KEY_DOWN) {
                if ((ev.modifiers & PXE_MOD_CTRL) &&
                    (ev.key == 'c' || ev.key == 'C' || ev.key == 3)) {
                    st->interrupted = 1;
                }
                if (out_ev) *out_ev = ev;
                return 0;
            }
        } else {
            pxe_sleep_ms(20);
        }
    }
    return -1;
}

int term_more_prompt(cmd_state_t* st) {
    if (!st || st->capturing) return 0;
    term_puts(st, CMD_TR(st, "-- 更多 [Space=下页 Enter=下行 Q=退出] --",
                         "-- More [Space=Page Enter=Line Q=Quit] --"));
    term_render_now(st);
    pxe_event_t ev;
    if (term_wait_key_blocking(st, &ev) != 0) return -1;
    term_puts(st, "\r                                              \r");
    if (st->interrupted || ev.key == 'q' || ev.key == 'Q' || ev.key == PXE_KEY_ESC) {
        return -1;
    }
    if (ev.key == PXE_KEY_ENTER) return 1;
    return 0;
}

static void push_history(cmd_state_t* st, const char* line) {
    if (!line || !line[0]) return;
    if (st->hist_count > 0 && strcmp(st->history[st->hist_count - 1], line) == 0) return;
    if (st->hist_count >= CMD_HISTORY_MAX) {
        memmove(st->history[0], st->history[1],
                (size_t)(CMD_HISTORY_MAX - 1) * sizeof(st->history[0]));
        st->hist_count = CMD_HISTORY_MAX - 1;
    }
    snprintf(st->history[st->hist_count++], CMD_MAX_LINE, "%s", line);
}

static void handle_prompt_key(cmd_state_t* st, const pxe_event_t* ev) {
    int ctrl = (ev->modifiers & PXE_MOD_CTRL) != 0;

    if (ctrl && (ev->key == 'c' || ev->key == 'C' || ev->key == 3)) {
        if (st->sel_active && st->line_len == 0) {
            copy_selection_to_clipboard(st);
            return;
        }
        if (st->sel_active) {
            copy_selection_to_clipboard(st);
        }
        term_puts(st, "^C\n");
        print_prompt(st);
        return;
    }
    if (ctrl && (ev->key == 'l' || ev->key == 'L' || ev->key == 12)) {
        term_clear(st);
        print_prompt(st);
        return;
    }
    if (ctrl && (ev->key == 'v' || ev->key == 'V' || ev->key == 22)) {
        paste_clipboard_to_line(st);
        return;
    }

    if (ev->key == PXE_KEY_ENTER) {
        char exec_buf[CMD_MAX_LINE];
        snprintf(exec_buf, sizeof(exec_buf), "%s", st->line);
        st->waiting_input = 0;
        st->cursor_col = st->prompt_start_col;
        st->cursor_row = st->prompt_start_row;
        redraw_prompt_line(st);
        /* Move cursor to end of printed prompt + line before newline */
        st->cursor_row = st->prompt_start_row;
        st->cursor_col = st->prompt_start_col;
        const char* p = st->line;
        while (*p) {
            uint32_t cp = utf8_decode_next(&p);
            if (!cp) break;
            int adv = cp_is_wide(cp) ? 2 : 1;
            if (adv == 2 && st->cursor_col >= st->vis_cols - 1) {
                st->cursor_col = 0;
                st->cursor_row++;
            }
            st->cursor_col += adv;
            if (st->cursor_col >= st->vis_cols) {
                st->cursor_col = 0;
                st->cursor_row++;
            }
        }
        if (st->cursor_row >= st->buf_rows) st->cursor_row = st->buf_rows - 1;
        term_putc_cp(st, '\n');

        push_history(st, exec_buf);
        st->interrupted = 0;
        if (exec_buf[0]) {
            (void)shell_exec_line(st, exec_buf);
        }
        if (st->running) print_prompt(st);
        return;
    }

    if (ev->key == PXE_KEY_BACKSPACE) {
        if (st->line_cursor > 0) {
            int prev = utf8_prev_char_bytes(st->line, st->line_cursor);
            if (prev <= 0) prev = 1;
            memmove(st->line + st->line_cursor - prev,
                    st->line + st->line_cursor,
                    (size_t)(st->line_len - st->line_cursor + 1));
            st->line_cursor -= prev;
            st->line_len -= prev;
            shell_tab_reset(st);
            redraw_prompt_line(st);
        }
        return;
    }

    if (ev->key == PXE_KEY_DELETE) {
        if (st->line_cursor < st->line_len) {
            int nxt = utf8_next_char_bytes(st->line, st->line_cursor, st->line_len);
            if (nxt <= 0) nxt = 1;
            memmove(st->line + st->line_cursor,
                    st->line + st->line_cursor + nxt,
                    (size_t)(st->line_len - st->line_cursor - nxt + 1));
            st->line_len -= nxt;
            shell_tab_reset(st);
            redraw_prompt_line(st);
        }
        return;
    }

    if (ev->key == PXE_KEY_LEFT) {
        if (st->line_cursor > 0) {
            int prev = utf8_prev_char_bytes(st->line, st->line_cursor);
            st->line_cursor -= (prev > 0 ? prev : 1);
            redraw_prompt_line(st);
        }
        return;
    }

    if (ev->key == PXE_KEY_RIGHT) {
        if (st->line_cursor < st->line_len) {
            int nxt = utf8_next_char_bytes(st->line, st->line_cursor, st->line_len);
            st->line_cursor += (nxt > 0 ? nxt : 1);
            redraw_prompt_line(st);
        }
        return;
    }

    if (ev->key == PXE_KEY_HOME) {
        st->line_cursor = 0;
        redraw_prompt_line(st);
        return;
    }

    if (ev->key == PXE_KEY_END) {
        st->line_cursor = st->line_len;
        redraw_prompt_line(st);
        return;
    }

    if (ev->key == PXE_KEY_UP) {
        if (st->hist_count > 0) {
            if (st->hist_nav < 0) {
                snprintf(st->hist_saved, sizeof(st->hist_saved), "%s", st->line);
                st->hist_nav = st->hist_count - 1;
            } else if (st->hist_nav > 0) {
                st->hist_nav--;
            }
            snprintf(st->line, sizeof(st->line), "%s", st->history[st->hist_nav]);
            st->line_len = (int)strlen(st->line);
            st->line_cursor = st->line_len;
            redraw_prompt_line(st);
        }
        return;
    }

    if (ev->key == PXE_KEY_DOWN) {
        if (st->hist_nav >= 0) {
            if (st->hist_nav + 1 < st->hist_count) {
                st->hist_nav++;
                snprintf(st->line, sizeof(st->line), "%s", st->history[st->hist_nav]);
            } else {
                st->hist_nav = -1;
                snprintf(st->line, sizeof(st->line), "%s", st->hist_saved);
            }
            st->line_len = (int)strlen(st->line);
            st->line_cursor = st->line_len;
            redraw_prompt_line(st);
        }
        return;
    }

    if (ev->key == PXE_KEY_PGUP) {
        st->view_row -= st->vis_rows;
        if (st->view_row < st->top_row) st->view_row = st->top_row;
        st->dirty = 1;
        return;
    }
    if (ev->key == PXE_KEY_PGDN) {
        st->view_row += st->vis_rows;
        int mv = max_view_row(st);
        if (st->view_row > mv) st->view_row = mv;
        st->dirty = 1;
        return;
    }

    if (ev->key == PXE_KEY_TAB) {
        shell_tab_complete(st);
        redraw_prompt_line(st);
        return;
    }

    if (ev->key == PXE_KEY_UTF8 && ev->text[0]) {
        line_insert_utf8(st, ev->text);
        return;
    }

    if (ev->key >= 32 && ev->key <= 126) {
        char ch[2] = { (char)ev->key, '\0' };
        line_insert_utf8(st, ch);
    }
}

static void sync_cmd_menu(cmd_state_t* st) {
    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 2;
    strcpy(mb.menus[0].title, CMD_TR(st, "编辑", "Edit"));
    mb.menus[0].item_count = 4;
    strcpy(mb.menus[0].items[0].label, CMD_TR(st, "复制", "Copy"));       mb.menus[0].items[0].enabled = 1;
    strcpy(mb.menus[0].items[1].label, CMD_TR(st, "粘贴", "Paste"));      mb.menus[0].items[1].enabled = 1;
    strcpy(mb.menus[0].items[2].label, CMD_TR(st, "全选", "Select All")); mb.menus[0].items[2].enabled = 1;
    strcpy(mb.menus[0].items[3].label, CMD_TR(st, "退出", "Exit"));       mb.menus[0].items[3].enabled = 1;

    strcpy(mb.menus[1].title, CMD_TR(st, "视图", "View"));
    mb.menus[1].item_count = 2;
    strcpy(mb.menus[1].items[0].label, CMD_TR(st, "清屏 (CLS)", "Clear Screen")); mb.menus[1].items[0].enabled = 1;
    strcpy(mb.menus[1].items[1].label, CMD_TR(st, "帮助 (HELP)", "Help"));        mb.menus[1].items[1].enabled = 1;
    pxe_win_set_menu(st->win_id, &mb);
}

int main(int argc, char** argv) {
    cmd_state_t* st = (cmd_state_t*)calloc(1, sizeof(cmd_state_t));
    if (!st) return 1;
    g_cmd = st;

    st->buf_cols = TERM_MAX_COLS;
    st->buf_rows = TERM_SCROLLBACK;
    st->vis_cols = TERM_DEFAULT_COLS;
    st->vis_rows = TERM_DEFAULT_ROWS;
    st->cells = (term_cell_t*)calloc((size_t)st->buf_cols * (size_t)st->buf_rows,
                                     sizeof(term_cell_t));
    if (!st->cells) {
        free(st);
        return 1;
    }

    st->zh = pxe_is_zh();
    st->running = 1;
    st->echo_on = 1;
    st->hist_nav = -1;
    st->def_bg = 0; /* Black */
    st->def_fg = 7; /* Light Gray */
    st->cur_bg = 0;
    st->cur_fg = 7;
    term_clear(st);

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = -1;
    desc.y = -1;
    desc.w = 660;
    desc.h = 420;
    desc.min_w = 340;
    desc.min_h = 220;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_RESIZABLE;
    strcpy(desc.title, st->zh ? "命令提示符" : "Command Prompt");

    pxe_win_surface_t surf;
    st->win_id = pxe_win_create(&desc, &surf);
    if (st->win_id < 0) {
        free(st->cells);
        free(st);
        return 1;
    }
    sync_cmd_menu(st);
    shell_init_env(st);

    /* Parse startup arguments: /c <cmd>, /k <cmd>, or <dir_path> */
    int auto_close = 0;
    char init_cmd[CMD_MAX_LINE];
    init_cmd[0] = '\0';
    if (argc > 1 && argv[1] && argv[1][0]) {
        if (strcasecmp(argv[1], "/c") == 0 || strcasecmp(argv[1], "/k") == 0) {
            auto_close = (strcasecmp(argv[1], "/c") == 0);
            for (int i = 2; i < argc; i++) {
                if (!argv[i]) continue;
                size_t cur = strlen(init_cmd);
                if (cur > 0 && cur + 1 < sizeof(init_cmd)) init_cmd[cur++] = ' ';
                snprintf(init_cmd + cur, sizeof(init_cmd) - cur, "%s", argv[i]);
            }
        } else {
            pxe_dirent_t de;
            char resolved[256];
            if (shell_resolve_path(st, argv[1], resolved, sizeof(resolved)) == 0 &&
                pxe_stat(resolved, &de) == 0 && de.is_dir) {
                snprintf(st->cwd, sizeof(st->cwd), "%s", resolved);
                shell_sync_cwd_env(st);
            } else {
                const char* dot = strrchr(argv[1], '.');
                if (dot && strcasecmp(dot, ".pat") == 0) {
                    snprintf(init_cmd, sizeof(init_cmd), "\"%s\"", argv[1]);
                }
            }
        }
    }

    if (!auto_close) {
        term_puts(st, CMD_TR(st,
            "Pixel Neko OS [版本 26A300b]\n(C) Pixel Neko OS Project. 输入 \"help\" 查看命令。\n\n",
            "Pixel Neko OS [Version 26A300b]\n(C) Pixel Neko OS Project. Type \"help\" for commands.\n\n"));
        term_render_now(st);
        pxe_sleep_ms(5);
    }

    if (init_cmd[0]) {
        if (auto_close) {
            (void)shell_exec_line(st, init_cmd);
            st->running = 0;
        } else {
            print_prompt(st);
            term_puts(st, init_cmd);
            term_putc_cp(st, '\n');
            term_flush_if_due(st, 1);
            st->waiting_input = 0;
            (void)shell_exec_line(st, init_cmd);
        }
    }
    if (st->running) {
        print_prompt(st);
        term_render_now(st);
    }

    uint64_t last_blink = pxe_get_ticks();
    while (st->running) {
        pxe_event_t ev;
        int rc;
        while ((rc = pxe_win_poll_event(st->win_id, &ev)) > 0) {
            if (ev.type == PXE_EV_CLOSE) {
                st->running = 0;
                break;
            } else if (ev.type == PXE_EV_REDRAW || ev.type == PXE_EV_ACTIVATE) {
                int new_zh = pxe_is_zh();
                if (new_zh != st->zh) {
                    st->zh = new_zh;
                    pxe_win_set_title(st->win_id, st->zh ? "命令提示符" : "Command Prompt");
                    sync_cmd_menu(st);
                    shell_set_env(st, "LANG", st->zh ? "ZH" : "EN");
                }
                if (ev.type == PXE_EV_ACTIVATE && ev.text[0]) {
                    pxe_dirent_t de;
                    char resolved[256];
                    if (shell_resolve_path(st, ev.text, resolved, sizeof(resolved)) == 0 &&
                        pxe_stat(resolved, &de) == 0 && de.is_dir) {
                        snprintf(st->cwd, sizeof(st->cwd), "%s", resolved);
                        term_puts(st, "\n");
                        print_prompt(st);
                    }
                }
                st->dirty = 1;
            } else if (ev.type == PXE_EV_RESIZE) {
                st->dirty = 1;
            } else if (ev.type == PXE_EV_MOUSE_DOWN || ev.type == PXE_EV_MOUSE_UP ||
                       ev.type == PXE_EV_MOUSE_MOVE || ev.type == PXE_EV_MOUSE_WHEEL) {
                handle_mouse_event(st, &ev);
            } else if (ev.type == PXE_EV_MENU) {
                if (ev.arg0 == 0) {
                    if (ev.arg1 == 0) copy_selection_to_clipboard(st);
                    else if (ev.arg1 == 1 && st->waiting_input) paste_clipboard_to_line(st);
                    else if (ev.arg1 == 2) {
                        st->sel_active = 1;
                        st->sel_r0 = st->top_row; st->sel_c0 = 0;
                        st->sel_r1 = st->cursor_row; st->sel_c1 = st->vis_cols - 1;
                    } else if (ev.arg1 == 3) {
                        st->running = 0;
                    }
                } else if (ev.arg0 == 1) {
                    if (ev.arg1 == 0) {
                        term_clear(st);
                        print_prompt(st);
                    } else if (ev.arg1 == 1) {
                        term_puts(st, "\n");
                        (void)shell_exec_line(st, "help");
                        print_prompt(st);
                    }
                }
                st->dirty = 1;
            } else if (ev.type == PXE_EV_KEY_DOWN && st->waiting_input) {
                handle_prompt_key(st, &ev);
                st->blink_phase = 0;
                last_blink = pxe_get_ticks();
            }
        }
        if (rc < 0) break;

        uint64_t now = pxe_get_ticks();
        if (now - last_blink >= 50ULL) {
            last_blink = now;
            st->blink_phase = !st->blink_phase;
            st->dirty = 1;
        }
        if (st->dirty && st->running) {
            term_render_now(st);
        } else if (st->running) {
            pxe_sleep_ms(20);
        }
    }

    pxe_win_destroy(st->win_id);
    if (st->cap_buf) free(st->cap_buf);
    if (st->back_pixels) free(st->back_pixels);
    if (st->cells) free(st->cells);
    free(st);
    return 0;
}
