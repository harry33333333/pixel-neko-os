#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>

#define CALC_MAX_ABS 999999999LL

typedef struct {
    int64_t display;
    int64_t stored;
    int     op;          /* 0=none, 1=+, 2=-, 3=*, 4=/ */
    int     new_number;
    int     error;
    int     pressed_idx; /* 0..15 or -1 */
} calc_state_t;

static const char* s_btn_labels[16] = {
    "7", "8", "9", "/",
    "4", "5", "6", "*",
    "1", "2", "3", "-",
    "0", "C", "=", "+"
};

static void btn_rect(int idx, int* ox, int* oy, int* ow, int* oh) {
    int col = idx & 3;
    int row = idx >> 2;
    *ox = 14 + col * 37;
    *oy = 42 + row * 28;
    *ow = 33;
    *oh = 23;
}

static void calc_reset(calc_state_t* st) {
    st->display = 0;
    st->stored = 0;
    st->op = 0;
    st->new_number = 1;
    st->error = 0;
}

static void calc_eval(calc_state_t* st) {
    if (st->error || st->op == 0) return;

    int64_t res = st->display;
    if (st->op == 1) {
        res = st->stored + st->display;
    } else if (st->op == 2) {
        res = st->stored - st->display;
    } else if (st->op == 3) {
        res = st->stored * st->display;
    } else if (st->op == 4) {
        if (st->display == 0) {
            st->error = 1;
            st->op = 0;
            st->new_number = 1;
            return;
        }
        res = st->stored / st->display;
    }

    if (res > CALC_MAX_ABS || res < -CALC_MAX_ABS) {
        st->error = 1;
        st->op = 0;
        st->new_number = 1;
        return;
    }

    st->display = res;
    st->op = 0;
    st->new_number = 1;
}

static void calc_input_digit(calc_state_t* st, int digit) {
    if (st->error) {
        calc_reset(st);
    }
    if (st->new_number) {
        st->display = digit;
        st->new_number = 0;
        return;
    }
    int64_t next = (st->display < 0)
                 ? (st->display * 10 - digit)
                 : (st->display * 10 + digit);
    if (next > CALC_MAX_ABS || next < -CALC_MAX_ABS) return;
    st->display = next;
}

static void calc_input_backspace(calc_state_t* st) {
    if (st->error) {
        calc_reset(st);
        return;
    }
    if (st->new_number) return;
    st->display /= 10;
}

static void calc_input_op(calc_state_t* st, int new_op) {
    if (st->error) return;
    if (st->op != 0 && !st->new_number) {
        calc_eval(st);
        if (st->error) return;
    }
    st->op = new_op;
    st->stored = st->display;
    st->new_number = 1;
}

static void calc_press_idx(calc_state_t* st, int idx) {
    if (idx < 0 || idx >= 16) return;
    char ch = s_btn_labels[idx][0];
    if (ch >= '0' && ch <= '9') {
        calc_input_digit(st, ch - '0');
    } else if (ch == 'C') {
        calc_reset(st);
    } else if (ch == '=') {
        calc_eval(st);
    } else if (ch == '+') {
        calc_input_op(st, 1);
    } else if (ch == '-') {
        calc_input_op(st, 2);
    } else if (ch == '*') {
        calc_input_op(st, 3);
    } else if (ch == '/') {
        calc_input_op(st, 4);
    }
}

static void draw_colored_btn(const pxe_win_surface_t* surf, int bx, int by, int bw, int bh,
                             const char* text, int pressed) {
    char c = text[0];
    uint32_t bg, hl, sh, fg;
    if (c == 'C') {
        bg = PXE_COL_DARK_RED;
        hl = PXE_COL_RED;
        sh = PXE_COL_BLACK;
        fg = PXE_COL_WHITE;
    } else if (c == '=') {
        bg = PXE_COL_DARK_GREEN;
        hl = PXE_COL_GREEN;
        sh = PXE_COL_BLACK;
        fg = PXE_COL_WHITE;
    } else if (c == '+' || c == '-' || c == '*' || c == '/') {
        bg = PXE_COL_BROWN;
        hl = PXE_COL_YELLOW;
        sh = PXE_COL_DARK_CYAN;
        fg = PXE_COL_BLACK;
    } else {
        pxe_draw_button(surf, bx, by, bw, bh, text, pressed, 1);
        return;
    }

    pxe_fill_rect(surf, bx, by, bw, bh, bg);
    pxe_draw_hline(surf, bx, by, bw, pressed ? sh : hl);
    pxe_draw_vline(surf, bx, by, bh, pressed ? sh : hl);
    pxe_draw_hline(surf, bx, by + bh - 1, bw, pressed ? hl : sh);
    pxe_draw_vline(surf, bx + bw - 1, by, bh, pressed ? hl : sh);

    int tw = (int)strlen(text) * 8;
    int tx = bx + (bw - tw) / 2 + (pressed ? 1 : 0);
    int ty = by + (bh - 16) / 2 + (pressed ? 1 : 0);
    pxe_draw_text(surf, tx, ty, text, fg);
}

static void format_i64(int64_t val, char* out, size_t cap) {
    if (cap < 2) return;
    if (val == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    int neg = 0;
    uint64_t u;
    if (val < 0) {
        neg = 1;
        u = (uint64_t)(-val);
    } else {
        u = (uint64_t)val;
    }
    char tmp[24];
    int n = 0;
    while (u > 0 && n < 20) {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    size_t pos = 0;
    if (neg && pos + 1 < cap) out[pos++] = '-';
    while (n > 0 && pos + 1 < cap) out[pos++] = tmp[--n];
    out[pos] = '\0';
}

static void render_calc(const pxe_win_surface_t* surf, const calc_state_t* st) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    int disp_w = surf->width - 16;
    pxe_draw_bevel_box(surf, 8, 8, disp_w, 26, 1, PXE_COL_WHITE);

    if (st->op >= 1 && st->op <= 4 && !st->error) {
        const char* op_ch = (st->op == 1) ? "+" : (st->op == 2) ? "-" : (st->op == 3) ? "*" : "/";
        pxe_draw_text_small(surf, 12, 17, op_ch, PXE_COL_DARK_GRAY);
    }

    if (st->error) {
        pxe_draw_text(surf, surf->width - 14 - 5 * 8, 13, "Error", PXE_COL_DARK_RED);
    } else {
        char buf[24];
        format_i64(st->display, buf, sizeof(buf));
        int tw = (int)strlen(buf) * 8;
        int tx = surf->width - 14 - tw;
        if (tx < 20) tx = 20;
        pxe_draw_text(surf, tx, 13, buf, PXE_COL_BLACK);
    }

    for (int i = 0; i < 16; i++) {
        int bx, by, bw, bh;
        btn_rect(i, &bx, &by, &bw, &bh);
        draw_colored_btn(surf, bx, by, bw, bh, s_btn_labels[i], st->pressed_idx == i);
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 120;
    desc.y = 72;
    desc.w = 172;
    desc.h = 160;
    desc.min_w = 172;
    desc.min_h = 160;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_DEFAULT;
    strcpy(desc.title, "Calculator");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    calc_state_t st;
    memset(&st, 0, sizeof(st));
    calc_reset(&st);
    st.pressed_idx = -1;

    render_calc(&surf, &st);
    pxe_win_present(win_id);

    int running = 1;
    while (running) {
        pxe_event_t ev;
        int rc = pxe_win_wait_event(win_id, &ev);
        if (rc < 0) break;
        if (rc == 0) continue;

        int dirty = 0;
        switch (ev.type) {
            case PXE_EV_CLOSE:
                running = 0;
                break;
            case PXE_EV_REDRAW:
            case PXE_EV_ACTIVATE:
                dirty = 1;
                break;
            case PXE_EV_MOUSE_DOWN:
                if (ev.buttons & PXE_BTN_LEFT) {
                    int hit = -1;
                    for (int i = 0; i < 16; i++) {
                        int bx, by, bw, bh;
                        btn_rect(i, &bx, &by, &bw, &bh);
                        if (pxe_point_in_rect(ev.x, ev.y, bx, by, bw, bh)) {
                            hit = i;
                            break;
                        }
                    }
                    if (hit != st.pressed_idx) {
                        st.pressed_idx = hit;
                        dirty = 1;
                    }
                }
                break;
            case PXE_EV_MOUSE_UP:
                if (st.pressed_idx >= 0) {
                    int idx = st.pressed_idx;
                    int bx, by, bw, bh;
                    btn_rect(idx, &bx, &by, &bw, &bh);
                    st.pressed_idx = -1;
                    if (pxe_point_in_rect(ev.x, ev.y, bx, by, bw, bh)) {
                        calc_press_idx(&st, idx);
                    }
                    dirty = 1;
                }
                break;
            case PXE_EV_KEY_DOWN:
                if (ev.key >= '0' && ev.key <= '9') {
                    calc_input_digit(&st, ev.key - '0');
                    dirty = 1;
                } else if (ev.key == '+') {
                    calc_input_op(&st, 1);
                    dirty = 1;
                } else if (ev.key == '-') {
                    calc_input_op(&st, 2);
                    dirty = 1;
                } else if (ev.key == '*') {
                    calc_input_op(&st, 3);
                    dirty = 1;
                } else if (ev.key == '/') {
                    calc_input_op(&st, 4);
                    dirty = 1;
                } else if (ev.key == '=' || ev.key == PXE_KEY_ENTER) {
                    calc_eval(&st);
                    dirty = 1;
                } else if (ev.key == 'c' || ev.key == 'C' || ev.key == PXE_KEY_ESC) {
                    calc_reset(&st);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_BACKSPACE) {
                    calc_input_backspace(&st);
                    dirty = 1;
                }
                break;
            default:
                break;
        }

        if (dirty && running) {
            render_calc(&surf, &st);
            pxe_win_present(win_id);
        }
    }

    pxe_win_destroy(win_id);
    return 0;
}
