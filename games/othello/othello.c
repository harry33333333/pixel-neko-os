/*
 * Reversi (Othello) - Classic Windows 3.x style board game for Pixel Neko OS.
 *
 * Controls:
 *   Left click      - place a disc on a highlighted square
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

#define WIN_W        440
#define WIN_H        430

#define BOARD_N      8
#define CELL_SIZE    40
#define BOARD_PX     (BOARD_N * CELL_SIZE)
#define BOARD_X      60
#define BOARD_Y      54

#define SCORE_BLACK_X 190
#define SCORE_WHITE_X 270
#define SCORE_Y       6
#define SCORE_W       72
#define SCORE_H       38

#define NEW_BTN_X     352
#define NEW_BTN_Y     10
#define NEW_BTN_W     76
#define NEW_BTN_H     28

#define STATUS_Y      382
#define STATUS_H      40
#define STATUS_LINE_Y 388
#define STATUS_HINT_Y 406

#define EMPTY 0
#define BLACK 1
#define WHITE 2

#define RGB(r, g, b) ((uint32_t)(((uint32_t)(r) << 16) | \
                                 ((uint32_t)(g) << 8) | \
                                  (uint32_t)(b)))

#define COL_BG           RGB(0, 128, 0)
#define COL_BG_DARK      RGB(0, 80, 0)
#define COL_GRID         RGB(0, 64, 0)
#define COL_BOARD_FRAME  RGB(0, 48, 0)
#define COL_TITLE        RGB(255, 255, 0)
#define COL_SUBTITLE     RGB(200, 255, 200)
#define COL_SCORE_BG     RGB(0, 64, 0)
#define COL_SCORE_LABEL  RGB(180, 255, 180)
#define COL_SCORE_VALUE  RGB(255, 255, 255)
#define COL_STATUS_BG    RGB(192, 192, 192)
#define COL_STATUS_DARK  RGB(128, 128, 128)
#define COL_STATUS_LIGHT RGB(255, 255, 255)
#define COL_STATUS_TEXT  PXE_COL_BLACK
#define COL_LEGAL        PXE_COL_YELLOW
#define COL_LAST_MOVE    RGB(220, 40, 40)
#define COL_PIECE_BLACK  PXE_COL_BLACK
#define COL_PIECE_WHITE  PXE_COL_WHITE
#define COL_PIECE_GRAY   RGB(80, 80, 80)
#define COL_PIECE_LIGHT  RGB(230, 230, 230)

typedef struct {
    uint8_t  board[BOARD_N * BOARD_N];
    int      turn;
    int      game_over;
    int      winner;
    int      black_count;
    int      white_count;
    int      last_move;
    int      ai_thinking;
    uint64_t ai_deadline;
    int      new_pressed;
    char     notice[48];
} othello_game_t;

typedef struct {
    othello_game_t game;
} othello_app_t;

static const int k_dirs[8][2] = {
    { -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, -1 },
    {  0,  1 }, {  1, -1 }, {  1, 0 }, {  1, 1 }
};

static int in_bounds(int r, int c) {
    return r >= 0 && r < BOARD_N && c >= 0 && c < BOARD_N;
}

static int opponent(int player) {
    return player == BLACK ? WHITE : BLACK;
}

static int count_flips(const uint8_t* board, int r, int c, int player) {
    if (!in_bounds(r, c) || board[r * BOARD_N + c] != EMPTY) return 0;

    int opp = opponent(player);
    int total = 0;
    for (int d = 0; d < 8; d++) {
        int rr = r + k_dirs[d][0];
        int cc = c + k_dirs[d][1];
        int n = 0;
        while (in_bounds(rr, cc) && board[rr * BOARD_N + cc] == opp) {
            n++;
            rr += k_dirs[d][0];
            cc += k_dirs[d][1];
        }
        if (n > 0 && in_bounds(rr, cc) &&
            board[rr * BOARD_N + cc] == player) {
            total += n;
        }
    }
    return total;
}

static int apply_move(uint8_t* board, int r, int c, int player) {
    if (!in_bounds(r, c) || board[r * BOARD_N + c] != EMPTY) return 0;

    int opp = opponent(player);
    int total = 0;

    for (int d = 0; d < 8; d++) {
        int rr = r + k_dirs[d][0];
        int cc = c + k_dirs[d][1];
        int n = 0;
        while (in_bounds(rr, cc) && board[rr * BOARD_N + cc] == opp) {
            n++;
            rr += k_dirs[d][0];
            cc += k_dirs[d][1];
        }
        if (n > 0 && in_bounds(rr, cc) &&
            board[rr * BOARD_N + cc] == player) {
            rr = r + k_dirs[d][0];
            cc = c + k_dirs[d][1];
            for (int i = 0; i < n; i++) {
                board[rr * BOARD_N + cc] = (uint8_t)player;
                rr += k_dirs[d][0];
                cc += k_dirs[d][1];
            }
            total += n;
        }
    }

    if (total > 0) {
        board[r * BOARD_N + c] = (uint8_t)player;
    }
    return total;
}

static int list_legal_moves(const uint8_t* board, int player, int* moves) {
    int count = 0;
    for (int idx = 0; idx < BOARD_N * BOARD_N; idx++) {
        if (board[idx] != EMPTY) continue;
        if (count_flips(board, idx / BOARD_N, idx % BOARD_N, player) <= 0) {
            continue;
        }
        if (moves) moves[count] = idx;
        count++;
    }
    return count;
}

static int has_legal_move(const uint8_t* board, int player) {
    return list_legal_moves(board, player, NULL) > 0;
}

static void update_counts(othello_game_t* g) {
    int black = 0;
    int white = 0;
    for (int i = 0; i < BOARD_N * BOARD_N; i++) {
        if (g->board[i] == BLACK) black++;
        else if (g->board[i] == WHITE) white++;
    }
    g->black_count = black;
    g->white_count = white;
}

static void finish_game(othello_app_t* app) {
    othello_game_t* g = &app->game;
    update_counts(g);
    g->game_over = 1;
    g->ai_thinking = 0;
    if (g->black_count > g->white_count) g->winner = BLACK;
    else if (g->white_count > g->black_count) g->winner = WHITE;
    else g->winner = 0;
    pxe_beep();
}

static void schedule_ai(othello_app_t* app) {
    othello_game_t* g = &app->game;
    if (g->game_over || g->turn != WHITE) return;
    g->ai_thinking = 1;
    g->ai_deadline = pxe_get_ticks() + 35; /* ~350 ms */
}

static void resolve_turn(othello_app_t* app) {
    othello_game_t* g = &app->game;
    g->notice[0] = '\0';
    if (g->game_over) return;

    if (has_legal_move(g->board, g->turn)) {
        if (g->turn == WHITE) schedule_ai(app);
        return;
    }

    int other = opponent(g->turn);
    if (has_legal_move(g->board, other)) {
        snprintf(g->notice, sizeof(g->notice), "%s has no move - %s plays",
                 g->turn == BLACK ? "Black" : "White",
                 other == BLACK ? "Black" : "White");
        g->turn = other;
        if (g->turn == WHITE) schedule_ai(app);
        return;
    }

    finish_game(app);
}

static void game_new(othello_app_t* app) {
    othello_game_t* g = &app->game;
    memset(g, 0, sizeof(*g));

    int mid = BOARD_N / 2;
    g->board[(mid - 1) * BOARD_N + (mid - 1)] = WHITE;
    g->board[(mid - 1) * BOARD_N + mid] = BLACK;
    g->board[mid * BOARD_N + (mid - 1)] = BLACK;
    g->board[mid * BOARD_N + mid] = WHITE;

    g->turn = BLACK;
    g->last_move = -1;
    update_counts(g);
    resolve_turn(app);
}

static void after_move(othello_app_t* app) {
    othello_game_t* g = &app->game;
    update_counts(g);
    g->turn = opponent(g->turn);
    resolve_turn(app);
}

static int human_move(othello_app_t* app, int idx) {
    othello_game_t* g = &app->game;
    if (g->game_over || g->ai_thinking || g->turn != BLACK) return 0;
    if (idx < 0 || idx >= BOARD_N * BOARD_N) return 0;

    int r = idx / BOARD_N;
    int c = idx % BOARD_N;
    if (count_flips(g->board, r, c, BLACK) <= 0) return 0;

    g->notice[0] = '\0';
    apply_move(g->board, r, c, BLACK);
    g->last_move = idx;
    after_move(app);
    return 1;
}

static const int k_weights[BOARD_N * BOARD_N] = {
    120, -20,  20,   5,   5,  20, -20, 120,
    -20, -40,  -5,  -5,  -5,  -5, -40, -20,
     20,  -5,  15,   3,   3,  15,  -5,  20,
      5,  -5,   3,   3,   3,   3,  -5,   5,
      5,  -5,   3,   3,   3,   3,  -5,   5,
     20,  -5,  15,   3,   3,  15,  -5,  20,
    -20, -40,  -5,  -5,  -5,  -5, -40, -20,
    120, -20,  20,   5,   5,  20, -20, 120
};

static int evaluate_board(const uint8_t* board, int player) {
    int opp = opponent(player);
    int score = 0;
    int diff = 0;

    for (int i = 0; i < BOARD_N * BOARD_N; i++) {
        if (board[i] == player) {
            score += k_weights[i];
            diff++;
        } else if (board[i] == opp) {
            score -= k_weights[i];
            diff--;
        }
    }

    int my_moves = list_legal_moves(board, player, NULL);
    int op_moves = list_legal_moves(board, opp, NULL);
    score += (my_moves - op_moves) * 8;
    score += diff;
    return score;
}

static int choose_ai_move(othello_app_t* app, const int* moves, int move_count) {
    int best_move = moves[0];
    int best_score = -1000000;

    for (int i = 0; i < move_count; i++) {
        uint8_t b1[BOARD_N * BOARD_N];
        memcpy(b1, app->game.board, sizeof(b1));
        apply_move(b1, moves[i] / BOARD_N, moves[i] % BOARD_N, WHITE);

        int opp_moves[BOARD_N * BOARD_N];
        int opp_count = list_legal_moves(b1, BLACK, opp_moves);
        int worst;

        if (opp_count <= 0) {
            worst = evaluate_board(b1, WHITE);
        } else {
            worst = 1000000;
            for (int j = 0; j < opp_count; j++) {
                uint8_t b2[BOARD_N * BOARD_N];
                memcpy(b2, b1, sizeof(b2));
                apply_move(b2, opp_moves[j] / BOARD_N,
                           opp_moves[j] % BOARD_N, BLACK);
                int value = evaluate_board(b2, WHITE);
                if (value < worst) worst = value;
            }
        }

        if (worst > best_score) {
            best_score = worst;
            best_move = moves[i];
        }
    }
    return best_move;
}

static void ai_make_move(othello_app_t* app) {
    othello_game_t* g = &app->game;
    g->ai_thinking = 0;
    if (g->game_over || g->turn != WHITE) return;

    int moves[BOARD_N * BOARD_N];
    int move_count = list_legal_moves(g->board, WHITE, moves);
    if (move_count <= 0) {
        resolve_turn(app);
        return;
    }

    int move = choose_ai_move(app, moves, move_count);
    g->notice[0] = '\0';
    apply_move(g->board, move / BOARD_N, move % BOARD_N, WHITE);
    g->last_move = move;
    after_move(app);
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
    pxe_draw_hline(surf, x, y, w, COL_BOARD_FRAME);
    pxe_draw_vline(surf, x, y, h, COL_BOARD_FRAME);
    pxe_draw_hline(surf, x, y + h - 1, w, COL_SCORE_LABEL);
    pxe_draw_vline(surf, x + w - 1, y, h, COL_SCORE_LABEL);
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

static void draw_piece(const pxe_win_surface_t* surf, int cx, int cy, int player) {
    if (player == BLACK) {
        draw_disc(surf, cx + 1, cy + 1, 16, COL_BG_DARK);
        draw_disc(surf, cx, cy, 16, COL_PIECE_BLACK);
        draw_disc(surf, cx - 1, cy - 1, 13, COL_PIECE_GRAY);
        pxe_fill_rect(surf, cx - 6, cy - 6, 4, 4, COL_PIECE_LIGHT);
    } else {
        draw_disc(surf, cx + 1, cy + 1, 16, COL_BG_DARK);
        draw_disc(surf, cx, cy, 16, COL_PIECE_BLACK);
        draw_disc(surf, cx, cy, 14, COL_PIECE_WHITE);
        pxe_put_pixel(surf, cx + 6, cy + 8, COL_STATUS_DARK);
        pxe_put_pixel(surf, cx + 7, cy + 7, COL_STATUS_DARK);
        pxe_put_pixel(surf, cx + 8, cy + 6, COL_STATUS_DARK);
        pxe_put_pixel(surf, cx + 8, cy + 5, COL_STATUS_DARK);
    }
}

static void draw_score_box(const pxe_win_surface_t* surf, int x, int y,
                           const char* label, int value) {
    draw_sunken_box(surf, x, y, SCORE_W, SCORE_H, COL_SCORE_BG);
    draw_text_center(surf, x + SCORE_W / 2, y + 4, label,
                     COL_SCORE_LABEL, 1);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d", value);
    draw_text_center(surf, x + SCORE_W / 2, y + 16, buf,
                     COL_SCORE_VALUE, 0);
}

static void draw_board(const pxe_win_surface_t* surf, const othello_app_t* app) {
    const othello_game_t* g = &app->game;

    pxe_fill_rect(surf, BOARD_X - 4, BOARD_Y - 4,
                  BOARD_PX + 8, BOARD_PX + 8, COL_BOARD_FRAME);
    pxe_fill_rect(surf, BOARD_X - 2, BOARD_Y - 2,
                  BOARD_PX + 4, BOARD_PX + 4, COL_BG);

    for (int i = 0; i <= BOARD_N; i++) {
        int x = BOARD_X + i * CELL_SIZE;
        int y = BOARD_Y + i * CELL_SIZE;
        pxe_draw_vline(surf, x, BOARD_Y, BOARD_PX, COL_GRID);
        pxe_draw_hline(surf, BOARD_X, y, BOARD_PX, COL_GRID);
    }

    int show_hints = (!g->game_over && !g->ai_thinking && g->turn == BLACK);

    for (int r = 0; r < BOARD_N; r++) {
        for (int c = 0; c < BOARD_N; c++) {
            int idx = r * BOARD_N + c;
            int x = BOARD_X + c * CELL_SIZE;
            int y = BOARD_Y + r * CELL_SIZE;
            int cx = x + CELL_SIZE / 2;
            int cy = y + CELL_SIZE / 2;

            if (g->board[idx] != EMPTY) {
                draw_piece(surf, cx, cy, g->board[idx]);
            } else if (show_hints && count_flips(g->board, r, c, BLACK) > 0) {
                draw_disc(surf, cx, cy, 4, COL_LEGAL);
            }

            if (idx == g->last_move) {
                pxe_fill_rect(surf, x + 3, y + 3, 3, 3, COL_LAST_MOVE);
            }
        }
    }
}

static void render_game(const pxe_win_surface_t* surf, const othello_app_t* app) {
    const othello_game_t* g = &app->game;

    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, COL_BG);

    pxe_draw_text(surf, 12, 8, "REVERSI", COL_TITLE);
    pxe_draw_text_small(surf, 12, 28, "You: Black   CPU: White", COL_SUBTITLE);

    draw_score_box(surf, SCORE_BLACK_X, SCORE_Y, "BLACK", g->black_count);
    draw_score_box(surf, SCORE_WHITE_X, SCORE_Y, "WHITE", g->white_count);

    pxe_draw_button(surf, NEW_BTN_X, NEW_BTN_Y, NEW_BTN_W, NEW_BTN_H,
                    "New Game", g->new_pressed, 1);

    draw_board(surf, app);

    pxe_fill_rect(surf, 0, STATUS_Y, WIN_W, STATUS_H, COL_STATUS_BG);
    pxe_draw_hline(surf, 0, STATUS_Y, WIN_W, COL_STATUS_DARK);
    pxe_draw_hline(surf, 0, STATUS_Y + 1, WIN_W, COL_STATUS_LIGHT);

    char line[64];
    if (g->game_over) {
        if (g->winner == BLACK) {
            snprintf(line, sizeof(line), "Black wins! %d : %d",
                     g->black_count, g->white_count);
        } else if (g->winner == WHITE) {
            snprintf(line, sizeof(line), "White wins! %d : %d",
                     g->black_count, g->white_count);
        } else {
            snprintf(line, sizeof(line), "Draw %d : %d",
                     g->black_count, g->white_count);
        }
    } else {
        snprintf(line, sizeof(line), "Black %d : %d White   %s to move",
                 g->black_count, g->white_count,
                 g->turn == BLACK ? "Black" : "White");
    }
    draw_text_center(surf, WIN_W / 2, STATUS_LINE_Y, line, COL_STATUS_TEXT, 0);

    const char* hint;
    if (g->game_over) {
        hint = "Ctrl+N: New Game";
    } else if (g->notice[0]) {
        hint = g->notice;
    } else if (g->ai_thinking) {
        hint = "Computer is thinking...";
    } else {
        hint = "Click a highlighted square    Ctrl+N: New Game";
    }
    draw_text_center(surf, WIN_W / 2, STATUS_HINT_Y, hint, COL_STATUS_TEXT, 1);
}

static int cell_at(int mx, int my) {
    if (mx < BOARD_X || my < BOARD_Y) return -1;
    int c = (mx - BOARD_X) / CELL_SIZE;
    int r = (my - BOARD_Y) / CELL_SIZE;
    if (c < 0 || c >= BOARD_N || r < 0 || r >= BOARD_N) return -1;
    return r * BOARD_N + c;
}

static int handle_mouse_down(othello_app_t* app, const pxe_event_t* ev) {
    if (!(ev->buttons & PXE_BTN_LEFT)) return 0;

    if (pxe_point_in_rect(ev->x, ev->y, NEW_BTN_X, NEW_BTN_Y,
                          NEW_BTN_W, NEW_BTN_H)) {
        app->game.new_pressed = 1;
        return 1;
    }

    int idx = cell_at(ev->x, ev->y);
    if (idx >= 0) {
        return human_move(app, idx);
    }
    return 0;
}

static int handle_mouse_up(othello_app_t* app, const pxe_event_t* ev) {
    if (!app->game.new_pressed) return 0;

    app->game.new_pressed = 0;
    if (pxe_point_in_rect(ev->x, ev->y, NEW_BTN_X, NEW_BTN_Y,
                          NEW_BTN_W, NEW_BTN_H)) {
        game_new(app);
    }
    return 1;
}

static int handle_key(othello_app_t* app, int key, uint32_t modifiers) {
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
    strcpy(desc.title, "Reversi");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    othello_app_t app;
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
