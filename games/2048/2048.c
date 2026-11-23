/*
 * 2048 - Classic sliding tile puzzle for Pixel Neko OS (PXE, Ring 3).
 *
 * Controls:
 *   Arrow keys / WASD - slide tiles
 *   Ctrl+N / Ctrl+R   - start a new game
 *   Esc               - close the application
 *   Mouse             - New Game / Keep Going buttons
 *
 * The best score is persisted to "<system drive>:\2048.BEST" when the
 * system is running from a writable installed disk.
 */

#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>
#include <pxe/fs.h>

#define BOARD_N      4
#define TILE_SIZE    62
#define TILE_GAP     6
#define BOARD_PX     (BOARD_N * TILE_SIZE + (BOARD_N + 1) * TILE_GAP)

#define WIN_W        300
#define WIN_H        392

#define BOARD_X      ((WIN_W - BOARD_PX) / 2)
#define BOARD_Y      52

#define SCORE_W      74
#define SCORE_H      38
#define SCORE_Y      10
#define BEST_X       (WIN_W - BOARD_X - SCORE_W)
#define SCORE_X      (BEST_X - SCORE_W - 6)

#define FOOTER_Y     (BOARD_Y + BOARD_PX + 6)
#define NEW_BTN_W    104
#define NEW_BTN_H    26
#define NEW_BTN_X    ((WIN_W - NEW_BTN_W) / 2)
#define NEW_BTN_Y    (FOOTER_Y + 12)

#define KEEP_BTN_W   120
#define KEEP_BTN_H   28
#define KEEP_BTN_X   ((WIN_W - KEEP_BTN_W) / 2)
#define KEEP_BTN_Y   (BOARD_Y + 168)

#define RGB(r, g, b) ((uint32_t)(((uint32_t)(r) << 16) | \
                                 ((uint32_t)(g) << 8) | \
                                  (uint32_t)(b)))

#define COL_PAGE_BG      RGB(250, 248, 239)
#define COL_TITLE        RGB(119, 110, 101)
#define COL_MUTED        RGB(143, 122, 102)
#define COL_BOARD        RGB(187, 173, 160)
#define COL_BOARD_EDGE   RGB(160, 148, 136)
#define COL_EMPTY        RGB(205, 193, 180)
#define COL_SCORE_BG     RGB(187, 173, 160)
#define COL_SCORE_LABEL  RGB(238, 228, 218)
#define COL_LIGHT_TEXT   RGB(249, 246, 242)
#define COL_DARK_TEXT    RGB(119, 110, 101)
#define COL_BTN          RGB(143, 122, 102)
#define COL_BTN_DARK     RGB(125, 105, 87)
#define COL_BTN_LIGHT    RGB(163, 142, 122)
#define COL_OVERLAY      RGB(250, 248, 239)

typedef struct {
    int      board[BOARD_N][BOARD_N];
    int      score;
    int      best;
    int      won;
    int      keep_going;
    int      game_over;
    int      best_dirty;
    int      new_pressed;
    int      keep_pressed;
    int      zh;
    uint32_t rng;
} game2048_t;

static pxe_basic_sysinfo_t g_sysinfo;
static int  g_sysinfo_ok;
static char g_best_path[24];

static uint32_t rgb_shade(uint32_t color, int delta) {
    int r = (int)((color >> 16) & 0xFFu) + delta;
    int g = (int)((color >> 8) & 0xFFu) + delta;
    int b = (int)(color & 0xFFu) + delta;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return RGB(r, g, b);
}

static uint32_t tile_color(int value) {
    switch (value) {
        case 2:    return RGB(238, 228, 218);
        case 4:    return RGB(237, 224, 200);
        case 8:    return RGB(242, 177, 121);
        case 16:   return RGB(245, 149, 99);
        case 32:   return RGB(246, 124, 95);
        case 64:   return RGB(246, 94, 59);
        case 128:  return RGB(237, 207, 114);
        case 256:  return RGB(237, 204, 97);
        case 512:  return RGB(237, 200, 80);
        case 1024: return RGB(237, 197, 63);
        case 2048: return RGB(237, 194, 46);
        default:   return RGB(60, 58, 50);
    }
}

static uint32_t game_rand(game2048_t* g) {
    uint32_t x = g->rng;
    if (x == 0) x = 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return x;
}

static void best_load(game2048_t* g) {
    if (!g_sysinfo_ok || g_sysinfo.system_drive == 0) return;

    snprintf(g_best_path, sizeof(g_best_path), "%c:\\2048.BEST",
             g_sysinfo.system_drive);

    char buf[32];
    int n = pxe_read_file(g_best_path, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';

    int value = 0;
    for (int i = 0; i < n; i++) {
        if (buf[i] < '0' || buf[i] > '9') break;
        value = value * 10 + (buf[i] - '0');
        if (value > 99999999) { value = 99999999; break; }
    }
    if (value > g->best) g->best = value;
}

static void best_save_if_dirty(game2048_t* g) {
    if (!g->best_dirty) return;
    if (!g_sysinfo_ok || !g_sysinfo.settings_writable || !g_best_path[0]) return;

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", g->best);
    if (len > 0 && pxe_write_file(g_best_path, buf, (size_t)len) >= 0) {
        g->best_dirty = 0;
    }
}

static void spawn_tile(game2048_t* g) {
    int rows[BOARD_N * BOARD_N];
    int cols[BOARD_N * BOARD_N];
    int count = 0;

    for (int r = 0; r < BOARD_N; r++) {
        for (int c = 0; c < BOARD_N; c++) {
            if (g->board[r][c] == 0) {
                rows[count] = r;
                cols[count] = c;
                count++;
            }
        }
    }
    if (count == 0) return;

    int pick = (int)(game_rand(g) % (uint32_t)count);
    int value = (game_rand(g) % 10u) == 0u ? 4 : 2;
    g->board[rows[pick]][cols[pick]] = value;
}

static int board_has_moves(const game2048_t* g) {
    for (int r = 0; r < BOARD_N; r++) {
        for (int c = 0; c < BOARD_N; c++) {
            if (g->board[r][c] == 0) return 1;
            if (c + 1 < BOARD_N && g->board[r][c] == g->board[r][c + 1]) return 1;
            if (r + 1 < BOARD_N && g->board[r][c] == g->board[r + 1][c]) return 1;
        }
    }
    return 0;
}

static void game_reset(game2048_t* g) {
    best_save_if_dirty(g);

    memset(g->board, 0, sizeof(g->board));
    g->score = 0;
    g->won = 0;
    g->keep_going = 0;
    g->game_over = 0;
    g->new_pressed = 0;
    g->keep_pressed = 0;

    spawn_tile(g);
    spawn_tile(g);
}

static void game_init(game2048_t* g) {
    memset(g, 0, sizeof(*g));

    uint64_t ticks = pxe_get_ticks();
    uint32_t seed = (uint32_t)ticks ^ (uint32_t)(ticks >> 32) ^
                    ((uint32_t)pxe_getpid() * 2654435761u);
    if (seed == 0) seed = 0x00002048u;
    g->rng = seed;

    best_load(g);
    game_reset(g);
}

static int slide_line(int line[BOARD_N], int* gained) {
    int src[BOARD_N];
    int dst[BOARD_N];
    int n = 0;

    for (int i = 0; i < BOARD_N; i++) {
        if (line[i] != 0) src[n++] = line[i];
    }
    for (int i = 0; i < BOARD_N; i++) dst[i] = 0;

    int out = 0;
    for (int i = 0; i < n; i++) {
        if (i + 1 < n && src[i] == src[i + 1]) {
            int merged = src[i] * 2;
            dst[out++] = merged;
            *gained += merged;
            i++;
        } else {
            dst[out++] = src[i];
        }
    }

    int changed = 0;
    for (int i = 0; i < BOARD_N; i++) {
        if (line[i] != dst[i]) changed = 1;
        line[i] = dst[i];
    }
    return changed;
}

static int move_board(game2048_t* g, int dir) {
    int gained = 0;
    int moved = 0;

    /* dir: 0=up, 1=down, 2=left, 3=right */
    for (int i = 0; i < BOARD_N; i++) {
        int line[BOARD_N];
        for (int j = 0; j < BOARD_N; j++) {
            int r;
            int c;
            if (dir == 0)      { r = j;               c = i; }
            else if (dir == 1) { r = BOARD_N - 1 - j; c = i; }
            else if (dir == 2) { r = i;               c = j; }
            else               { r = i;               c = BOARD_N - 1 - j; }
            line[j] = g->board[r][c];
        }

        if (slide_line(line, &gained)) moved = 1;

        for (int j = 0; j < BOARD_N; j++) {
            int r;
            int c;
            if (dir == 0)      { r = j;               c = i; }
            else if (dir == 1) { r = BOARD_N - 1 - j; c = i; }
            else if (dir == 2) { r = i;               c = j; }
            else               { r = i;               c = BOARD_N - 1 - j; }
            g->board[r][c] = line[j];
        }
    }

    if (!moved) return 0;

    g->score += gained;
    if (g->score > g->best) {
        g->best = g->score;
        g->best_dirty = 1;
    }

    spawn_tile(g);

    if (!g->won) {
        for (int r = 0; r < BOARD_N && !g->won; r++) {
            for (int c = 0; c < BOARD_N; c++) {
                if (g->board[r][c] >= 2048) {
                    g->won = 1;
                    g->keep_going = 0;
                    pxe_beep();
                    break;
                }
            }
        }
    }

    if (!board_has_moves(g)) {
        g->game_over = 1;
        pxe_beep();
        best_save_if_dirty(g);
    }
    return 1;
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

static void draw_score_box(const pxe_win_surface_t* surf, int x, int y,
                           const char* label, int value) {
    pxe_fill_rect(surf, x, y, SCORE_W, SCORE_H, COL_SCORE_BG);
    pxe_draw_hline(surf, x, y, SCORE_W, COL_SCORE_LABEL);
    pxe_draw_vline(surf, x, y, SCORE_H, COL_SCORE_LABEL);
    pxe_draw_hline(surf, x, y + SCORE_H - 1, SCORE_W,
                   rgb_shade(COL_SCORE_BG, -24));
    pxe_draw_vline(surf, x + SCORE_W - 1, y, SCORE_H,
                   rgb_shade(COL_SCORE_BG, -24));

    draw_text_center(surf, x + SCORE_W / 2, y + 4, label,
                     COL_SCORE_LABEL, 1);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    if ((int)strlen(buf) * 8 > SCORE_W - 8) {
        draw_text_center(surf, x + SCORE_W / 2, y + 18, buf,
                         COL_LIGHT_TEXT, 1);
    } else {
        draw_text_center(surf, x + SCORE_W / 2, y + 16, buf,
                         COL_LIGHT_TEXT, 0);
    }
}

static void draw_tile(const pxe_win_surface_t* surf, int x, int y, int value) {
    if (value == 0) {
        pxe_fill_rect(surf, x, y, TILE_SIZE, TILE_SIZE, COL_EMPTY);
        return;
    }

    uint32_t bg = tile_color(value);
    pxe_fill_rect(surf, x, y, TILE_SIZE, TILE_SIZE, bg);
    pxe_draw_hline(surf, x, y, TILE_SIZE, rgb_shade(bg, 26));
    pxe_draw_vline(surf, x, y, TILE_SIZE, rgb_shade(bg, 26));
    pxe_draw_hline(surf, x, y + TILE_SIZE - 1, TILE_SIZE,
                   rgb_shade(bg, -22));
    pxe_draw_vline(surf, x + TILE_SIZE - 1, y, TILE_SIZE,
                   rgb_shade(bg, -22));

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    int len = (int)strlen(buf);
    uint32_t fg = (value <= 4) ? COL_DARK_TEXT : COL_LIGHT_TEXT;

    if (len <= 6) {
        int tw = len * 8;
        pxe_draw_text(surf, x + (TILE_SIZE - tw) / 2,
                      y + (TILE_SIZE - 16) / 2, buf, fg);
    } else {
        int tw = len * 6;
        pxe_draw_text_small(surf, x + (TILE_SIZE - tw) / 2,
                            y + (TILE_SIZE - 7) / 2, buf, fg);
    }
}

static void draw_action_button(const pxe_win_surface_t* surf, int x, int y,
                               int w, int h, const char* label, int pressed) {
    pxe_fill_rect(surf, x, y, w, h, pressed ? COL_BTN_DARK : COL_BTN);
    pxe_draw_hline(surf, x, y, w, pressed ? COL_BTN_DARK : COL_BTN_LIGHT);
    pxe_draw_vline(surf, x, y, h, pressed ? COL_BTN_DARK : COL_BTN_LIGHT);
    pxe_draw_hline(surf, x, y + h - 1, w,
                   pressed ? COL_BTN_LIGHT : COL_BTN_DARK);
    pxe_draw_vline(surf, x + w - 1, y, h,
                   pressed ? COL_BTN_LIGHT : COL_BTN_DARK);

    int tw = pxe_text_width(label);
    int tx = x + (w - tw) / 2 + (pressed ? 1 : 0);
    int ty = y + (h - 16) / 2 + (pressed ? 1 : 0);
    pxe_draw_text(surf, tx, ty, label, COL_LIGHT_TEXT);
}

static void draw_overlay(const pxe_win_surface_t* surf, const game2048_t* g) {
    pxe_fill_rect(surf, BOARD_X, BOARD_Y, BOARD_PX, BOARD_PX, COL_OVERLAY);
    pxe_draw_rect(surf, BOARD_X, BOARD_Y, BOARD_PX, BOARD_PX, COL_BOARD);
    pxe_draw_rect(surf, BOARD_X + 3, BOARD_Y + 3,
                  BOARD_PX - 6, BOARD_PX - 6, COL_BOARD_EDGE);

    if (g->won && !g->keep_going) {
        draw_text_center(surf, WIN_W / 2, BOARD_Y + 82,
                         g->zh ? "恭喜获胜!" : "You Win!", COL_TITLE, 0);
        draw_text_center(surf, WIN_W / 2, BOARD_Y + 112,
                         g->zh ? "成功合成 2048!" : "You reached 2048!", COL_MUTED, 1);
        draw_action_button(surf, KEEP_BTN_X, KEEP_BTN_Y,
                           KEEP_BTN_W, KEEP_BTN_H,
                           g->zh ? "继续挑战" : "Keep Going",
                           g->keep_pressed);
        draw_text_center(surf, WIN_W / 2, BOARD_Y + 208,
                         g->zh ? "点击或按任意键继续" : "Click or press any key", COL_MUTED, 1);
    } else if (g->game_over) {
        char buf[40];
        draw_text_center(surf, WIN_W / 2, BOARD_Y + 88,
                         g->zh ? "游戏结束" : "Game Over", COL_TITLE, 0);
        snprintf(buf, sizeof(buf), g->zh ? "最终得分: %d" : "Score: %d", g->score);
        draw_text_center(surf, WIN_W / 2, BOARD_Y + 120, buf,
                         COL_MUTED, 0);
        draw_text_center(surf, WIN_W / 2, BOARD_Y + 158,
                         g->zh ? "按 Ctrl+N 或点击新游戏" : "Press Ctrl+N or click New Game",
                         COL_MUTED, 1);
        draw_text_center(surf, WIN_W / 2, BOARD_Y + 178,
                         g->zh ? "重新开始挑战" : "to try again", COL_MUTED, 1);
    }
}

static void render_game(const pxe_win_surface_t* surf, const game2048_t* g) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, COL_PAGE_BG);

    pxe_draw_text(surf, BOARD_X, 14, "2048", COL_TITLE);
    pxe_draw_text_small(surf, BOARD_X, 33,
                        g->zh ? "合并方块直到 2048!" : "Join tiles to 2048!", COL_MUTED);

    draw_score_box(surf, SCORE_X, SCORE_Y, g->zh ? "得分" : "SCORE", g->score);
    draw_score_box(surf, BEST_X, SCORE_Y, g->zh ? "最高" : "BEST", g->best);

    pxe_fill_rect(surf, BOARD_X, BOARD_Y, BOARD_PX, BOARD_PX, COL_BOARD);
    for (int r = 0; r < BOARD_N; r++) {
        for (int c = 0; c < BOARD_N; c++) {
            int x = BOARD_X + TILE_GAP + c * (TILE_SIZE + TILE_GAP);
            int y = BOARD_Y + TILE_GAP + r * (TILE_SIZE + TILE_GAP);
            draw_tile(surf, x, y, g->board[r][c]);
        }
    }
    pxe_draw_rect(surf, BOARD_X - 1, BOARD_Y - 1,
                  BOARD_PX + 2, BOARD_PX + 2, COL_BOARD_EDGE);
    pxe_draw_rect(surf, BOARD_X - 2, BOARD_Y - 2,
                  BOARD_PX + 4, BOARD_PX + 4, COL_BOARD);

    draw_text_center(surf, WIN_W / 2, FOOTER_Y,
                     g->zh ? "方向键/WASD: 移动   Ctrl+N: 新游戏"
                           : "Arrows/WASD: Move   Ctrl+N: New Game", COL_MUTED, 1);
    draw_action_button(surf, NEW_BTN_X, NEW_BTN_Y,
                       NEW_BTN_W, NEW_BTN_H,
                       g->zh ? "新游戏" : "New Game", g->new_pressed);

    if ((g->won && !g->keep_going) || g->game_over) {
        draw_overlay(surf, g);
    }
}

static int key_to_dir(int key) {
    if (key == PXE_KEY_UP || key == 'w' || key == 'W') return 0;
    if (key == PXE_KEY_DOWN || key == 's' || key == 'S') return 1;
    if (key == PXE_KEY_LEFT || key == 'a' || key == 'A') return 2;
    if (key == PXE_KEY_RIGHT || key == 'd' || key == 'D') return 3;
    return -1;
}

static int is_new_game_key(int key, uint32_t modifiers) {
    if (!(modifiers & PXE_MOD_CTRL)) return 0;
    return key == 'n' || key == 'N' || key == 'r' || key == 'R';
}

/* Returns 0 = no change, 1 = redraw, 2 = quit. */
static int handle_key(game2048_t* g, int key, uint32_t modifiers) {
    if (key == PXE_KEY_ESC) return 2;

    if (g->won && !g->keep_going) {
        if (is_new_game_key(key, modifiers)) {
            game_reset(g);
        } else {
            g->keep_going = 1;
        }
        return 1;
    }

    if (is_new_game_key(key, modifiers)) {
        game_reset(g);
        return 1;
    }

    if (g->game_over) {
        if (key == PXE_KEY_ENTER || key == ' ') {
            game_reset(g);
            return 1;
        }
        return 0;
    }

    int dir = key_to_dir(key);
    if (dir >= 0) return move_board(g, dir);
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
    strcpy(desc.title, init_zh ? "2048 数字方块" : "2048");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    g_sysinfo_ok = (pxe_sys_get_basic_info(&g_sysinfo) == 0);
    if (!g_sysinfo_ok) memset(&g_sysinfo, 0, sizeof(g_sysinfo));

    game2048_t game;
    game_init(&game);
    game.zh = init_zh;

    render_game(&surf, &game);
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
            case PXE_EV_REDRAW: {
                int new_zh = pxe_is_zh();
                if (new_zh != game.zh) {
                    game.zh = new_zh;
                    pxe_win_set_title(win_id, game.zh ? "2048 数字方块" : "2048");
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
                if (ev.buttons & PXE_BTN_LEFT) {
                    if (game.won && !game.keep_going &&
                        pxe_point_in_rect(ev.x, ev.y, KEEP_BTN_X,
                                          KEEP_BTN_Y, KEEP_BTN_W,
                                          KEEP_BTN_H)) {
                        game.keep_pressed = 1;
                        dirty = 1;
                    } else if (pxe_point_in_rect(ev.x, ev.y, NEW_BTN_X,
                                                 NEW_BTN_Y, NEW_BTN_W,
                                                 NEW_BTN_H)) {
                        game.new_pressed = 1;
                        dirty = 1;
                    }
                }
                break;
            case PXE_EV_MOUSE_UP:
                if (game.keep_pressed) {
                    int inside = pxe_point_in_rect(ev.x, ev.y, KEEP_BTN_X,
                                                   KEEP_BTN_Y, KEEP_BTN_W,
                                                   KEEP_BTN_H);
                    game.keep_pressed = 0;
                    if (inside) game.keep_going = 1;
                    dirty = 1;
                }
                if (game.new_pressed) {
                    int inside = pxe_point_in_rect(ev.x, ev.y, NEW_BTN_X,
                                                   NEW_BTN_Y, NEW_BTN_W,
                                                   NEW_BTN_H);
                    game.new_pressed = 0;
                    if (inside) game_reset(&game);
                    dirty = 1;
                }
                break;
            case PXE_EV_KEY_DOWN: {
                int action = handle_key(&game, ev.key, ev.modifiers);
                if (action == 2) running = 0;
                else if (action == 1) dirty = 1;
                break;
            }
            default:
                break;
        }

        if (dirty && running) {
            render_game(&surf, &game);
            pxe_win_present(win_id);
        }
    }

    best_save_if_dirty(&game);
    pxe_win_destroy(win_id);
    return 0;
}
