/*
 * Minesweeper - Classic mine hunting game for Pixel Neko OS (PXE, Ring 3).
 *
 * Controls:
 *   Left click  - reveal a cell (click an opened number to chord)
 *   Right click - flag / question mark
 *   Arrow keys  - move the keyboard cursor
 *   Space/Enter - reveal at the cursor
 *   F           - flag at the cursor
 *   Ctrl+N / Ctrl+R - start a new game
 *   Ctrl+S      - open / close the settings dialog
 *   Esc         - close the settings dialog / exit
 *
 * Settings and the selected difficulty are persisted to
 * "<system drive>:\MINES.CFG" on an installed, writable system disk.
 */

#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>
#include <pxe/fs.h>

#define WIN_W        520
#define WIN_H        420

#define MAX_COLS     30
#define MAX_ROWS     16
#define MAX_CELLS    (MAX_COLS * MAX_ROWS)

#define BOARD_AREA_X 10
#define BOARD_AREA_Y 58
#define BOARD_AREA_W 500
#define BOARD_AREA_H 330
#define FOOTER_Y     399

#define MINE_BOX_X   180
#define MINE_BOX_Y   8
#define LED_W        68
#define LED_H        38

#define FACE_X       256
#define FACE_Y       8
#define FACE_SIZE    36

#define TIME_BOX_X   300
#define TIME_BOX_Y   8

#define SET_BTN_X    420
#define SET_BTN_Y    12
#define SET_BTN_W    88
#define SET_BTN_H    26

#define DLG_X        80
#define DLG_Y        60
#define DLG_W        360
#define DLG_H        300
#define DLG_TITLE_H  24
#define DLG_ROW_H    24
#define DLG_DIFF_Y(i)   (DLG_Y + 64 + (i) * 26)
#define DLG_TOGGLE_Y(i) (DLG_Y + 148 + (i) * 26)
#define DLG_BTN_W    90
#define DLG_BTN_H    26
#define DLG_APPLY_X  (DLG_X + 58)
#define DLG_APPLY_Y  (DLG_Y + 246)
#define DLG_CANCEL_X (DLG_X + 212)
#define DLG_CANCEL_Y (DLG_Y + 246)

#define CELL_HIDDEN   0
#define CELL_REVEALED 1
#define CELL_FLAGGED  2
#define CELL_QUESTION 3

#define ACT_NONE      0
#define ACT_FACE      1
#define ACT_SETTINGS  2
#define ACT_DIFF_BASE 3
#define ACT_SAFE      6
#define ACT_MARKS     7
#define ACT_SOUND     8
#define ACT_APPLY     9
#define ACT_CANCEL    10

#define RGB(r, g, b) ((uint32_t)(((uint32_t)(r) << 16) | \
                                 ((uint32_t)(g) << 8) | \
                                  (uint32_t)(b)))

#define COL_BG        RGB(192, 192, 192)
#define COL_LIGHT     RGB(255, 255, 255)
#define COL_DARK      RGB(128, 128, 128)
#define COL_DARKER    RGB(64, 64, 64)
#define COL_MUTED     RGB(96, 96, 96)
#define COL_BLACK     PXE_COL_BLACK
#define COL_WHITE     PXE_COL_WHITE
#define COL_LED_RED   RGB(255, 32, 32)
#define COL_FLAG      RGB(220, 20, 20)
#define COL_MINE      PXE_COL_BLACK
#define COL_EXPLODED  RGB(220, 60, 60)
#define COL_WIN       PXE_COL_DARK_GREEN
#define COL_LOSE      RGB(200, 0, 0)
#define COL_TITLEBAR  PXE_COL_DARK_BLUE
#define COL_SELECT    PXE_COL_BLUE
#define COL_CHECK     PXE_COL_DARK_GREEN
#define COL_CURSOR    PXE_COL_YELLOW

typedef struct {
    const char* name;
    int cols;
    int rows;
    int mines;
} diff_def_t;

static const diff_def_t k_diffs[3] = {
    { "Beginner",     9,  9, 10 },
    { "Intermediate", 16, 16, 40 },
    { "Expert",       30, 16, 99 }
};
static const char* const k_diff_names_zh[3] = {
    "初级", "中级", "高级"
};

typedef struct {
    int difficulty;
    int safe_first;
    int question_marks;
    int sound;
} mines_settings_t;

typedef struct {
    int      cols;
    int      rows;
    int      mines;
    uint8_t  mine[MAX_CELLS];
    uint8_t  state[MAX_CELLS];
    uint8_t  adj[MAX_CELLS];
    int      mines_placed;
    int      flags;
    int      revealed;
    int      started;
    int      game_over;
    int      won;
    int      exploded;
    int      cursor_r;
    int      cursor_c;
    int      cursor_visible;
    uint64_t start_ticks;
    int      elapsed_sec;
} mines_game_t;

typedef struct {
    mines_game_t        game;
    mines_settings_t    settings;
    mines_settings_t    draft;
    int                 settings_open;
    int                 press_action;
    int                 paused_for_settings;
    uint64_t            pause_start_ticks;
    uint32_t            rng;
    pxe_basic_sysinfo_t sysinfo;
    int                 sysinfo_ok;
    char                cfg_path[24];
    int                 cfg_ready;
    int                 cfg_writable;
    int                 zh;
} mines_app_t;

static void settings_defaults(mines_settings_t* s) {
    s->difficulty = 0;
    s->safe_first = 1;
    s->question_marks = 1;
    s->sound = 1;
}

static int cfg_value(const char* buf, const char* key, int fallback) {
    size_t klen = strlen(key);
    const char* p = buf;

    while (*p) {
        if (strncmp(p, key, klen) == 0) {
            const char* v = p + klen;
            while (*v == ' ' || *v == '\t' || *v == '=') v++;
            if (*v >= '0' && *v <= '9') return *v - '0';
            return fallback;
        }
        while (*p && *p != '\n' && *p != '\r') p++;
        while (*p == '\n' || *p == '\r') p++;
    }
    return fallback;
}

static void settings_load(mines_app_t* app) {
    if (!app->cfg_ready) return;

    char buf[128];
    int n = pxe_read_file(app->cfg_path, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';

    app->settings.difficulty = cfg_value(buf, "diff=", app->settings.difficulty);
    app->settings.safe_first = cfg_value(buf, "safe=", app->settings.safe_first);
    app->settings.question_marks = cfg_value(buf, "marks=", app->settings.question_marks);
    app->settings.sound = cfg_value(buf, "sound=", app->settings.sound);

    if (app->settings.difficulty < 0 || app->settings.difficulty > 2) {
        app->settings.difficulty = 0;
    }
    app->settings.safe_first = app->settings.safe_first ? 1 : 0;
    app->settings.question_marks = app->settings.question_marks ? 1 : 0;
    app->settings.sound = app->settings.sound ? 1 : 0;
}

static void settings_save(const mines_app_t* app) {
    if (!app->cfg_ready || !app->cfg_writable) return;

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "diff=%d\nsafe=%d\nmarks=%d\nsound=%d\n",
                       app->settings.difficulty,
                       app->settings.safe_first ? 1 : 0,
                       app->settings.question_marks ? 1 : 0,
                       app->settings.sound ? 1 : 0);
    if (len > 0) {
        (void)pxe_write_file(app->cfg_path, buf, (size_t)len);
    }
}

static uint32_t game_rand(mines_app_t* app) {
    uint32_t x = app->rng;
    if (x == 0) x = 0x51A7E5u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    app->rng = x;
    return x;
}

static int game_live_seconds(const mines_game_t* g) {
    if (!g->started) return 0;
    uint64_t now = pxe_get_ticks();
    uint64_t delta = now - g->start_ticks;
    int sec = (int)(delta / 100ULL);
    if (sec < 0) sec = 0;
    if (sec > 999) sec = 999;
    return sec;
}

static void compute_adjacency(mines_game_t* g) {
    int total = g->cols * g->rows;
    for (int idx = 0; idx < total; idx++) {
        int r = idx / g->cols;
        int c = idx % g->cols;
        int n = 0;
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr;
                int nc = c + dc;
                if (nr < 0 || nr >= g->rows || nc < 0 || nc >= g->cols) continue;
                if (g->mine[nr * g->cols + nc]) n++;
            }
        }
        g->adj[idx] = (uint8_t)n;
    }
}

static void place_mines(mines_app_t* app, int safe_idx) {
    mines_game_t* g = &app->game;
    int total = g->cols * g->rows;
    if (g->mines_placed) return;

    for (int i = 0; i < total; i++) g->mine[i] = 0;

    int placed = 0;
    int guard = 0;
    while (placed < g->mines && guard < total * 64) {
        int idx = (int)(game_rand(app) % (uint32_t)total);
        guard++;
        if (g->mine[idx]) continue;
        if (app->settings.safe_first && idx == safe_idx) continue;
        g->mine[idx] = 1;
        placed++;
    }

    if (placed < g->mines) {
        for (int i = 0; i < total && placed < g->mines; i++) {
            if (g->mine[i]) continue;
            if (app->settings.safe_first && i == safe_idx) continue;
            g->mine[i] = 1;
            placed++;
        }
    }

    compute_adjacency(g);
    g->mines_placed = 1;
}

static void game_new(mines_app_t* app) {
    mines_game_t* g = &app->game;
    const diff_def_t* d = &k_diffs[app->settings.difficulty];

    memset(g, 0, sizeof(*g));
    g->cols = d->cols;
    g->rows = d->rows;
    g->mines = d->mines;
    g->exploded = -1;
    g->cursor_r = g->rows / 2;
    g->cursor_c = g->cols / 2;
    g->cursor_visible = 0;
    g->elapsed_sec = 0;
}

static void open_settings(mines_app_t* app) {
    if (app->settings_open) return;
    app->draft = app->settings;
    app->settings_open = 1;
    app->press_action = ACT_NONE;
    app->paused_for_settings = (app->game.started && !app->game.game_over) ? 1 : 0;
    if (app->paused_for_settings) {
        app->pause_start_ticks = pxe_get_ticks();
    }
}

static void close_settings(mines_app_t* app, int apply) {
    if (!app->settings_open) return;

    if (apply) {
        app->settings = app->draft;
        settings_save(app);
        app->paused_for_settings = 0;
        game_new(app);
    } else if (app->paused_for_settings) {
        uint64_t now = pxe_get_ticks();
        app->game.start_ticks += now - app->pause_start_ticks;
        app->paused_for_settings = 0;
    }

    app->settings_open = 0;
    app->press_action = ACT_NONE;
}

static void check_win(mines_app_t* app) {
    mines_game_t* g = &app->game;
    if (g->game_over) return;

    int total = g->cols * g->rows;
    int safe_total = total - g->mines;
    if (g->revealed < safe_total) return;

    g->elapsed_sec = game_live_seconds(g);
    g->won = 1;
    g->game_over = 1;

    for (int i = 0; i < total; i++) {
        if (g->mine[i] && g->state[i] != CELL_FLAGGED) {
            g->state[i] = CELL_FLAGGED;
            g->flags++;
        }
    }

    if (app->settings.sound) pxe_beep();
}

static int reveal_cell(mines_app_t* app, int idx) {
    mines_game_t* g = &app->game;
    int total = g->cols * g->rows;

    if (g->game_over || idx < 0 || idx >= total) return 0;
    if (g->state[idx] == CELL_FLAGGED || g->state[idx] == CELL_REVEALED) return 0;

    if (!g->mines_placed) place_mines(app, idx);

    if (!g->started) {
        g->started = 1;
        g->start_ticks = pxe_get_ticks();
        g->elapsed_sec = 0;
    }

    if (g->mine[idx]) {
        g->elapsed_sec = game_live_seconds(g);
        g->game_over = 1;
        g->won = 0;
        g->exploded = idx;
        g->state[idx] = CELL_REVEALED;
        if (app->settings.sound) pxe_beep();
        return 1;
    }

    int queue[MAX_CELLS];
    int head = 0;
    int tail = 0;

    g->state[idx] = CELL_REVEALED;
    g->revealed++;
    queue[tail++] = idx;

    while (head < tail) {
        int cur = queue[head++];
        if (g->adj[cur] != 0) continue;

        int r = cur / g->cols;
        int c = cur % g->cols;
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr;
                int nc = c + dc;
                if (nr < 0 || nr >= g->rows || nc < 0 || nc >= g->cols) continue;
                int n = nr * g->cols + nc;
                if (g->state[n] != CELL_HIDDEN) continue;
                if (g->mine[n]) continue;
                g->state[n] = CELL_REVEALED;
                g->revealed++;
                queue[tail++] = n;
            }
        }
    }

    check_win(app);
    return 1;
}

static int flag_cell(mines_app_t* app, int idx) {
    mines_game_t* g = &app->game;
    int total = g->cols * g->rows;
    if (g->game_over || idx < 0 || idx >= total) return 0;

    if (g->state[idx] == CELL_HIDDEN) {
        g->state[idx] = CELL_FLAGGED;
        g->flags++;
        return 1;
    }
    if (g->state[idx] == CELL_FLAGGED) {
        g->flags--;
        if (g->flags < 0) g->flags = 0;
        g->state[idx] = app->settings.question_marks ? CELL_QUESTION : CELL_HIDDEN;
        return 1;
    }
    if (g->state[idx] == CELL_QUESTION) {
        g->state[idx] = CELL_HIDDEN;
        return 1;
    }
    return 0;
}

static int chord_cell(mines_app_t* app, int idx) {
    mines_game_t* g = &app->game;
    int total = g->cols * g->rows;

    if (g->game_over || idx < 0 || idx >= total) return 0;
    if (g->state[idx] != CELL_REVEALED || g->adj[idx] <= 0) return 0;

    int r = idx / g->cols;
    int c = idx % g->cols;
    int flags = 0;

    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr;
            int nc = c + dc;
            if (nr < 0 || nr >= g->rows || nc < 0 || nc >= g->cols) continue;
            if (g->state[nr * g->cols + nc] == CELL_FLAGGED) flags++;
        }
    }
    if (flags != g->adj[idx]) return 0;

    int changed = 0;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr;
            int nc = c + dc;
            if (nr < 0 || nr >= g->rows || nc < 0 || nc >= g->cols) continue;
            int n = nr * g->cols + nc;
            if (g->state[n] == CELL_HIDDEN) {
                changed |= reveal_cell(app, n);
                if (g->game_over) return changed;
            }
        }
    }
    return changed;
}

static int click_cell(mines_app_t* app, int idx) {
    mines_game_t* g = &app->game;
    if (g->game_over || idx < 0) return 0;
    if (g->state[idx] == CELL_REVEALED) return chord_cell(app, idx);
    if (g->state[idx] == CELL_FLAGGED) return 0;
    return reveal_cell(app, idx);
}

static void draw_raised_box(const pxe_win_surface_t* surf, int x, int y,
                            int w, int h, uint32_t fill) {
    pxe_fill_rect(surf, x, y, w, h, fill);
    pxe_draw_hline(surf, x, y, w, COL_LIGHT);
    pxe_draw_vline(surf, x, y, h, COL_LIGHT);
    pxe_draw_hline(surf, x, y + h - 1, w, COL_DARK);
    pxe_draw_vline(surf, x + w - 1, y, h, COL_DARK);
}

static void draw_sunken_box(const pxe_win_surface_t* surf, int x, int y,
                            int w, int h, uint32_t fill) {
    pxe_fill_rect(surf, x, y, w, h, fill);
    pxe_draw_hline(surf, x, y, w, COL_DARK);
    pxe_draw_vline(surf, x, y, h, COL_DARK);
    pxe_draw_hline(surf, x, y + h - 1, w, COL_LIGHT);
    pxe_draw_vline(surf, x + w - 1, y, h, COL_LIGHT);
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

static uint32_t number_color(int n) {
    switch (n) {
        case 1: return PXE_COL_BLUE;
        case 2: return PXE_COL_DARK_GREEN;
        case 3: return PXE_COL_RED;
        case 4: return PXE_COL_DARK_BLUE;
        case 5: return PXE_COL_DARK_RED;
        case 6: return PXE_COL_DARK_CYAN;
        case 7: return PXE_COL_BLACK;
        default: return PXE_COL_DARK_GRAY;
    }
}

static void format_counter(int value, char out[4]) {
    if (value < 0) {
        if (value < -99) value = -99;
        value = -value;
        out[0] = '-';
        out[1] = (char)('0' + (value / 10) % 10);
        out[2] = (char)('0' + value % 10);
    } else {
        if (value > 999) value = 999;
        out[0] = (char)('0' + (value / 100) % 10);
        out[1] = (char)('0' + (value / 10) % 10);
        out[2] = (char)('0' + value % 10);
    }
    out[3] = '\0';
}

static void draw_led_box(const pxe_win_surface_t* surf, int x, int y,
                         const char* label, const char* value) {
    pxe_fill_rect(surf, x, y, LED_W, LED_H, COL_BLACK);
    pxe_draw_hline(surf, x, y, LED_W, COL_DARK);
    pxe_draw_vline(surf, x, y, LED_H, COL_DARK);
    pxe_draw_hline(surf, x, y + LED_H - 1, LED_W, COL_LIGHT);
    pxe_draw_vline(surf, x + LED_W - 1, y, LED_H, COL_LIGHT);
    pxe_draw_rect(surf, x + 2, y + 2, LED_W - 4, LED_H - 4, COL_DARKER);

    draw_text_center(surf, x + LED_W / 2, y + 3, label, COL_LIGHT, 1);
    draw_text_center(surf, x + LED_W / 2, y + 17, value, COL_LED_RED, 0);
}

static void draw_face_button(const pxe_win_surface_t* surf,
                             const mines_app_t* app) {
    int pressed = (app->press_action == ACT_FACE);
    int x = FACE_X;
    int y = FACE_Y;
    int cx = x + FACE_SIZE / 2 + (pressed ? 1 : 0);
    int cy = y + FACE_SIZE / 2 + (pressed ? 1 : 0);

    draw_raised_box(surf, x, y, FACE_SIZE, FACE_SIZE, COL_BG);

    draw_disc(surf, cx, cy, 12, PXE_COL_YELLOW);
    draw_circle_outline(surf, cx, cy, 12, COL_BLACK);

    if (app->game.game_over && !app->game.won) {
        for (int i = -2; i <= 2; i++) {
            pxe_put_pixel(surf, cx - 5 + i, cy - 5 + i, COL_BLACK);
            pxe_put_pixel(surf, cx - 5 + i, cy - 5 - i, COL_BLACK);
            pxe_put_pixel(surf, cx + 5 + i, cy - 5 + i, COL_BLACK);
            pxe_put_pixel(surf, cx + 5 + i, cy - 5 - i, COL_BLACK);
        }
        for (int i = -4; i <= 4; i++) {
            int yy = cy + 7 - (i * i) / 16;
            pxe_put_pixel(surf, cx + i, yy, COL_BLACK);
        }
    } else {
        if (app->game.won) {
            pxe_fill_rect(surf, cx - 8, cy - 6, 17, 4, COL_BLACK);
            pxe_fill_rect(surf, cx - 1, cy - 4, 3, 1, COL_BLACK);
        } else {
            pxe_fill_rect(surf, cx - 6, cy - 5, 2, 2, COL_BLACK);
            pxe_fill_rect(surf, cx + 5, cy - 5, 2, 2, COL_BLACK);
        }
        for (int i = -4; i <= 4; i++) {
            int yy = cy + 4 + (i * i) / 16;
            pxe_put_pixel(surf, cx + i, yy, COL_BLACK);
        }
    }
}

static int board_cell_size(const mines_game_t* g) {
    int cw = BOARD_AREA_W / g->cols;
    int ch = BOARD_AREA_H / g->rows;
    int s = (cw < ch) ? cw : ch;
    if (s > 40) s = 40;
    if (s < 12) s = 12;
    return s;
}

static void board_rect(const mines_game_t* g, int* bx, int* by,
                       int* bw, int* bh, int* cell) {
    int s = board_cell_size(g);
    int w = g->cols * s;
    int h = g->rows * s;
    *bx = BOARD_AREA_X + (BOARD_AREA_W - w) / 2;
    *by = BOARD_AREA_Y + (BOARD_AREA_H - h) / 2;
    *bw = w;
    *bh = h;
    *cell = s;
}

static int cell_at(const mines_game_t* g, int mx, int my) {
    int bx, by, bw, bh, s;
    board_rect(g, &bx, &by, &bw, &bh, &s);
    if (mx < bx || my < by || mx >= bx + bw || my >= by + bh) return -1;

    int c = (mx - bx) / s;
    int r = (my - by) / s;
    if (c < 0 || c >= g->cols || r < 0 || r >= g->rows) return -1;
    return r * g->cols + c;
}

static void draw_flag(const pxe_win_surface_t* surf, int x, int y, int size) {
    int cx = x + size / 2;
    int cy = y + size / 2;
    int pole_h = size * 2 / 3;
    if (pole_h < 8) pole_h = 8;

    int top = cy - pole_h / 2;
    int bottom = top + pole_h - 1;
    int pole_x = cx - size / 6;
    if (pole_x < x + 2) pole_x = x + 2;

    pxe_draw_vline(surf, pole_x, top, pole_h, COL_BLACK);
    if (pole_x + 1 < x + size - 1) {
        pxe_draw_vline(surf, pole_x + 1, top, pole_h, COL_BLACK);
    }

    int fw = size / 3;
    if (fw < 4) fw = 4;
    if (pole_x + 2 + fw > x + size - 1) {
        fw = x + size - 1 - (pole_x + 2);
    }
    int fh = pole_h / 2;
    if (fh < 3) fh = 3;
    for (int i = 0; i < fh; i++) {
        int w = fw - (i * fw) / fh;
        if (w < 1) w = 1;
        pxe_draw_hline(surf, pole_x + 2, top + i, w, COL_FLAG);
    }

    int base_w = size / 2;
    if (base_w < 6) base_w = 6;
    pxe_draw_hline(surf, cx - base_w / 2, bottom, base_w, COL_BLACK);
}

static void draw_mine(const pxe_win_surface_t* surf, int x, int y, int size) {
    int cx = x + size / 2;
    int cy = y + size / 2;
    int r = size / 5;
    if (r < 3) r = 3;
    int spike = r * 2;

    pxe_draw_hline(surf, cx - spike, cy, spike * 2 + 1, COL_MINE);
    pxe_draw_vline(surf, cx, cy - spike, spike * 2 + 1, COL_MINE);
    for (int i = -spike; i <= spike; i++) {
        pxe_put_pixel(surf, cx + i, cy + i, COL_MINE);
        pxe_put_pixel(surf, cx + i, cy - i, COL_MINE);
    }

    draw_disc(surf, cx, cy, r, COL_MINE);

    pxe_put_pixel(surf, cx - r / 2, cy - r / 2, COL_LIGHT);
    if (r >= 5) {
        pxe_put_pixel(surf, cx - r / 2 + 1, cy - r / 2, COL_LIGHT);
    }
}

static void draw_wrong_mark(const pxe_win_surface_t* surf, int x, int y, int size) {
    int start = x + 3;
    int end = x + size - 4;
    int steps = end - start;
    for (int i = 0; i <= steps; i++) {
        pxe_put_pixel(surf, start + i, y + 3 + i, COL_LOSE);
        pxe_put_pixel(surf, end - i, y + 3 + i, COL_LOSE);
    }
}

static void draw_open_cell(const pxe_win_surface_t* surf, int x, int y,
                           int size, uint32_t bg) {
    pxe_fill_rect(surf, x, y, size, size, bg);
    pxe_draw_hline(surf, x, y, size, COL_DARK);
    pxe_draw_vline(surf, x, y, size, COL_DARK);
    pxe_draw_hline(surf, x, y + size - 1, size, COL_LIGHT);
    pxe_draw_vline(surf, x + size - 1, y, size, COL_LIGHT);
}

static void draw_hidden_cell(const pxe_win_surface_t* surf, int x, int y, int size) {
    pxe_fill_rect(surf, x, y, size, size, COL_BG);
    pxe_draw_hline(surf, x, y, size, COL_LIGHT);
    pxe_draw_vline(surf, x, y, size, COL_LIGHT);
    pxe_draw_hline(surf, x, y + size - 1, size, COL_DARK);
    pxe_draw_vline(surf, x + size - 1, y, size, COL_DARK);
}

static void draw_cell_number(const pxe_win_surface_t* surf, int x, int y,
                             int size, int n) {
    int tx = x + (size - 8) / 2;
    int ty = y + (size - 16) / 2;
    pxe_draw_char(surf, tx, ty, (char)('0' + n), number_color(n));
}

static void draw_cell(const pxe_win_surface_t* surf, const mines_app_t* app,
                      int idx, int x, int y, int size) {
    const mines_game_t* g = &app->game;
    int state = g->state[idx];
    int r = idx / g->cols;
    int c = idx % g->cols;
    int over = g->game_over;

    if (state == CELL_REVEALED) {
        uint32_t bg = (over && idx == g->exploded) ? COL_EXPLODED : COL_BG;
        draw_open_cell(surf, x, y, size, bg);
        if (over && g->mine[idx]) {
            draw_mine(surf, x, y, size);
        } else if (g->adj[idx] > 0) {
            draw_cell_number(surf, x, y, size, g->adj[idx]);
        }
    } else if (state == CELL_FLAGGED) {
        draw_hidden_cell(surf, x, y, size);
        draw_flag(surf, x, y, size);
        if (over && !g->mine[idx]) {
            draw_wrong_mark(surf, x, y, size);
        }
    } else if (state == CELL_QUESTION) {
        draw_hidden_cell(surf, x, y, size);
        if (over && g->mine[idx]) {
            draw_mine(surf, x, y, size);
        } else {
            int tx = x + (size - 8) / 2;
            int ty = y + (size - 16) / 2;
            pxe_draw_char(surf, tx, ty, '?', PXE_COL_DARK_BLUE);
        }
    } else {
        draw_hidden_cell(surf, x, y, size);
        if (over && g->mine[idx]) {
            draw_mine(surf, x, y, size);
        }
    }

    if (!over && g->cursor_visible &&
        r == g->cursor_r && c == g->cursor_c) {
        pxe_draw_rect(surf, x + 1, y + 1, size - 2, size - 2, COL_CURSOR);
        if (size >= 20) {
            pxe_draw_rect(surf, x + 2, y + 2, size - 4, size - 4, COL_CURSOR);
        }
    }
}

static void draw_board(const pxe_win_surface_t* surf, const mines_app_t* app) {
    const mines_game_t* g = &app->game;
    int bx, by, bw, bh, s;
    board_rect(g, &bx, &by, &bw, &bh, &s);

    pxe_fill_rect(surf, bx - 3, by - 3, bw + 6, bh + 6, COL_BG);
    pxe_draw_hline(surf, bx - 3, by - 3, bw + 6, COL_DARK);
    pxe_draw_vline(surf, bx - 3, by - 3, bh + 6, COL_DARK);
    pxe_draw_hline(surf, bx - 2, by - 2, bw + 4, COL_DARK);
    pxe_draw_vline(surf, bx - 2, by - 2, bh + 4, COL_DARK);
    pxe_draw_hline(surf, bx - 3, by + bh + 2, bw + 6, COL_LIGHT);
    pxe_draw_vline(surf, bx + bw + 2, by - 3, bh + 6, COL_LIGHT);
    pxe_draw_hline(surf, bx - 2, by + bh + 1, bw + 4, COL_LIGHT);
    pxe_draw_vline(surf, bx + bw + 1, by - 2, bh + 4, COL_LIGHT);

    for (int r = 0; r < g->rows; r++) {
        for (int c = 0; c < g->cols; c++) {
            int idx = r * g->cols + c;
            draw_cell(surf, app, idx, bx + c * s, by + r * s, s);
        }
    }
}

static void draw_settings_dialog(const pxe_win_surface_t* surf,
                                 const mines_app_t* app);

static void render_game(const pxe_win_surface_t* surf, const mines_app_t* app) {
    const mines_game_t* g = &app->game;
    const diff_def_t* d = &k_diffs[app->settings.difficulty];

    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, COL_BG);

    pxe_draw_text(surf, 12, 9, app->zh ? "扫雷" : "MINESWEEPER", COL_BLACK);

    char status[64];
    uint32_t status_color = COL_BLACK;
    if (g->game_over && g->won) {
        snprintf(status, sizeof(status),
                 app->zh ? "扫雷成功! 用时 %ds" : "You Win! %ds", g->elapsed_sec);
        status_color = COL_WIN;
    } else if (g->game_over) {
        snprintf(status, sizeof(status), "%s",
                 app->zh ? "踩雷了! Ctrl+N 重试" : "Boom! Ctrl+N to retry");
        status_color = COL_LOSE;
    } else {
        snprintf(status, sizeof(status), "%s %dx%d (%d)",
                 app->zh ? k_diff_names_zh[app->settings.difficulty] : d->name,
                 g->cols, g->rows, g->mines);
    }
    pxe_draw_text_small(surf, 12, 32, status, status_color);

    char mines_txt[4];
    char time_txt[4];
    format_counter(g->mines - g->flags, mines_txt);
    format_counter(g->elapsed_sec, time_txt);
    draw_led_box(surf, MINE_BOX_X, MINE_BOX_Y, app->zh ? "雷数" : "MINES", mines_txt);
    draw_led_box(surf, TIME_BOX_X, TIME_BOX_Y, app->zh ? "时间" : "TIME", time_txt);

    draw_face_button(surf, app);

    pxe_draw_button(surf, SET_BTN_X, SET_BTN_Y, SET_BTN_W, SET_BTN_H,
                    app->zh ? "设置" : "Settings", app->press_action == ACT_SETTINGS, 1);

    draw_board(surf, app);

    draw_text_center(surf, WIN_W / 2, FOOTER_Y,
                     app->zh ? "左键: 翻开  右键: 标旗  Ctrl+N: 新游戏  Ctrl+S: 设置"
                             : "Left: Reveal  Right: Flag  Ctrl+N: New  Ctrl+S: Settings",
                     COL_MUTED, 1);

    if (app->settings_open) {
        draw_settings_dialog(surf, app);
    }
}

static void draw_radio(const pxe_win_surface_t* surf, int cx, int cy, int selected) {
    draw_disc(surf, cx, cy, 6, COL_LIGHT);
    draw_circle_outline(surf, cx, cy, 6, COL_DARKER);
    if (selected) {
        draw_disc(surf, cx, cy, 3, COL_SELECT);
    }
}

static void draw_checkbox(const pxe_win_surface_t* surf, int x, int y, int checked) {
    draw_sunken_box(surf, x, y, 18, 18, COL_LIGHT);
    if (checked) {
        for (int i = 0; i < 4; i++) {
            pxe_put_pixel(surf, x + 4 + i, y + 8 + i, COL_CHECK);
            pxe_put_pixel(surf, x + 5 + i, y + 8 + i, COL_CHECK);
        }
        for (int i = 0; i < 8; i++) {
            pxe_put_pixel(surf, x + 7 + i, y + 11 - i, COL_CHECK);
            pxe_put_pixel(surf, x + 7 + i, y + 12 - i, COL_CHECK);
        }
    }
}

static void draw_settings_dialog(const pxe_win_surface_t* surf,
                                 const mines_app_t* app) {
    pxe_fill_rect(surf, DLG_X + 6, DLG_Y + 6, DLG_W, DLG_H, COL_DARKER);

    pxe_fill_rect(surf, DLG_X, DLG_Y, DLG_W, DLG_H, COL_BG);
    pxe_draw_hline(surf, DLG_X, DLG_Y, DLG_W, COL_LIGHT);
    pxe_draw_vline(surf, DLG_X, DLG_Y, DLG_H, COL_LIGHT);
    pxe_draw_hline(surf, DLG_X, DLG_Y + DLG_H - 1, DLG_W, COL_DARK);
    pxe_draw_vline(surf, DLG_X + DLG_W - 1, DLG_Y, DLG_H, COL_DARK);

    pxe_fill_rect(surf, DLG_X + 3, DLG_Y + 3, DLG_W - 6,
                  DLG_TITLE_H - 3, COL_TITLEBAR);
    draw_text_center(surf, DLG_X + DLG_W / 2, DLG_Y + 6,
                     app->zh ? "游戏设置" : "SETTINGS", COL_WHITE, 0);

    pxe_draw_text(surf, DLG_X + 20, DLG_Y + 38,
                  app->zh ? "难度选择" : "Difficulty", COL_BLACK);

    for (int i = 0; i < 3; i++) {
        int y = DLG_DIFF_Y(i);
        if (app->press_action == ACT_DIFF_BASE + i) {
            draw_sunken_box(surf, DLG_X + 14, y - 2, DLG_W - 28, 22, COL_BG);
        }
        draw_radio(surf, DLG_X + 30, y + 8, app->draft.difficulty == i);
        pxe_draw_text(surf, DLG_X + 46, y,
                      app->zh ? k_diff_names_zh[i] : k_diffs[i].name, COL_BLACK);

        char info[40];
        snprintf(info, sizeof(info),
                 app->zh ? "%d x %d 格, %d 个地雷" : "%d x %d, %d mines",
                 k_diffs[i].cols, k_diffs[i].rows, k_diffs[i].mines);
        pxe_draw_text_small(surf, DLG_X + 160, y + 4, info, COL_MUTED);
    }

    pxe_draw_hline(surf, DLG_X + 16, DLG_Y + 140, DLG_W - 32, COL_DARK);
    pxe_draw_hline(surf, DLG_X + 16, DLG_Y + 141, DLG_W - 32, COL_LIGHT);

    for (int i = 0; i < 3; i++) {
        int y = DLG_TOGGLE_Y(i);
        int action = ACT_SAFE + i;
        int checked = 0;
        const char* label = "";

        if (i == 0) {
            checked = app->draft.safe_first;
            label = app->zh ? "首次点击安全" : "Safe first click";
        } else if (i == 1) {
            checked = app->draft.question_marks;
            label = app->zh ? "允许标问号 (?)" : "Question marks";
        } else {
            checked = app->draft.sound;
            label = app->zh ? "开启音效提示" : "Sound effects";
        }

        if (app->press_action == action) {
            draw_sunken_box(surf, DLG_X + 14, y - 2, DLG_W - 28, 22, COL_BG);
        }
        pxe_draw_text(surf, DLG_X + 20, y, label, COL_BLACK);
        draw_checkbox(surf, DLG_X + DLG_W - 46, y - 1, checked);
    }

    draw_text_center(surf, DLG_X + DLG_W / 2, DLG_Y + 224,
                     app->zh ? "应用设置将开始新一局游戏" : "Applying starts a new game",
                     COL_MUTED, 1);

    pxe_draw_button(surf, DLG_APPLY_X, DLG_APPLY_Y, DLG_BTN_W, DLG_BTN_H,
                    app->zh ? "应用" : "Apply", app->press_action == ACT_APPLY, 1);
    pxe_draw_button(surf, DLG_CANCEL_X, DLG_CANCEL_Y, DLG_BTN_W, DLG_BTN_H,
                    app->zh ? "取消" : "Cancel", app->press_action == ACT_CANCEL, 1);
}

static int settings_hit_test(const mines_app_t* app, int x, int y) {
    if (!app->settings_open) return ACT_NONE;
    if (!pxe_point_in_rect(x, y, DLG_X, DLG_Y, DLG_W, DLG_H)) return ACT_NONE;

    for (int i = 0; i < 3; i++) {
        if (pxe_point_in_rect(x, y, DLG_X + 14, DLG_DIFF_Y(i) - 2,
                              DLG_W - 28, 22)) {
            return ACT_DIFF_BASE + i;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (pxe_point_in_rect(x, y, DLG_X + 14, DLG_TOGGLE_Y(i) - 2,
                              DLG_W - 28, 22)) {
            return ACT_SAFE + i;
        }
    }
    if (pxe_point_in_rect(x, y, DLG_APPLY_X, DLG_APPLY_Y,
                          DLG_BTN_W, DLG_BTN_H)) {
        return ACT_APPLY;
    }
    if (pxe_point_in_rect(x, y, DLG_CANCEL_X, DLG_CANCEL_Y,
                          DLG_BTN_W, DLG_BTN_H)) {
        return ACT_CANCEL;
    }
    return ACT_NONE;
}

static int handle_mouse_down(mines_app_t* app, const pxe_event_t* ev) {
    int mx = ev->x;
    int my = ev->y;

    if (app->settings_open) {
        if (!(ev->buttons & PXE_BTN_LEFT)) return 0;
        int hit = settings_hit_test(app, mx, my);
        if (hit != ACT_NONE) {
            app->press_action = hit;
            return 1;
        }
        return 0;
    }

    if (ev->buttons & PXE_BTN_RIGHT) {
        int idx = cell_at(&app->game, mx, my);
        if (idx >= 0) {
            app->game.cursor_visible = 0;
            return flag_cell(app, idx);
        }
        return 0;
    }

    if (!(ev->buttons & PXE_BTN_LEFT)) return 0;

    app->game.cursor_visible = 0;

    if (pxe_point_in_rect(mx, my, FACE_X, FACE_Y, FACE_SIZE, FACE_SIZE)) {
        app->press_action = ACT_FACE;
        return 1;
    }
    if (pxe_point_in_rect(mx, my, SET_BTN_X, SET_BTN_Y, SET_BTN_W, SET_BTN_H)) {
        app->press_action = ACT_SETTINGS;
        return 1;
    }

    int idx = cell_at(&app->game, mx, my);
    if (idx >= 0) {
        return click_cell(app, idx);
    }
    return 0;
}

static int handle_mouse_up(mines_app_t* app, const pxe_event_t* ev) {
    int action = app->press_action;
    if (action == ACT_NONE) return 0;

    app->press_action = ACT_NONE;
    int mx = ev->x;
    int my = ev->y;

    if (app->settings_open) {
        if (action == ACT_APPLY) {
            if (pxe_point_in_rect(mx, my, DLG_APPLY_X, DLG_APPLY_Y,
                                  DLG_BTN_W, DLG_BTN_H)) {
                close_settings(app, 1);
            }
            return 1;
        }
        if (action == ACT_CANCEL) {
            if (pxe_point_in_rect(mx, my, DLG_CANCEL_X, DLG_CANCEL_Y,
                                  DLG_BTN_W, DLG_BTN_H)) {
                close_settings(app, 0);
            }
            return 1;
        }
        if (action >= ACT_DIFF_BASE && action < ACT_DIFF_BASE + 3) {
            int i = action - ACT_DIFF_BASE;
            if (pxe_point_in_rect(mx, my, DLG_X + 14, DLG_DIFF_Y(i) - 2,
                                  DLG_W - 28, 22)) {
                app->draft.difficulty = i;
            }
            return 1;
        }
        if (action >= ACT_SAFE && action <= ACT_SOUND) {
            int i = action - ACT_SAFE;
            if (pxe_point_in_rect(mx, my, DLG_X + 14, DLG_TOGGLE_Y(i) - 2,
                                  DLG_W - 28, 22)) {
                if (i == 0) {
                    app->draft.safe_first = app->draft.safe_first ? 0 : 1;
                } else if (i == 1) {
                    app->draft.question_marks = app->draft.question_marks ? 0 : 1;
                } else {
                    app->draft.sound = app->draft.sound ? 0 : 1;
                }
            }
            return 1;
        }
        return 1;
    }

    if (action == ACT_FACE) {
        if (pxe_point_in_rect(mx, my, FACE_X, FACE_Y, FACE_SIZE, FACE_SIZE)) {
            game_new(app);
        }
        return 1;
    }
    if (action == ACT_SETTINGS) {
        if (pxe_point_in_rect(mx, my, SET_BTN_X, SET_BTN_Y,
                              SET_BTN_W, SET_BTN_H)) {
            open_settings(app);
        }
        return 1;
    }
    return 1;
}

static void move_cursor(mines_game_t* g, int key) {
    if (key == PXE_KEY_UP && g->cursor_r > 0) {
        g->cursor_r--;
    } else if (key == PXE_KEY_DOWN && g->cursor_r < g->rows - 1) {
        g->cursor_r++;
    } else if (key == PXE_KEY_LEFT && g->cursor_c > 0) {
        g->cursor_c--;
    } else if (key == PXE_KEY_RIGHT && g->cursor_c < g->cols - 1) {
        g->cursor_c++;
    }
    g->cursor_visible = 1;
}

static int handle_key(mines_app_t* app, int key, uint32_t modifiers) {
    mines_game_t* g = &app->game;
    int ctrl = (modifiers & PXE_MOD_CTRL) != 0;

    if (key == PXE_KEY_ESC) {
        if (app->settings_open) {
            close_settings(app, 0);
            return 1;
        }
        return 2;
    }

    if (ctrl && (key == 's' || key == 'S')) {
        if (app->settings_open) {
            close_settings(app, 0);
        } else {
            open_settings(app);
        }
        return 1;
    }

    if (app->settings_open) {
        if (key == PXE_KEY_ENTER) {
            close_settings(app, 1);
            return 1;
        }
        return 0;
    }

    if (ctrl && (key == 'n' || key == 'N' || key == 'r' || key == 'R')) {
        game_new(app);
        return 1;
    }

    if (key == PXE_KEY_UP || key == PXE_KEY_DOWN ||
        key == PXE_KEY_LEFT || key == PXE_KEY_RIGHT) {
        move_cursor(g, key);
        return 1;
    }

    if (key == ' ' || key == PXE_KEY_ENTER) {
        int idx = g->cursor_r * g->cols + g->cursor_c;
        g->cursor_visible = 1;
        if (g->state[idx] == CELL_REVEALED) {
            return chord_cell(app, idx);
        }
        return reveal_cell(app, idx);
    }

    if (key == 'f' || key == 'F') {
        int idx = g->cursor_r * g->cols + g->cursor_c;
        g->cursor_visible = 1;
        return flag_cell(app, idx);
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
    strcpy(desc.title, init_zh ? "扫雷" : "Minesweeper");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    mines_app_t app;
    memset(&app, 0, sizeof(app));
    app.zh = init_zh;
    settings_defaults(&app.settings);

    app.sysinfo_ok = (pxe_sys_get_basic_info(&app.sysinfo) == 0);
    if (!app.sysinfo_ok) {
        memset(&app.sysinfo, 0, sizeof(app.sysinfo));
    }
    if (app.sysinfo.system_drive) {
        snprintf(app.cfg_path, sizeof(app.cfg_path), "%c:\\MINES.CFG",
                 app.sysinfo.system_drive);
        app.cfg_ready = 1;
        app.cfg_writable = app.sysinfo.settings_writable ? 1 : 0;
        settings_load(&app);
    }

    uint64_t seed_t = pxe_get_ticks();
    app.rng = (uint32_t)seed_t ^ (uint32_t)(seed_t >> 32) ^
              ((uint32_t)pxe_getpid() * 2654435761u);
    if (app.rng == 0) app.rng = 0x51A7E5u;

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
                        pxe_win_set_title(win_id, app.zh ? "扫雷" : "Minesweeper");
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

        if (!app.settings_open && app.game.started && !app.game.game_over) {
            int sec = game_live_seconds(&app.game);
            if (sec != app.game.elapsed_sec) {
                app.game.elapsed_sec = sec;
                dirty = 1;
            }
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
