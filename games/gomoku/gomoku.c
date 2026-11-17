/*
 * Gomoku (Five in a Row) for Pixel Neko OS.
 *
 * Controls:
 *   Left click      - place a stone on an empty intersection
 *   Ctrl+N / Ctrl+R - start a new game
 *   Mouse           - New Game button
 *   Esc             - close the application
 *
 * The human plays Black; the computer plays White.
 */

#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>

#define WIN_W        520
#define WIN_H        430

#define BOARD_N      15
#define CELL_SIZE    24
#define GRID_X       80
#define GRID_Y       56
#define GRID_PX      ((BOARD_N - 1) * CELL_SIZE)

#define BOARD_RECT_X (GRID_X - 12)
#define BOARD_RECT_Y (GRID_Y - 12)
#define BOARD_RECT_W (GRID_PX + 24)
#define BOARD_RECT_H (GRID_PX + 24)

#define STATUS_X     200
#define STATUS_Y     6
#define STATUS_W     220
#define STATUS_H     34

#define NEW_BTN_X    430
#define NEW_BTN_Y    8
#define NEW_BTN_W    78
#define NEW_BTN_H    28

#define FOOTER_Y     412

#define EMPTY 0
#define BLACK 1
#define WHITE 2

#define RGB(r, g, b) ((uint32_t)(((uint32_t)(r) << 16) | \
                                 ((uint32_t)(g) << 8) | \
                                  (uint32_t)(b)))

#define COL_BG           RGB(192, 192, 192)
#define COL_BOARD        RGB(222, 184, 135)
#define COL_BOARD_DARK   RGB(150, 105, 55)
#define COL_BOARD_LINE   RGB(90, 60, 30)
#define COL_TITLE        PXE_COL_BLACK
#define COL_SUBTITLE     RGB(120, 80, 40)
#define COL_STATUS_BG    RGB(232, 232, 232)
#define COL_STATUS_DARK  RGB(128, 128, 128)
#define COL_STATUS_LIGHT RGB(255, 255, 255)
#define COL_STONE_BLACK  PXE_COL_BLACK
#define COL_STONE_WHITE  PXE_COL_WHITE
#define COL_STONE_GRAY   RGB(80, 80, 80)
#define COL_STONE_LIGHT  RGB(240, 240, 240)
#define COL_LAST         RGB(220, 40, 40)
#define COL_WIN          PXE_COL_DARK_RED

typedef struct {
    uint8_t  board[BOARD_N * BOARD_N];
    int      turn;
    int      game_over;
    int      winner;
    int      last_move;
    int      moves;
    int      win_start;
    int      win_end;
    int      ai_thinking;
    uint64_t ai_deadline;
    int      new_pressed;
} gomoku_game_t;

typedef struct {
    gomoku_game_t game;
} gomoku_app_t;

static const int k_dirs[4][2] = {
    { 0, 1 }, { 1, 0 }, { 1, 1 }, { 1, -1 }
};

static int in_bounds(int r, int c) {
    return r >= 0 && r < BOARD_N && c >= 0 && c < BOARD_N;
}

static int opponent(int player) {
    return player == BLACK ? WHITE : BLACK;
}

static int check_win_at(const uint8_t* board, int idx, int player,
                        int* start, int* end) {
    int r = idx / BOARD_N;
    int c = idx % BOARD_N;

    for (int d = 0; d < 4; d++) {
        int dr = k_dirs[d][0];
        int dc = k_dirs[d][1];

        int sr = r;
        int sc = c;
        while (in_bounds(sr - dr, sc - dc) &&
               board[(sr - dr) * BOARD_N + (sc - dc)] == player) {
            sr -= dr;
            sc -= dc;
        }

        int er = r;
        int ec = c;
        while (in_bounds(er + dr, ec + dc) &&
               board[(er + dr) * BOARD_N + (ec + dc)] == player) {
            er += dr;
            ec += dc;
        }

        int count = 0;
        int rr = sr;
        int cc = sc;
        for (;;) {
            count++;
            if (rr == er && cc == ec) break;
            rr += dr;
            cc += dc;
        }

        if (count >= 5) {
            *start = sr * BOARD_N + sc;
            *end = er * BOARD_N + ec;
            return 1;
        }
    }
    return 0;
}

static int board_is_full(const uint8_t* board) {
    for (int i = 0; i < BOARD_N * BOARD_N; i++) {
        if (board[i] == EMPTY) return 0;
    }
    return 1;
}

static int pattern_score_place(const uint8_t* board, int idx, int player) {
    int r = idx / BOARD_N;
    int c = idx % BOARD_N;
    int total = 0;

    for (int d = 0; d < 4; d++) {
        int dr = k_dirs[d][0];
        int dc = k_dirs[d][1];
        int count = 1;
        int open1 = 0;
        int open2 = 0;

        int rr = r + dr;
        int cc = c + dc;
        while (in_bounds(rr, cc) && board[rr * BOARD_N + cc] == player) {
            count++;
            rr += dr;
            cc += dc;
        }
        if (in_bounds(rr, cc) && board[rr * BOARD_N + cc] == EMPTY) {
            open1 = 1;
        }

        rr = r - dr;
        cc = c - dc;
        while (in_bounds(rr, cc) && board[rr * BOARD_N + cc] == player) {
            count++;
            rr -= dr;
            cc -= dc;
        }
        if (in_bounds(rr, cc) && board[rr * BOARD_N + cc] == EMPTY) {
            open2 = 1;
        }

        int score = 0;
        if (count >= 5) {
            score = 1000000;
        } else if (count == 4) {
            score = (open1 && open2) ? 100000 : ((open1 || open2) ? 12000 : 0);
        } else if (count == 3) {
            score = (open1 && open2) ? 12000 : ((open1 || open2) ? 1200 : 0);
        } else if (count == 2) {
            score = (open1 && open2) ? 1200 : ((open1 || open2) ? 120 : 0);
        } else {
            score = (open1 && open2) ? 20 : ((open1 || open2) ? 2 : 0);
        }
        total += score;
    }
    return total;
}

static int ai_choose_move(const gomoku_game_t* g) {
    int best_move = -1;
    int best_score = -1000000000;

    for (int idx = 0; idx < BOARD_N * BOARD_N; idx++) {
        if (g->board[idx] != EMPTY) continue;

        int my_score = pattern_score_place(g->board, idx, WHITE);
        int op_score = pattern_score_place(g->board, idx, BLACK);

        if (my_score >= 1000000) return idx;

        int total = my_score * 12 + op_score * 10;
        if (op_score >= 1000000) total += 5000000;

        int r = idx / BOARD_N;
        int c = idx % BOARD_N;
        int dist = (r > 7 ? r - 7 : 7 - r) + (c > 7 ? c - 7 : 7 - c);
        total -= dist * 2;

        if (total > best_score) {
            best_score = total;
            best_move = idx;
        }
    }
    return best_move;
}

static void schedule_ai(gomoku_app_t* app) {
    gomoku_game_t* g = &app->game;
    if (g->game_over || g->turn != WHITE) return;
    g->ai_thinking = 1;
    g->ai_deadline = pxe_get_ticks() + 35;
}

static int place_stone(gomoku_app_t* app, int idx) {
    gomoku_game_t* g = &app->game;
    if (g->game_over || idx < 0 || idx >= BOARD_N * BOARD_N) return 0;
    if (g->board[idx] != EMPTY) return 0;

    int player = g->turn;
    g->board[idx] = (uint8_t)player;
    g->last_move = idx;
    g->moves++;

    int start = -1;
    int end = -1;
    if (check_win_at(g->board, idx, player, &start, &end)) {
        g->game_over = 1;
        g->winner = player;
        g->win_start = start;
        g->win_end = end;
        pxe_beep();
        return 1;
    }

    if (board_is_full(g->board)) {
        g->game_over = 1;
        g->winner = 0;
        pxe_beep();
        return 1;
    }

    g->turn = opponent(player);
    if (g->turn == WHITE) schedule_ai(app);
    return 1;
}

static void ai_make_move(gomoku_app_t* app) {
    gomoku_game_t* g = &app->game;
    g->ai_thinking = 0;
    if (g->game_over || g->turn != WHITE) return;

    int move = ai_choose_move(g);
    if (move < 0) {
        g->game_over = 1;
        g->winner = 0;
        return;
    }
    place_stone(app, move);
}

static void game_new(gomoku_app_t* app) {
    gomoku_game_t* g = &app->game;
    memset(g, 0, sizeof(*g));
    g->turn = BLACK;
    g->last_move = -1;
    g->win_start = -1;
    g->win_end = -1;
}

static void draw_text_center(const pxe_win_surface_t* surf, int cx, int y,
                             const char* text, uint32_t color, int small) {
    int len = (int)strlen(text);
    int char_w = small ? 6 : 8;
    int x = cx - (len * char_w) / 2;
    if (small) {
        pxe_draw_text_small(surf, x, y, text, color);
    } else {
        pxe_draw_text(surf, x, y, text, color);
    }
}

static void draw_sunken_box(const pxe_win_surface_t* surf, int x, int y,
                            int w, int h, uint32_t fill) {
    pxe_fill_rect(surf, x, y, w, h, fill);
    pxe_draw_hline(surf, x, y, w, COL_STATUS_DARK);
    pxe_draw_vline(surf, x, y, h, COL_STATUS_DARK);
    pxe_draw_hline(surf, x, y + h - 1, w, COL_STATUS_LIGHT);
    pxe_draw_vline(surf, x + w - 1, y, h, COL_STATUS_LIGHT);
}

static void draw_disc(const pxe_win_surface_t* surf, int cx, int cy,
                      int radius, uint32_t color) {
    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= r2) {
                pxe_put_pixel(surf, cx + dx, cy + dy, color);
            }
        }
    }
}

static void draw_stone(const pxe_win_surface_t* surf, int cx, int cy, int player) {
    if (player == BLACK) {
        draw_disc(surf, cx + 1, cy + 1, 9, COL_BOARD_DARK);
        draw_disc(surf, cx, cy, 9, COL_STONE_BLACK);
        draw_disc(surf, cx - 1, cy - 1, 7, COL_STONE_GRAY);
        pxe_fill_rect(surf, cx - 5, cy - 5, 3, 3, COL_STONE_LIGHT);
    } else {
        draw_disc(surf, cx + 1, cy + 1, 9, COL_BOARD_DARK);
        draw_disc(surf, cx, cy, 9, COL_STONE_BLACK);
        draw_disc(surf, cx, cy, 8, COL_STONE_WHITE);
        pxe_put_pixel(surf, cx + 4, cy + 5, COL_STONE_GRAY);
        pxe_put_pixel(surf, cx + 5, cy + 3, COL_STONE_GRAY);
    }
}

static void draw_board(const pxe_win_surface_t* surf, const gomoku_app_t* app) {
    const gomoku_game_t* g = &app->game;

    pxe_fill_rect(surf, BOARD_RECT_X, BOARD_RECT_Y,
                  BOARD_RECT_W, BOARD_RECT_H, COL_BOARD);
    pxe_draw_rect(surf, BOARD_RECT_X, BOARD_RECT_Y,
                  BOARD_RECT_W, BOARD_RECT_H, COL_BOARD_DARK);
    pxe_draw_rect(surf, BOARD_RECT_X + 1, BOARD_RECT_Y + 1,
                  BOARD_RECT_W - 2, BOARD_RECT_H - 2, COL_BOARD_DARK);

    for (int i = 0; i < BOARD_N; i++) {
        int x = GRID_X + i * CELL_SIZE;
        int y = GRID_Y + i * CELL_SIZE;
        pxe_draw_vline(surf, x, GRID_Y, GRID_PX + 1, COL_BOARD_LINE);
        pxe_draw_hline(surf, GRID_X, y, GRID_PX + 1, COL_BOARD_LINE);
    }

    static const int stars[5][2] = {
        { 3, 3 }, { 11, 3 }, { 7, 7 }, { 3, 11 }, { 11, 11 }
    };
    for (int i = 0; i < 5; i++) {
        int x = GRID_X + stars[i][0] * CELL_SIZE;
        int y = GRID_Y + stars[i][1] * CELL_SIZE;
        pxe_fill_rect(surf, x - 2, y - 2, 5, 5, COL_BOARD_LINE);
    }

    for (int r = 0; r < BOARD_N; r++) {
        for (int c = 0; c < BOARD_N; c++) {
            int idx = r * BOARD_N + c;
            if (g->board[idx] == EMPTY) continue;
            int x = GRID_X + c * CELL_SIZE;
            int y = GRID_Y + r * CELL_SIZE;
            draw_stone(surf, x, y, g->board[idx]);
        }
    }

    if (g->last_move >= 0) {
        int x = GRID_X + (g->last_move % BOARD_N) * CELL_SIZE;
        int y = GRID_Y + (g->last_move / BOARD_N) * CELL_SIZE;
        pxe_fill_rect(surf, x - 1, y - 1, 3, 3, COL_LAST);
    }

    if (g->game_over && g->winner != 0 &&
        g->win_start >= 0 && g->win_end >= 0) {
        int sr = g->win_start / BOARD_N;
        int sc = g->win_start % BOARD_N;
        int er = g->win_end / BOARD_N;
        int ec = g->win_end % BOARD_N;
        int dx = (er > sr) ? 1 : ((er < sr) ? -1 : 0);
        int dy = (ec > sc) ? 1 : ((ec < sc) ? -1 : 0);
        int steps = (er > sr ? er - sr : sr - er);
        int steps2 = (ec > sc ? ec - sc : sc - ec);
        if (steps2 > steps) steps = steps2;
        for (int i = 0; i <= steps; i++) {
            int x = GRID_X + (sc + dy * i) * CELL_SIZE;
            int y = GRID_Y + (sr + dx * i) * CELL_SIZE;
            pxe_fill_rect(surf, x - 1, y - 1, 3, 3, COL_WIN);
        }
    }
}

static void render_game(const pxe_win_surface_t* surf, const gomoku_app_t* app) {
    const gomoku_game_t* g = &app->game;

    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, COL_BG);

    pxe_draw_text(surf, 12, 6, "GOMOKU", COL_TITLE);

    char sub[48];
    snprintf(sub, sizeof(sub), "Five in a Row  Moves: %d", g->moves);
    pxe_draw_text_small(surf, 12, 26, sub, COL_SUBTITLE);

    draw_sunken_box(surf, STATUS_X, STATUS_Y, STATUS_W, STATUS_H, COL_STATUS_BG);

    const char* status;
    uint32_t status_color = PXE_COL_BLACK;
    if (g->game_over) {
        if (g->winner == BLACK) {
            status = "Black wins!";
            status_color = COL_WIN;
        } else if (g->winner == WHITE) {
            status = "White wins!";
            status_color = COL_WIN;
        } else {
            status = "Draw";
            status_color = PXE_COL_DARK_RED;
        }
    } else if (g->ai_thinking) {
        status = "White is thinking...";
        status_color = PXE_COL_DARK_BLUE;
    } else {
        status = (g->turn == BLACK) ? "Black to move" : "White to move";
    }
    draw_text_center(surf, STATUS_X + STATUS_W / 2, STATUS_Y + 9,
                     status, status_color, 0);

    pxe_draw_button(surf, NEW_BTN_X, NEW_BTN_Y, NEW_BTN_W, NEW_BTN_H,
                    "New Game", g->new_pressed, 1);

    draw_board(surf, app);

    draw_text_center(surf, WIN_W / 2, FOOTER_Y,
                     "Click an intersection to place a stone    Ctrl+N: New Game",
                     COL_SUBTITLE, 1);
}

static int cell_at(int mx, int my) {
    if (mx < GRID_X - 12 || mx > GRID_X + GRID_PX + 12 ||
        my < GRID_Y - 12 || my > GRID_Y + GRID_PX + 12) {
        return -1;
    }
    int c = (mx - GRID_X + CELL_SIZE / 2) / CELL_SIZE;
    int r = (my - GRID_Y + CELL_SIZE / 2) / CELL_SIZE;
    if (c < 0 || c >= BOARD_N || r < 0 || r >= BOARD_N) return -1;
    return r * BOARD_N + c;
}

static int handle_mouse_down(gomoku_app_t* app, const pxe_event_t* ev) {
    if (!(ev->buttons & PXE_BTN_LEFT)) return 0;

    if (pxe_point_in_rect(ev->x, ev->y, NEW_BTN_X, NEW_BTN_Y,
                          NEW_BTN_W, NEW_BTN_H)) {
        app->game.new_pressed = 1;
        return 1;
    }

    gomoku_game_t* g = &app->game;
    if (g->game_over || g->ai_thinking || g->turn != BLACK) return 0;

    int idx = cell_at(ev->x, ev->y);
    if (idx >= 0) {
        return place_stone(app, idx);
    }
    return 0;
}

static int handle_mouse_up(gomoku_app_t* app, const pxe_event_t* ev) {
    if (!app->game.new_pressed) return 0;

    app->game.new_pressed = 0;
    if (pxe_point_in_rect(ev->x, ev->y, NEW_BTN_X, NEW_BTN_Y,
                          NEW_BTN_W, NEW_BTN_H)) {
        game_new(app);
    }
    return 1;
}

static int handle_key(gomoku_app_t* app, int key, uint32_t modifiers) {
    if (key == PXE_KEY_ESC) return 2;

    if ((modifiers & PXE_MOD_CTRL) &&
        (key == 'n' || key == 'N' || key == 'r' || key == 'R')) {
        game_new(app);
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = -1;
    desc.y = -1;
    desc.w = WIN_W;
    desc.h = WIN_H;
    desc.min_w = WIN_W;
    desc.min_h = WIN_H;
    desc.format = PXE_CANVAS_RGB32;
    desc.flags = PXE_WIN_DEFAULT;
    strcpy(desc.title, "Gomoku");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    gomoku_app_t app;
    memset(&app, 0, sizeof(app));
    game_new(&app);

    render_game(&surf, &app);
    pxe_win_present(win_id);

    int running = 1;
    int dirty = 0;

    while (running) {
        pxe_event_t ev;
        int rc;
        while ((rc = pxe_win_poll_event(win_id, &ev)) > 0) {
            switch (ev.type) {
                case PXE_EV_CLOSE:
                    running = 0;
                    break;
                case PXE_EV_REDRAW:
                case PXE_EV_ACTIVATE:
                    dirty = 1;
                    break;
                case PXE_EV_RESIZE:
                    pxe_win_get_surface(win_id, &surf);
                    dirty = 1;
                    break;
                case PXE_EV_MOUSE_DOWN:
                    if (handle_mouse_down(&app, &ev)) dirty = 1;
                    break;
                case PXE_EV_MOUSE_UP:
                    if (handle_mouse_up(&app, &ev)) dirty = 1;
                    break;
                case PXE_EV_KEY_DOWN: {
                    int action = handle_key(&app, ev.key, ev.modifiers);
                    if (action == 2) running = 0;
                    else if (action == 1) dirty = 1;
                    break;
                }
                default:
                    break;
            }
            if (!running) break;
        }
        if (rc < 0) break;

        if (app.game.ai_thinking &&
            pxe_get_ticks() >= app.game.ai_deadline) {
            ai_make_move(&app);
            dirty = 1;
        }

        if (dirty && running) {
            render_game(&surf, &app);
            pxe_win_present(win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(20);
        }
    }

    pxe_win_destroy(win_id);
    return 0;
}
