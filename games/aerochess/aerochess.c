/*
 * Aero Chess (Aeroplane Chess / Ludo) for Pixel Neko OS.
 *
 * Controls:
 *   Click Roll / dice - roll the dice
 *   Click a plane     - move a highlighted plane
 *   Ctrl+N / Ctrl+R   - start a new game
 *   Space / Enter     - roll (or move the only legal plane)
 *   Esc               - close the application
 *
 * Four local players: Red, Green, Yellow and Blue.
 * Roll a 6 to launch a plane; land on an opponent to send it back.
 * First player to bring all four planes home wins.
 */

#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>

#define WIN_W        520
#define WIN_H        430

#define GRID_X       80
#define GRID_Y       44
#define CELL_SIZE    24
#define BOARD_PX     (15 * CELL_SIZE)

#define TURN_X       100
#define TURN_Y       6
#define TURN_W       130
#define TURN_H       34

#define DICE_X       238
#define DICE_Y       4
#define DICE_W       40
#define DICE_H       38

#define ROLL_X       286
#define ROLL_Y       8
#define ROLL_W       64
#define ROLL_H       28

#define NEW_BTN_X    412
#define NEW_BTN_Y    8
#define NEW_BTN_W    96
#define NEW_BTN_H    28

#define FOOTER_Y     412

#define RGB(r, g, b) ((uint32_t)(((uint32_t)(r) << 16) | \
                                 ((uint32_t)(g) << 8) | \
                                  (uint32_t)(b)))

#define COL_BG          RGB(192, 192, 192)
#define COL_BOARD       RGB(255, 248, 232)
#define COL_TRACK       RGB(252, 248, 238)
#define COL_TRACK_EDGE  RGB(120, 120, 120)
#define COL_TEXT        PXE_COL_BLACK
#define COL_SUBTITLE    RGB(90, 70, 40)
#define COL_DICE_FACE   RGB(255, 255, 255)
#define COL_DICE_EDGE   PXE_COL_BLACK
#define COL_DICE_PIP    PXE_COL_BLACK
#define COL_SELECT      PXE_COL_YELLOW
#define COL_CENTER      RGB(255, 220, 120)
#define COL_CENTER_TEXT RGB(120, 80, 20)

static const int k_start[4] = { 0, 13, 26, 39 };

static const int k_path[52][2] = {
    { 6, 1 }, { 6, 2 }, { 6, 3 }, { 6, 4 }, { 6, 5 },
    { 5, 6 }, { 4, 6 }, { 3, 6 }, { 2, 6 }, { 1, 6 }, { 0, 6 },
    { 0, 7 },
    { 0, 8 }, { 1, 8 }, { 2, 8 }, { 3, 8 }, { 4, 8 }, { 5, 8 },
    { 6, 9 }, { 6, 10 }, { 6, 11 }, { 6, 12 }, { 6, 13 }, { 6, 14 },
    { 7, 14 },
    { 8, 14 }, { 8, 13 }, { 8, 12 }, { 8, 11 }, { 8, 10 }, { 8, 9 },
    { 9, 8 }, { 10, 8 }, { 11, 8 }, { 12, 8 }, { 13, 8 }, { 14, 8 },
    { 14, 7 },
    { 14, 6 }, { 13, 6 }, { 12, 6 }, { 11, 6 }, { 10, 6 }, { 9, 6 },
    { 8, 5 }, { 8, 4 }, { 8, 3 }, { 8, 2 }, { 8, 1 }, { 8, 0 },
    { 7, 0 },
    { 6, 0 }
};

static const int k_home[4][6][2] = {
    { { 7, 1 }, { 7, 2 }, { 7, 3 }, { 7, 4 }, { 7, 5 }, { 7, 6 } },
    { { 1, 7 }, { 2, 7 }, { 3, 7 }, { 4, 7 }, { 5, 7 }, { 6, 7 } },
    { { 7, 13 }, { 7, 12 }, { 7, 11 }, { 7, 10 }, { 7, 9 }, { 7, 8 } },
    { { 13, 7 }, { 12, 7 }, { 11, 7 }, { 10, 7 }, { 9, 7 }, { 8, 7 } }
};

static const char* k_name[4] = { "RED", "GREEN", "YELLOW", "BLUE" };
static const char* k_name_zh[4] = { "红方", "绿方", "黄方", "蓝方" };

static const uint32_t k_color[4] = {
    RGB(220, 40, 40),
    RGB(40, 170, 60),
    RGB(230, 190, 30),
    RGB(40, 90, 210)
};

static const uint32_t k_light[4] = {
    RGB(255, 205, 205),
    RGB(205, 245, 205),
    RGB(255, 245, 205),
    RGB(205, 220, 255)
};

typedef struct {
    int      plane[4][4];   /* -1 hangar, 0..50 track, 51..56 home */
    int      turn;
    int      dice;
    int      rolled;
    int      rolling;
    int      roll_frames;
    uint64_t roll_deadline;
    int      legal[4];
    int      legal_count;
    int      game_over;
    int      winner;
    int      new_pressed;
    char     notice[96];
} aero_game_t;

typedef struct {
    aero_game_t game;
    uint32_t    rng;
    int         zh;
} aero_app_t;

static void base_origin(int player, int* bx, int* by) {
    *bx = GRID_X + ((player == 1 || player == 2) ? 9 * CELL_SIZE : 0);
    *by = GRID_Y + ((player == 2 || player == 3) ? 9 * CELL_SIZE : 0);
}

static int actual_index(int player, int pos) {
    return (k_start[player] + pos) % 52;
}

static int plane_cell_key(int player, int pos) {
    if (pos < 0) return -1;
    if (pos <= 50) return actual_index(player, pos);
    return 1000 + player * 10 + (pos - 51);
}

static uint32_t aero_rand(aero_app_t* app) {
    uint32_t x = app->rng;
    if (x == 0) x = 0x1A2B3C4Du;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    app->rng = x;
    return x;
}

static void append_msg(char* msg, size_t cap, const char* text) {
    size_t len = strlen(msg);
    if (len + 1 >= cap) return;
    snprintf(msg + len, cap - len, "%s", text);
}

static void game_new(aero_app_t* app) {
    aero_game_t* g = &app->game;
    memset(g, 0, sizeof(*g));
    for (int p = 0; p < 4; p++) {
        for (int i = 0; i < 4; i++) g->plane[p][i] = -1;
    }
    g->turn = 0;
    g->winner = -1;
    snprintf(g->notice, sizeof(g->notice),
             app->zh ? "红方回合 - 请点击投骰" : "RED's turn - click Roll");
}

static void compute_legal(aero_app_t* app) {
    aero_game_t* g = &app->game;
    g->legal_count = 0;
    for (int i = 0; i < 4; i++) {
        int pos = g->plane[g->turn][i];
        int ok = 0;
        if (pos < 0) {
            ok = (g->dice == 6);
        } else if (pos < 56) {
            ok = (pos + g->dice <= 56);
        }
        g->legal[i] = ok;
        if (ok) g->legal_count++;
    }
}

static int capture_at(aero_app_t* app, int player, int pos) {
    aero_game_t* g = &app->game;
    if (pos < 0 || pos > 50) return 0;

    int idx = actual_index(player, pos);
    int captured = 0;

    for (int p = 0; p < 4; p++) {
        if (p == player) continue;
        for (int i = 0; i < 4; i++) {
            int q = g->plane[p][i];
            if (q >= 0 && q <= 50 && actual_index(p, q) == idx) {
                g->plane[p][i] = -1;
                captured++;
            }
        }
    }
    return captured;
}

static int all_home(const aero_game_t* g, int player) {
    for (int i = 0; i < 4; i++) {
        if (g->plane[player][i] != 56) return 0;
    }
    return 1;
}

static int move_plane(aero_app_t* app, int plane_idx) {
    aero_game_t* g = &app->game;
    if (g->game_over || !g->rolled) return 0;
    if (plane_idx < 0 || plane_idx > 3 || !g->legal[plane_idx]) return 0;

    int player = g->turn;
    int dice = g->dice;
    int pos = g->plane[player][plane_idx];

    char msg[64];
    msg[0] = '\0';
    int captured = 0;
    int jumped = 0;
    int launched = 0;

    if (pos < 0) {
        pos = 0;
        launched = 1;
        captured = capture_at(app, player, pos);
    } else {
        pos += dice;
        if (pos == 4) {
            pos = 8;
            jumped = 1;
        }
        if (pos <= 50) {
            captured = capture_at(app, player, pos);
        }
    }

    if (pos > 56) return 0;
    g->plane[player][plane_idx] = pos;

    if (launched) append_msg(msg, sizeof(msg), app->zh ? "起飞! " : "Launched! ");
    if (jumped) append_msg(msg, sizeof(msg), app->zh ? "跳跃 +4! " : "Jump +4! ");
    if (captured > 0) append_msg(msg, sizeof(msg), app->zh ? "击落敌机! " : "Captured! ");
    if (pos == 56) append_msg(msg, sizeof(msg), app->zh ? "抵达终点! " : "HOME! ");

    if (all_home(g, player)) {
        g->game_over = 1;
        g->winner = player;
        g->rolled = 0;
        g->dice = 0;
        g->legal_count = 0;
        snprintf(g->notice, sizeof(g->notice),
                 app->zh ? "%s获胜!" : "%s wins!",
                 app->zh ? k_name_zh[player] : k_name[player]);
        pxe_beep();
        return 1;
    }

    int extra = (dice == 6);
    if (extra) {
        append_msg(msg, sizeof(msg), app->zh ? "再投一次!" : "Roll again!");
    } else {
        g->turn = (g->turn + 1) % 4;
        if (msg[0] == '\0') {
            snprintf(msg, sizeof(msg),
                     app->zh ? "%s回合" : "%s's turn",
                     app->zh ? k_name_zh[g->turn] : k_name[g->turn]);
        }
    }

    g->rolled = 0;
    g->dice = 0;
    g->legal_count = 0;
    for (int i = 0; i < 4; i++) g->legal[i] = 0;

    if (msg[0] != '\0') {
        snprintf(g->notice, sizeof(g->notice), "%s", msg);
    } else {
        g->notice[0] = '\0';
    }
    return 1;
}

static void finish_roll(aero_app_t* app) {
    aero_game_t* g = &app->game;
    g->rolling = 0;
    g->dice = 1 + (int)(aero_rand(app) % 6u);
    g->rolled = 1;
    compute_legal(app);

    if (g->legal_count == 0) {
        int next = (g->turn + 1) % 4;
        snprintf(g->notice, sizeof(g->notice),
                 app->zh ? "无子可走 - %s回合" : "No moves - %s's turn",
                 app->zh ? k_name_zh[next] : k_name[next]);
        g->turn = next;
        g->rolled = 0;
        g->dice = 0;
    } else if (g->dice == 6) {
        snprintf(g->notice, sizeof(g->notice), "%s",
                 app->zh ? "掷出 6 点 - 可起飞或移动棋子" : "Rolled 6 - launch or move");
    } else {
        snprintf(g->notice, sizeof(g->notice), "%s",
                 app->zh ? "请点击高亮显示的棋子" : "Click a highlighted plane");
    }
}

static void start_roll(aero_app_t* app) {
    aero_game_t* g = &app->game;
    if (g->game_over || g->rolling || g->rolled) return;
    g->rolling = 1;
    g->roll_frames = 8;
    g->dice = 1;
    g->roll_deadline = pxe_get_ticks() + 3;
    g->notice[0] = '\0';
}

static void plane_screen_pos(const aero_app_t* app, int player, int plane_idx,
                             int* out_x, int* out_y, int* out_r) {
    const aero_game_t* g = &app->game;
    int pos = g->plane[player][plane_idx];

    if (pos < 0) {
        int bx, by;
        base_origin(player, &bx, &by);
        static const int spot[4][2] = {
            { 36, 36 }, { 108, 36 }, { 36, 108 }, { 108, 108 }
        };
        *out_x = bx + spot[plane_idx][0];
        *out_y = by + spot[plane_idx][1];
        *out_r = 10;
        return;
    }

    int row;
    int col;
    if (pos <= 50) {
        int idx = actual_index(player, pos);
        row = k_path[idx][0];
        col = k_path[idx][1];
    } else {
        int hi = pos - 51;
        row = k_home[player][hi][0];
        col = k_home[player][hi][1];
    }

    int x = GRID_X + col * CELL_SIZE + CELL_SIZE / 2;
    int y = GRID_Y + row * CELL_SIZE + CELL_SIZE / 2;

    int key = plane_cell_key(player, pos);
    int count = 0;
    int order = 0;
    for (int p = 0; p < 4; p++) {
        for (int i = 0; i < 4; i++) {
            int q = g->plane[p][i];
            if (q < 0) continue;
            if (plane_cell_key(p, q) == key) {
                if (p == player && i == plane_idx) order = count;
                count++;
            }
        }
    }

    int radius = 9;
    if (count > 1) {
        static const int off[4][2] = {
            { -4, -4 }, { 4, -4 }, { -4, 4 }, { 4, 4 }
        };
        radius = 5;
        if (order < 4) {
            x += off[order][0];
            y += off[order][1];
        }
    }

    *out_x = x;
    *out_y = y;
    *out_r = radius;
}

static void draw_text_center(const pxe_win_surface_t* surf, int cx, int y,
                             const char* text, uint32_t color, int small) {
    int tw = small ? pxe_text_width_small(text) : pxe_text_width(text);
    int x = cx - tw / 2;
    if (small) {
        pxe_draw_text_small(surf, x, y, text, color);
    } else {
        pxe_draw_text(surf, x, y, text, color);
    }
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

static void draw_circle_outline(const pxe_win_surface_t* surf, int cx, int cy,
                                int radius, uint32_t color) {
    int outer = radius * radius;
    int inner = (radius - 2) * (radius - 2);
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int d = dx * dx + dy * dy;
            if (d <= outer && d >= inner) {
                pxe_put_pixel(surf, cx + dx, cy + dy, color);
            }
        }
    }
}

static void draw_sunken_box(const pxe_win_surface_t* surf, int x, int y,
                            int w, int h, uint32_t fill) {
    pxe_fill_rect(surf, x, y, w, h, fill);
    pxe_draw_hline(surf, x, y, w, COL_TRACK_EDGE);
    pxe_draw_vline(surf, x, y, h, COL_TRACK_EDGE);
    pxe_draw_hline(surf, x, y + h - 1, w, COL_DICE_FACE);
    pxe_draw_vline(surf, x + w - 1, y, h, COL_DICE_FACE);
}

static void draw_token(const pxe_win_surface_t* surf, int x, int y,
                       int radius, int player, int highlight) {
    draw_disc(surf, x + 1, y + 1, radius, COL_TRACK_EDGE);
    draw_disc(surf, x, y, radius + 1, COL_DICE_EDGE);
    draw_disc(surf, x, y, radius, k_color[player]);

    uint32_t glyph = (player == 2) ? COL_TEXT : COL_DICE_FACE;
    int arm = radius / 2;
    pxe_draw_vline(surf, x, y - arm, arm * 2 + 1, glyph);
    pxe_draw_hline(surf, x - arm, y, arm * 2 + 1, glyph);
    pxe_draw_hline(surf, x - arm / 2, y + arm / 2, arm + 1, glyph);

    if (highlight) {
        draw_circle_outline(surf, x, y, radius + 3, COL_SELECT);
    }
}

static void draw_die(const pxe_win_surface_t* surf, int x, int y, int value) {
    draw_sunken_box(surf, x, y, DICE_W, DICE_H, COL_DICE_FACE);
    int fx = x + 4;
    int fy = y + 3;
    int fs = 32;

    pxe_draw_rect(surf, fx, fy, fs, fs, COL_DICE_EDGE);

    if (value < 1 || value > 6) {
        draw_text_center(surf, fx + fs / 2, fy + 8, "?", COL_DICE_PIP, 0);
        return;
    }

    int p1 = 8;
    int p2 = 16;
    int p3 = 24;
    int pr = 2;

    switch (value) {
        case 1:
            draw_disc(surf, fx + p2, fy + p2, pr, COL_DICE_PIP);
            break;
        case 2:
            draw_disc(surf, fx + p1, fy + p1, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p3, fy + p3, pr, COL_DICE_PIP);
            break;
        case 3:
            draw_disc(surf, fx + p1, fy + p1, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p2, fy + p2, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p3, fy + p3, pr, COL_DICE_PIP);
            break;
        case 4:
            draw_disc(surf, fx + p1, fy + p1, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p3, fy + p1, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p1, fy + p3, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p3, fy + p3, pr, COL_DICE_PIP);
            break;
        case 5:
            draw_disc(surf, fx + p1, fy + p1, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p3, fy + p1, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p2, fy + p2, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p1, fy + p3, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p3, fy + p3, pr, COL_DICE_PIP);
            break;
        case 6:
            draw_disc(surf, fx + p1, fy + p1, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p1, fy + p2, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p1, fy + p3, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p3, fy + p1, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p3, fy + p2, pr, COL_DICE_PIP);
            draw_disc(surf, fx + p3, fy + p3, pr, COL_DICE_PIP);
            break;
        default:
            break;
    }
}

static void draw_board(const pxe_win_surface_t* surf, const aero_app_t* app) {
    const aero_game_t* g = &app->game;

    pxe_fill_rect(surf, GRID_X, GRID_Y, BOARD_PX, BOARD_PX, COL_BOARD);
    pxe_draw_rect(surf, GRID_X, GRID_Y, BOARD_PX, BOARD_PX, COL_TRACK_EDGE);

    static const int spot[4][2] = {
        { 36, 36 }, { 108, 36 }, { 36, 108 }, { 108, 108 }
    };

    for (int p = 0; p < 4; p++) {
        int bx, by;
        base_origin(p, &bx, &by);
        pxe_fill_rect(surf, bx, by, 6 * CELL_SIZE, 6 * CELL_SIZE, k_light[p]);
        pxe_draw_rect(surf, bx, by, 6 * CELL_SIZE, 6 * CELL_SIZE, k_color[p]);
        pxe_draw_rect(surf, bx + 1, by + 1,
                      6 * CELL_SIZE - 2, 6 * CELL_SIZE - 2, k_color[p]);

        for (int i = 0; i < 4; i++) {
            draw_disc(surf, bx + spot[i][0], by + spot[i][1], 10,
                      COL_DICE_FACE);
            draw_circle_outline(surf, bx + spot[i][0], by + spot[i][1], 10,
                                k_color[p]);
        }
    }

    for (int i = 0; i < 52; i++) {
        int x = GRID_X + k_path[i][1] * CELL_SIZE;
        int y = GRID_Y + k_path[i][0] * CELL_SIZE;
        uint32_t fill = COL_TRACK;
        uint32_t edge = COL_TRACK_EDGE;

        for (int p = 0; p < 4; p++) {
            if (i == k_start[p] || i == (k_start[p] + 4) % 52) {
                fill = k_light[p];
                edge = k_color[p];
            }
        }

        pxe_fill_rect(surf, x, y, CELL_SIZE, CELL_SIZE, fill);
        pxe_draw_rect(surf, x, y, CELL_SIZE, CELL_SIZE, edge);

        for (int p = 0; p < 4; p++) {
            if (i == k_start[p]) {
                draw_text_center(surf, x + CELL_SIZE / 2, y + 8,
                                 ">", k_color[p], 1);
            } else if (i == (k_start[p] + 4) % 52) {
                draw_text_center(surf, x + CELL_SIZE / 2, y + 8,
                                 ">>", k_color[p], 1);
            }
        }
    }

    int cx = GRID_X + 7 * CELL_SIZE + CELL_SIZE / 2;
    int cy = GRID_Y + 7 * CELL_SIZE + CELL_SIZE / 2;
    draw_disc(surf, cx, cy, 18, COL_CENTER);
    draw_circle_outline(surf, cx, cy, 18, COL_CENTER_TEXT);
    draw_text_center(surf, cx, cy - 5, app->zh ? "终点" : "HOME", COL_CENTER_TEXT, 1);

    for (int p = 0; p < 4; p++) {
        for (int j = 0; j < 6; j++) {
            int x = GRID_X + k_home[p][j][1] * CELL_SIZE;
            int y = GRID_Y + k_home[p][j][0] * CELL_SIZE;
            pxe_fill_rect(surf, x, y, CELL_SIZE, CELL_SIZE, k_light[p]);
            pxe_draw_rect(surf, x, y, CELL_SIZE, CELL_SIZE, k_color[p]);
        }
    }

    for (int p = 0; p < 4; p++) {
        for (int i = 0; i < 4; i++) {
            int x, y, r;
            plane_screen_pos(app, p, i, &x, &y, &r);
            int highlight = (p == g->turn && g->rolled && g->legal[i]);
            draw_token(surf, x, y, r, p, highlight);
        }
    }
}

static void render_game(const pxe_win_surface_t* surf, const aero_app_t* app) {
    const aero_game_t* g = &app->game;

    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, COL_BG);

    pxe_draw_text(surf, 12, 6, app->zh ? "飞行棋" : "AERO CHESS", COL_TEXT);
    pxe_draw_text_small(surf, 12, 26,
                        app->zh ? "投骰子出发" : "Roll the dice", COL_SUBTITLE);

    draw_sunken_box(surf, TURN_X, TURN_Y, TURN_W, TURN_H, COL_DICE_FACE);

    char turn_text[40];
    int show_player = g->turn;
    const char* pname = app->zh ? k_name_zh[show_player] : k_name[show_player];
    if (g->game_over && g->winner >= 0) {
        show_player = g->winner;
        pname = app->zh ? k_name_zh[g->winner] : k_name[g->winner];
        snprintf(turn_text, sizeof(turn_text),
                 app->zh ? "%s获胜!" : "%s WINS!", pname);
    } else if (g->rolling) {
        snprintf(turn_text, sizeof(turn_text), "%s",
                 app->zh ? "投骰中..." : "ROLLING...");
    } else if (g->rolled) {
        snprintf(turn_text, sizeof(turn_text),
                 app->zh ? "%s掷出 %d" : "%s ROLLED %d",
                 pname, g->dice);
    } else {
        snprintf(turn_text, sizeof(turn_text),
                 app->zh ? "%s回合" : "%s'S TURN", pname);
    }
    draw_text_center(surf, TURN_X + TURN_W / 2, TURN_Y + 9,
                     turn_text, k_color[show_player], 0);

    int can_roll = (!g->game_over && !g->rolling && !g->rolled);
    draw_die(surf, DICE_X, DICE_Y, g->dice);
    if (can_roll) {
        pxe_draw_rect(surf, DICE_X - 1, DICE_Y - 1,
                      DICE_W + 2, DICE_H + 2, COL_SELECT);
    }

    pxe_draw_button(surf, ROLL_X, ROLL_Y, ROLL_W, ROLL_H,
                    app->zh ? "投骰" : "Roll", 0, can_roll ? 1 : 0);
    pxe_draw_button(surf, NEW_BTN_X, NEW_BTN_Y, NEW_BTN_W, NEW_BTN_H,
                    app->zh ? "新游戏" : "New Game", g->new_pressed, 1);

    draw_board(surf, app);

    const char* footer;
    uint32_t footer_color = COL_SUBTITLE;
    if (g->game_over) {
        footer = g->notice[0] ? g->notice
                              : (app->zh ? "游戏结束 - Ctrl+N: 新游戏" : "Game over - Ctrl+N: New Game");
        if (g->winner >= 0) footer_color = k_color[g->winner];
    } else if (g->rolling) {
        footer = app->zh ? "正在投掷骰子..." : "Rolling the dice...";
    } else if (g->rolled) {
        footer = app->zh ? "请点击高亮显示的棋子" : "Click a highlighted plane";
    } else if (g->notice[0]) {
        footer = g->notice;
    } else {
        footer = app->zh ? "点击投骰子再移动高亮棋子    Ctrl+N: 新游戏"
                         : "Click Roll, then click a highlighted plane    Ctrl+N: New Game";
    }
    draw_text_center(surf, WIN_W / 2, FOOTER_Y, footer, footer_color, 1);
}

static int in_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static int plane_at_click(const aero_app_t* app, int mx, int my) {
    const aero_game_t* g = &app->game;
    int best = -1;
    int best_dist = 100000;

    for (int i = 0; i < 4; i++) {
        if (!g->legal[i]) continue;
        int x, y, r;
        plane_screen_pos(app, g->turn, i, &x, &y, &r);
        int dx = mx - x;
        int dy = my - y;
        int reach = r + 4;
        int dist = dx * dx + dy * dy;
        if (dist <= reach * reach && dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

static int handle_mouse_down(aero_app_t* app, const pxe_event_t* ev) {
    if (!(ev->buttons & PXE_BTN_LEFT)) return 0;
    aero_game_t* g = &app->game;

    if (in_rect(ev->x, ev->y, NEW_BTN_X, NEW_BTN_Y, NEW_BTN_W, NEW_BTN_H)) {
        g->new_pressed = 1;
        return 1;
    }

    if (g->game_over || g->rolling) return 0;

    if (g->rolled) {
        int idx = plane_at_click(app, ev->x, ev->y);
        if (idx >= 0) {
            return move_plane(app, idx);
        }
        return 0;
    }

    if (in_rect(ev->x, ev->y, ROLL_X, ROLL_Y, ROLL_W, ROLL_H) ||
        in_rect(ev->x, ev->y, DICE_X, DICE_Y, DICE_W, DICE_H)) {
        start_roll(app);
        return 1;
    }
    return 0;
}

static int handle_mouse_up(aero_app_t* app, const pxe_event_t* ev) {
    if (!app->game.new_pressed) return 0;

    app->game.new_pressed = 0;
    if (in_rect(ev->x, ev->y, NEW_BTN_X, NEW_BTN_Y, NEW_BTN_W, NEW_BTN_H)) {
        game_new(app);
    }
    return 1;
}

static int handle_key(aero_app_t* app, int key, uint32_t modifiers) {
    aero_game_t* g = &app->game;

    if (key == PXE_KEY_ESC) return 2;

    if ((modifiers & PXE_MOD_CTRL) &&
        (key == 'n' || key == 'N' || key == 'r' || key == 'R')) {
        game_new(app);
        return 1;
    }

    if (key == ' ' || key == PXE_KEY_ENTER) {
        if (g->game_over || g->rolling) return 0;
        if (g->rolled) {
            if (g->legal_count == 1) {
                for (int i = 0; i < 4; i++) {
                    if (g->legal[i]) return move_plane(app, i);
                }
            }
            return 0;
        }
        start_roll(app);
        return 1;
    }

    if (g->rolled && key >= '1' && key <= '4') {
        int idx = key - '1';
        if (g->legal[idx]) return move_plane(app, idx);
        return 0;
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
    int init_zh = pxe_is_zh();
    strcpy(desc.title, init_zh ? "飞行棋" : "Aero Chess");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    aero_app_t app;
    memset(&app, 0, sizeof(app));
    app.zh = init_zh;
    uint64_t seed = pxe_get_ticks();
    app.rng = (uint32_t)seed ^ (uint32_t)(seed >> 32) ^
              ((uint32_t)pxe_getpid() * 2654435761u);
    if (app.rng == 0) app.rng = 0x1A2B3C4Du;
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
                case PXE_EV_REDRAW: {
                    int new_zh = pxe_is_zh();
                    if (new_zh != app.zh) {
                        app.zh = new_zh;
                        pxe_win_set_title(win_id, app.zh ? "飞行棋" : "Aero Chess");
                    }
                    dirty = 1;
                    break;
                }
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

        if (app.game.rolling &&
            pxe_get_ticks() >= app.game.roll_deadline) {
            if (app.game.roll_frames > 0) {
                app.game.dice = 1 + (int)(aero_rand(&app) % 6u);
                app.game.roll_frames--;
                app.game.roll_deadline = pxe_get_ticks() + 3;
            } else {
                finish_roll(&app);
            }
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
