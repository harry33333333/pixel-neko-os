#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>

typedef struct {
    int      board[4][4];
    int      empty_r;
    int      empty_c;
    int      moves;
    int      is_won;
    int      new_pressed;
    uint32_t rng;
} puzzle_state_t;

static uint32_t puzzle_rand(puzzle_state_t* st) {
    st->rng = st->rng * 1103515245u + 12345u;
    return (st->rng >> 16) & 0x7FFFu;
}

static int board_is_solved(const puzzle_state_t* st) {
    int expected = 1;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (r == 3 && c == 3) {
                if (st->board[r][c] != 0) return 0;
            } else {
                if (st->board[r][c] != expected) return 0;
                expected++;
            }
        }
    }
    return 1;
}

static void puzzle_shuffle(puzzle_state_t* st) {
    int val = 1;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            st->board[r][c] = val++;
        }
    }
    st->board[3][3] = 0;
    st->empty_r = 3;
    st->empty_c = 3;
    st->moves = 0;
    st->is_won = 0;

    st->rng ^= (uint32_t)pxe_get_ticks() ^ 0xDEADBEEFu;
    if (st->rng == 0) st->rng = 123456789u;

    static const int dr[4] = { -1, 1, 0, 0 };
    static const int dc[4] = { 0, 0, -1, 1 };
    int last_move = -1;

    for (int i = 0; i < 150 || board_is_solved(st); i++) {
        int valid[4];
        int n = 0;
        for (int m = 0; m < 4; m++) {
            if (last_move != -1 && (m ^ 1) == last_move) continue;
            int nr = st->empty_r + dr[m];
            int nc = st->empty_c + dc[m];
            if (nr >= 0 && nr < 4 && nc >= 0 && nc < 4) {
                valid[n++] = m;
            }
        }
        if (n == 0) {
            last_move = -1;
            continue;
        }
        int chosen = valid[puzzle_rand(st) % (uint32_t)n];
        int nr = st->empty_r + dr[chosen];
        int nc = st->empty_c + dc[chosen];
        st->board[st->empty_r][st->empty_c] = st->board[nr][nc];
        st->board[nr][nc] = 0;
        st->empty_r = nr;
        st->empty_c = nc;
        last_move = chosen;
    }
}

static int puzzle_try_move(puzzle_state_t* st, int r, int c) {
    if (r < 0 || r >= 4 || c < 0 || c >= 4) return 0;
    int dr = r - st->empty_r;
    int dc = c - st->empty_c;
    if (dr < 0) dr = -dr;
    if (dc < 0) dc = -dc;
    if (!((dr == 1 && dc == 0) || (dr == 0 && dc == 1))) return 0;

    st->board[st->empty_r][st->empty_c] = st->board[r][c];
    st->board[r][c] = 0;
    st->empty_r = r;
    st->empty_c = c;
    st->moves++;

    if (!st->is_won && board_is_solved(st)) {
        st->is_won = 1;
        pxe_beep();
    }
    return 1;
}

static void puzzle_layout(const pxe_win_surface_t* surf,
                          int* start_x, int* start_y,
                          int* btn_x, int* btn_y) {
    const int grid_w = 128;
    *start_x = (surf->width - grid_w) / 2;
    *start_y = 28;
    *btn_x = (surf->width - 88) / 2;
    *btn_y = *start_y + grid_w + 10;
}

static void render_puzzle(const pxe_win_surface_t* surf, const puzzle_state_t* st) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    int start_x, start_y, btn_x, btn_y;
    puzzle_layout(surf, &start_x, &start_y, &btn_x, &btn_y);

    if (st->is_won) {
        const char* msg = "SOLVED! CONGRATULATIONS!";
        int tx = (surf->width - (int)strlen(msg) * 8) / 2;
        if (tx < 4) tx = 4;
        pxe_draw_text(surf, tx, 6, msg, PXE_COL_DARK_GREEN);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "Moves: %d", st->moves);
        int tx = (surf->width - (int)strlen(buf) * 8) / 2;
        pxe_draw_text(surf, tx, 6, buf, PXE_COL_BLACK);
    }

    pxe_draw_bevel_box(surf, start_x - 2, start_y - 2, 132, 132, 1, PXE_COL_DARK_GRAY);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int val = st->board[r][c];
            int tx = start_x + c * 32;
            int ty = start_y + r * 32;
            if (val == 0) {
                pxe_fill_rect(surf, tx, ty, 32, 32, PXE_COL_DARK_GRAY);
            } else {
                pxe_draw_bevel_box(surf, tx, ty, 32, 32, 0, PXE_COL_LIGHT_GRAY);
                char num[4];
                snprintf(num, sizeof(num), "%d", val);
                int nw = (int)strlen(num) * 8;
                pxe_draw_text(surf, tx + (32 - nw) / 2, ty + 8, num, PXE_COL_BLACK);
            }
        }
    }

    pxe_draw_button(surf, btn_x, btn_y, 88, 22, "New Game", st->new_pressed, 1);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 150;
    desc.y = 68;
    desc.w = 212;
    desc.h = 200;
    desc.min_w = 212;
    desc.min_h = 200;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_DEFAULT;
    strcpy(desc.title, "15-Puzzle");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    puzzle_state_t st;
    memset(&st, 0, sizeof(st));
    st.rng = 123456789u;
    puzzle_shuffle(&st);

    render_puzzle(&surf, &st);
    pxe_win_present(win_id);

    int running = 1;
    while (running) {
        pxe_event_t ev;
        int rc = pxe_win_wait_event(win_id, &ev);
        if (rc < 0) break;
        if (rc == 0) continue;

        int dirty = 0;
        int start_x, start_y, btn_x, btn_y;
        puzzle_layout(&surf, &start_x, &start_y, &btn_x, &btn_y);

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
                    if (pxe_point_in_rect(ev.x, ev.y, btn_x, btn_y, 88, 22)) {
                        st.new_pressed = 1;
                        dirty = 1;
                    } else if (pxe_point_in_rect(ev.x, ev.y, start_x, start_y, 128, 128)) {
                        int c = (ev.x - start_x) / 32;
                        int r = (ev.y - start_y) / 32;
                        if (puzzle_try_move(&st, r, c)) {
                            dirty = 1;
                        }
                    }
                }
                break;
            case PXE_EV_MOUSE_UP:
                if (st.new_pressed) {
                    int inside = pxe_point_in_rect(ev.x, ev.y, btn_x, btn_y, 88, 22);
                    st.new_pressed = 0;
                    if (inside) {
                        puzzle_shuffle(&st);
                    }
                    dirty = 1;
                }
                break;
            case PXE_EV_KEY_DOWN:
                if (ev.key == PXE_KEY_UP) {
                    dirty = puzzle_try_move(&st, st.empty_r + 1, st.empty_c);
                } else if (ev.key == PXE_KEY_DOWN) {
                    dirty = puzzle_try_move(&st, st.empty_r - 1, st.empty_c);
                } else if (ev.key == PXE_KEY_LEFT) {
                    dirty = puzzle_try_move(&st, st.empty_r, st.empty_c + 1);
                } else if (ev.key == PXE_KEY_RIGHT) {
                    dirty = puzzle_try_move(&st, st.empty_r, st.empty_c - 1);
                } else if (ev.key == 'r' || ev.key == 'R' || ev.key == 'n' || ev.key == 'N') {
                    puzzle_shuffle(&st);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_ESC) {
                    running = 0;
                }
                break;
            default:
                break;
        }

        if (dirty && running) {
            render_puzzle(&surf, &st);
            pxe_win_present(win_id);
        }
    }

    pxe_win_destroy(win_id);
    return 0;
}
