/*
 * Solitaire (Klondike) for Pixel Neko OS.
 *
 * Controls:
 *   Left click      - select a card / destination, click stock to draw
 *   Ctrl+N / Ctrl+R - start a new game
 *   Mouse           - New Game button
 *   Esc             - close the application
 *
 * Click a face-up card to select it, then click a valid destination.
 * Click the stock to draw one card; when empty, click it again to recycle.
 */

#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>

#define WIN_W        520
#define WIN_H        430

#define CARD_W       48
#define CARD_H       64

#define TOP_Y        44
#define STOCK_X      12
#define WASTE_X      66
#define FOUND_X0     292
#define FOUND_GAP    54

#define TABLEAU_X0   71
#define TABLEAU_GAP  54
#define TABLEAU_Y    116
#define TABLEAU_MAX_BOTTOM 392

#define STATUS_Y     398
#define STATUS_H     30

#define NEW_BTN_X    428
#define NEW_BTN_Y    8
#define NEW_BTN_W    80
#define NEW_BTN_H    28

#define SEL_NONE     0
#define SEL_WASTE    1
#define SEL_FOUND    2
#define SEL_TABLEAU  3

#define RGB(r, g, b) ((uint32_t)(((uint32_t)(r) << 16) | \
                                 ((uint32_t)(g) << 8) | \
                                  (uint32_t)(b)))

#define COL_BG           RGB(0, 128, 0)
#define COL_BG_DARK      RGB(0, 80, 0)
#define COL_TITLE        RGB(255, 255, 255)
#define COL_SUBTITLE     RGB(190, 245, 190)
#define COL_SLOT         RGB(0, 80, 0)
#define COL_SLOT_EDGE    RGB(0, 45, 0)
#define COL_SLOT_LABEL   RGB(120, 200, 120)
#define COL_CARD_FACE    RGB(255, 255, 255)
#define COL_CARD_EDGE    PXE_COL_BLACK
#define COL_CARD_INNER   RGB(180, 180, 180)
#define COL_CARD_BACK    RGB(0, 32, 160)
#define COL_CARD_BACK_LT RGB(90, 130, 210)
#define COL_RED          RGB(200, 0, 0)
#define COL_BLACK        PXE_COL_BLACK
#define COL_SELECT       PXE_COL_YELLOW
#define COL_STATUS_BG    RGB(0, 80, 0)
#define COL_STATUS_LINE  RGB(120, 200, 120)
#define COL_WIN          PXE_COL_YELLOW

typedef struct {
    uint8_t suit;    /* 0=S, 1=H, 2=D, 3=C */
    uint8_t rank;    /* 1=A .. 13=K */
    uint8_t face_up;
} sol_card_t;

typedef struct {
    sol_card_t stock[52];
    int        stock_count;

    sol_card_t waste[52];
    int        waste_count;

    sol_card_t found[4][13];
    int        found_count[4];

    sol_card_t tab[7][52];
    int        tab_count[7];

    int sel_type;
    int sel_pile;
    int sel_index;

    int moves;
    int game_over;
    int new_pressed;
} solitaire_game_t;

typedef struct {
    solitaire_game_t game;
    uint32_t         rng;
} solitaire_app_t;

static int card_is_red(const sol_card_t* c) {
    return c->suit == 1 || c->suit == 2;
}

static const char* rank_name(int rank) {
    static const char* names[] = {
        "", "A", "2", "3", "4", "5", "6", "7",
        "8", "9", "10", "J", "Q", "K"
    };
    if (rank < 1 || rank > 13) return "?";
    return names[rank];
}

static char suit_char(int suit) {
    static const char suits[] = "SHDC";
    if (suit > 3) return '?';
    return suits[suit];
}

static uint32_t sol_rand(solitaire_app_t* app) {
    uint32_t x = app->rng;
    if (x == 0) x = 0x5EED1234u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    app->rng = x;
    return x;
}

static void clear_selection(solitaire_game_t* g) {
    g->sel_type = SEL_NONE;
    g->sel_pile = 0;
    g->sel_index = 0;
}

static int tableau_offset(int count) {
    if (count <= 1) return 0;
    int max_h = TABLEAU_MAX_BOTTOM - TABLEAU_Y;
    int off = (max_h - CARD_H) / (count - 1);
    if (off > 16) off = 16;
    if (off < 4) off = 4;
    return off;
}

static void flip_exposed(solitaire_game_t* g, int pile) {
    if (pile < 0 || pile >= 7) return;
    if (g->tab_count[pile] <= 0) return;
    int top = g->tab_count[pile] - 1;
    if (!g->tab[pile][top].face_up) {
        g->tab[pile][top].face_up = 1;
    }
}

static void game_new(solitaire_app_t* app) {
    solitaire_game_t* g = &app->game;

    sol_card_t deck[52];
    int n = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int rank = 1; rank <= 13; rank++) {
            deck[n].suit = (uint8_t)suit;
            deck[n].rank = (uint8_t)rank;
            deck[n].face_up = 0;
            n++;
        }
    }
    for (int i = 51; i > 0; i--) {
        int j = (int)(sol_rand(app) % (uint32_t)(i + 1));
        sol_card_t tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }

    memset(g, 0, sizeof(*g));

    int d = 0;
    for (int p = 0; p < 7; p++) {
        for (int k = 0; k <= p; k++) {
            g->tab[p][k] = deck[d++];
            g->tab[p][k].face_up = (k == p) ? 1 : 0;
        }
        g->tab_count[p] = p + 1;
    }

    g->stock_count = 52 - d;
    for (int k = 0; k < g->stock_count; k++) {
        g->stock[k] = deck[d + k];
        g->stock[k].face_up = 0;
    }
}

static int draw_stock(solitaire_game_t* g) {
    clear_selection(g);

    if (g->stock_count > 0) {
        sol_card_t c = g->stock[--g->stock_count];
        c.face_up = 1;
        g->waste[g->waste_count++] = c;
        g->moves++;
        return 1;
    }

    if (g->waste_count > 0) {
        while (g->waste_count > 0) {
            sol_card_t c = g->waste[--g->waste_count];
            c.face_up = 0;
            g->stock[g->stock_count++] = c;
        }
        g->moves++;
        return 1;
    }
    return 0;
}

static int can_foundation(const solitaire_game_t* g, const sol_card_t* c, int f) {
    if (f < 0 || f > 3) return 0;
    return c->suit == f && c->rank == g->found_count[f] + 1;
}

static int can_tableau(const solitaire_game_t* g, const sol_card_t* c, int pile) {
    if (pile < 0 || pile > 6) return 0;
    if (g->tab_count[pile] == 0) return c->rank == 13;
    const sol_card_t* top = &g->tab[pile][g->tab_count[pile] - 1];
    if (!top->face_up) return 0;
    return c->rank + 1 == top->rank &&
           card_is_red(c) != card_is_red(top);
}

static int sequence_valid(const solitaire_game_t* g, int pile, int idx) {
    if (pile < 0 || pile > 6) return 0;
    if (idx < 0 || idx >= g->tab_count[pile]) return 0;
    if (!g->tab[pile][idx].face_up) return 0;

    for (int k = idx; k < g->tab_count[pile] - 1; k++) {
        const sol_card_t* a = &g->tab[pile][k];
        const sol_card_t* b = &g->tab[pile][k + 1];
        if (!a->face_up || !b->face_up) return 0;
        if (a->rank != b->rank + 1) return 0;
        if (card_is_red(a) == card_is_red(b)) return 0;
    }
    return 1;
}

static int select_tableau(solitaire_game_t* g, int pile, int idx) {
    if (!sequence_valid(g, pile, idx)) return 0;
    g->sel_type = SEL_TABLEAU;
    g->sel_pile = pile;
    g->sel_index = idx;
    return 1;
}

static void select_waste(solitaire_game_t* g) {
    if (g->waste_count <= 0) {
        clear_selection(g);
        return;
    }
    g->sel_type = SEL_WASTE;
    g->sel_pile = 0;
    g->sel_index = g->waste_count - 1;
}

static void select_foundation(solitaire_game_t* g, int f) {
    if (f < 0 || f > 3 || g->found_count[f] <= 0) {
        clear_selection(g);
        return;
    }
    g->sel_type = SEL_FOUND;
    g->sel_pile = f;
    g->sel_index = g->found_count[f] - 1;
}

static void check_win(solitaire_game_t* g) {
    for (int f = 0; f < 4; f++) {
        if (g->found_count[f] != 13) return;
    }
    g->game_over = 1;
    clear_selection(g);
    pxe_beep();
}

static int move_to_foundation(solitaire_game_t* g, int f) {
    if (g->sel_type == SEL_NONE) return 0;

    sol_card_t card;
    int src_type = g->sel_type;
    int src_pile = g->sel_pile;
    int src_idx = g->sel_index;

    if (src_type == SEL_WASTE) {
        if (g->waste_count <= 0) return 0;
        src_idx = g->waste_count - 1;
        card = g->waste[src_idx];
    } else if (src_type == SEL_FOUND) {
        if (src_pile == f || g->found_count[src_pile] <= 0) return 0;
        src_idx = g->found_count[src_pile] - 1;
        card = g->found[src_pile][src_idx];
    } else if (src_type == SEL_TABLEAU) {
        if (src_pile < 0 || src_pile > 6) return 0;
        if (src_idx < 0 || src_idx != g->tab_count[src_pile] - 1) return 0;
        card = g->tab[src_pile][src_idx];
    } else {
        return 0;
    }

    if (!can_foundation(g, &card, f)) return 0;

    if (src_type == SEL_WASTE) {
        g->waste_count--;
    } else if (src_type == SEL_FOUND) {
        g->found_count[src_pile]--;
    } else {
        g->tab_count[src_pile] = src_idx;
        flip_exposed(g, src_pile);
    }

    card.face_up = 1;
    g->found[f][g->found_count[f]++] = card;
    g->moves++;
    clear_selection(g);
    check_win(g);
    return 1;
}

static int move_to_tableau(solitaire_game_t* g, int dest) {
    if (g->sel_type == SEL_NONE) return 0;

    sol_card_t group[52];
    int group_count = 0;
    int src_type = g->sel_type;
    int src_pile = g->sel_pile;
    int src_idx = g->sel_index;

    if (src_type == SEL_WASTE) {
        if (g->waste_count <= 0) return 0;
        src_idx = g->waste_count - 1;
        group[0] = g->waste[src_idx];
        group_count = 1;
    } else if (src_type == SEL_FOUND) {
        if (g->found_count[src_pile] <= 0) return 0;
        src_idx = g->found_count[src_pile] - 1;
        group[0] = g->found[src_pile][src_idx];
        group_count = 1;
    } else if (src_type == SEL_TABLEAU) {
        if (!sequence_valid(g, src_pile, src_idx)) return 0;
        group_count = g->tab_count[src_pile] - src_idx;
        for (int i = 0; i < group_count; i++) {
            group[i] = g->tab[src_pile][src_idx + i];
        }
    } else {
        return 0;
    }

    if (group_count <= 0) return 0;
    if (src_type == SEL_TABLEAU && dest == src_pile) return 0;
    if (!can_tableau(g, &group[0], dest)) return 0;

    if (src_type == SEL_WASTE) {
        g->waste_count--;
    } else if (src_type == SEL_FOUND) {
        g->found_count[src_pile]--;
    } else {
        g->tab_count[src_pile] = src_idx;
        flip_exposed(g, src_pile);
    }

    for (int i = 0; i < group_count; i++) {
        group[i].face_up = 1;
        g->tab[dest][g->tab_count[dest]++] = group[i];
    }

    g->moves++;
    clear_selection(g);
    return 1;
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

static void draw_empty_slot(const pxe_win_surface_t* surf, int x, int y,
                            const char* label, int large) {
    pxe_fill_rect(surf, x, y, CARD_W, CARD_H, COL_SLOT);
    pxe_draw_rect(surf, x, y, CARD_W, CARD_H, COL_SLOT_EDGE);
    pxe_draw_rect(surf, x + 2, y + 2, CARD_W - 4, CARD_H - 4, COL_SLOT_EDGE);

    if (label && label[0]) {
        if (large) {
            draw_text_center(surf, x + CARD_W / 2, y + 24,
                             label, COL_SLOT_LABEL, 0);
        } else {
            draw_text_center(surf, x + CARD_W / 2, y + 28,
                             label, COL_SLOT_LABEL, 1);
        }
    }
}

static void draw_card(const pxe_win_surface_t* surf, int x, int y,
                      const sol_card_t* c, int face_up, int highlight) {
    if (face_up) {
        pxe_fill_rect(surf, x, y, CARD_W, CARD_H, COL_CARD_FACE);
        pxe_draw_rect(surf, x, y, CARD_W, CARD_H, COL_CARD_EDGE);
        pxe_draw_rect(surf, x + 2, y + 2, CARD_W - 4, CARD_H - 4, COL_CARD_INNER);

        uint32_t color = card_is_red(c) ? COL_RED : COL_BLACK;
        pxe_draw_text(surf, x + 3, y + 2, rank_name(c->rank), color);

        char suit[2];
        suit[0] = suit_char(c->suit);
        suit[1] = '\0';
        pxe_draw_text_small(surf, x + 3, y + 19, suit, color);
        pxe_draw_char(surf, x + 20, y + 38, suit[0], color);
    } else {
        pxe_fill_rect(surf, x, y, CARD_W, CARD_H, COL_CARD_BACK);
        pxe_draw_rect(surf, x, y, CARD_W, CARD_H, COL_CARD_EDGE);
        pxe_draw_rect(surf, x + 2, y + 2, CARD_W - 4, CARD_H - 4,
                      COL_CARD_BACK_LT);

        for (int d = -CARD_H; d < CARD_W; d += 10) {
            for (int yy = 3; yy < CARD_H - 3; yy++) {
                int xx = d + yy;
                if (xx >= 3 && xx < CARD_W - 3) {
                    pxe_put_pixel(surf, x + xx, y + yy, COL_CARD_BACK_LT);
                }
                int xx2 = CARD_W - 1 - xx;
                if (xx2 >= 3 && xx2 < CARD_W - 3) {
                    pxe_put_pixel(surf, x + xx2, y + yy, COL_CARD_BACK_LT);
                }
            }
        }
    }

    if (highlight) {
        pxe_draw_rect(surf, x - 1, y - 1, CARD_W + 2, CARD_H + 2, COL_SELECT);
    }
}

static void draw_top_row(const pxe_win_surface_t* surf,
                         const solitaire_game_t* g) {
    if (g->stock_count > 0) {
        draw_card(surf, STOCK_X, TOP_Y, &g->stock[g->stock_count - 1], 0, 0);
    } else {
        draw_empty_slot(surf, STOCK_X, TOP_Y, "STOCK", 0);
    }

    if (g->waste_count > 0) {
        int sel = (g->sel_type == SEL_WASTE);
        draw_card(surf, WASTE_X, TOP_Y, &g->waste[g->waste_count - 1], 1, sel);
    } else {
        draw_empty_slot(surf, WASTE_X, TOP_Y, NULL, 0);
    }

    for (int f = 0; f < 4; f++) {
        int x = FOUND_X0 + f * FOUND_GAP;
        if (g->found_count[f] > 0) {
            int sel = (g->sel_type == SEL_FOUND && g->sel_pile == f);
            draw_card(surf, x, TOP_Y,
                      &g->found[f][g->found_count[f] - 1], 1, sel);
        } else {
            char label[2];
            label[0] = suit_char(f);
            label[1] = '\0';
            draw_empty_slot(surf, x, TOP_Y, label, 1);
        }
    }
}

static void draw_tableau(const pxe_win_surface_t* surf,
                         const solitaire_game_t* g) {
    for (int p = 0; p < 7; p++) {
        int x = TABLEAU_X0 + p * TABLEAU_GAP;
        if (g->tab_count[p] == 0) {
            draw_empty_slot(surf, x, TABLEAU_Y, "K", 1);
            continue;
        }

        int off = tableau_offset(g->tab_count[p]);
        for (int idx = 0; idx < g->tab_count[p]; idx++) {
            int y = TABLEAU_Y + idx * off;
            int highlight = (g->sel_type == SEL_TABLEAU &&
                             g->sel_pile == p &&
                             idx >= g->sel_index);
            draw_card(surf, x, y, &g->tab[p][idx],
                      g->tab[p][idx].face_up, highlight);
        }
    }
}

static void render_game(const pxe_win_surface_t* surf,
                        const solitaire_game_t* g) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, COL_BG);

    pxe_draw_text(surf, 12, 6, "SOLITAIRE", COL_TITLE);

    char moves[32];
    snprintf(moves, sizeof(moves), "Moves: %d", g->moves);
    pxe_draw_text_small(surf, 12, 26, moves, COL_SUBTITLE);

    pxe_draw_button(surf, NEW_BTN_X, NEW_BTN_Y, NEW_BTN_W, NEW_BTN_H,
                    "New Game", g->new_pressed, 1);

    draw_top_row(surf, g);
    draw_tableau(surf, g);

    pxe_fill_rect(surf, 0, STATUS_Y, WIN_W, STATUS_H, COL_STATUS_BG);
    pxe_draw_hline(surf, 0, STATUS_Y, WIN_W, COL_STATUS_LINE);

    char status[96];
    int small = 1;
    int text_y = STATUS_Y + 10;

    if (g->game_over) {
        snprintf(status, sizeof(status), "You Win! Moves: %d", g->moves);
        small = 0;
        text_y = STATUS_Y + 7;
        draw_text_center(surf, WIN_W / 2, text_y, status, COL_WIN, small);
        return;
    }

    if (g->sel_type != SEL_NONE) {
        const sol_card_t* c = NULL;
        if (g->sel_type == SEL_WASTE && g->waste_count > 0) {
            c = &g->waste[g->waste_count - 1];
        } else if (g->sel_type == SEL_FOUND && g->found_count[g->sel_pile] > 0) {
            c = &g->found[g->sel_pile][g->found_count[g->sel_pile] - 1];
        } else if (g->sel_type == SEL_TABLEAU && g->sel_index >= 0 &&
                   g->sel_index < g->tab_count[g->sel_pile]) {
            c = &g->tab[g->sel_pile][g->sel_index];
        }

        if (c) {
            snprintf(status, sizeof(status), "Selected %s%c - click a destination",
                     rank_name(c->rank), suit_char(c->suit));
        } else {
            snprintf(status, sizeof(status), "Click a destination");
        }
        draw_text_center(surf, WIN_W / 2, text_y, status, COL_TITLE, small);
        return;
    }

    snprintf(status, sizeof(status),
             "Stock: draw   Select card, then destination   Ctrl+N: New Game");
    draw_text_center(surf, WIN_W / 2, text_y, status, COL_SUBTITLE, 1);
}

static int in_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static int tableau_hit(const solitaire_game_t* g, int mx, int my,
                       int* out_pile, int* out_idx) {
    for (int p = 0; p < 7; p++) {
        int x = TABLEAU_X0 + p * TABLEAU_GAP;
        int count = g->tab_count[p];

        if (count == 0) {
            if (in_rect(mx, my, x, TABLEAU_Y, CARD_W, CARD_H)) {
                *out_pile = p;
                *out_idx = -1;
                return 1;
            }
            continue;
        }

        int off = tableau_offset(count);
        for (int idx = count - 1; idx >= 0; idx--) {
            int y = TABLEAU_Y + idx * off;
            if (in_rect(mx, my, x, y, CARD_W, CARD_H)) {
                *out_pile = p;
                *out_idx = idx;
                return 1;
            }
        }
    }
    return 0;
}

static int handle_mouse_down(solitaire_app_t* app, const pxe_event_t* ev) {
    solitaire_game_t* g = &app->game;
    if (!(ev->buttons & PXE_BTN_LEFT)) return 0;

    if (in_rect(ev->x, ev->y, NEW_BTN_X, NEW_BTN_Y, NEW_BTN_W, NEW_BTN_H)) {
        g->new_pressed = 1;
        return 1;
    }

    if (g->game_over) return 0;

    if (in_rect(ev->x, ev->y, STOCK_X, TOP_Y, CARD_W, CARD_H)) {
        return draw_stock(g);
    }

    if (in_rect(ev->x, ev->y, WASTE_X, TOP_Y, CARD_W, CARD_H)) {
        if (g->sel_type != SEL_NONE) {
            clear_selection(g);
        } else {
            select_waste(g);
        }
        return 1;
    }

    for (int f = 0; f < 4; f++) {
        int fx = FOUND_X0 + f * FOUND_GAP;
        if (in_rect(ev->x, ev->y, fx, TOP_Y, CARD_W, CARD_H)) {
            if (g->sel_type != SEL_NONE) {
                return move_to_foundation(g, f);
            }
            select_foundation(g, f);
            return 1;
        }
    }

    int pile = -1;
    int idx = -1;
    if (tableau_hit(g, ev->x, ev->y, &pile, &idx)) {
        if (g->sel_type != SEL_NONE) {
            if (g->sel_type == SEL_TABLEAU &&
                g->sel_pile == pile && g->sel_index == idx) {
                clear_selection(g);
                return 1;
            }
            if (move_to_tableau(g, pile)) return 1;

            if (idx >= 0 && g->tab[pile][idx].face_up &&
                select_tableau(g, pile, idx)) {
                return 1;
            }
            clear_selection(g);
            return 1;
        }

        if (idx >= 0 && g->tab[pile][idx].face_up) {
            select_tableau(g, pile, idx);
        }
        return 1;
    }

    if (g->sel_type != SEL_NONE) {
        clear_selection(g);
        return 1;
    }
    return 0;
}

static int handle_mouse_up(solitaire_app_t* app, const pxe_event_t* ev) {
    if (!app->game.new_pressed) return 0;

    app->game.new_pressed = 0;
    if (in_rect(ev->x, ev->y, NEW_BTN_X, NEW_BTN_Y, NEW_BTN_W, NEW_BTN_H)) {
        game_new(app);
    }
    return 1;
}

static int handle_key(solitaire_app_t* app, int key, uint32_t modifiers) {
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
    strcpy(desc.title, "Solitaire");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    solitaire_app_t app;
    memset(&app, 0, sizeof(app));
    uint64_t seed = pxe_get_ticks();
    app.rng = (uint32_t)seed ^ (uint32_t)(seed >> 32) ^
              ((uint32_t)pxe_getpid() * 2654435761u);
    if (app.rng == 0) app.rng = 0x5EED1234u;
    game_new(&app);

    render_game(&surf, &app.game);
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

        if (dirty && running) {
            render_game(&surf, &app.game);
            pxe_win_present(win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(20);
        }
    }

    pxe_win_destroy(win_id);
    return 0;
}
