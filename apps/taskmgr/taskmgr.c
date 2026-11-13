#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/mem.h>
#include <pxe/str.h>
#include <pxe/ring0.h>

#define MAX_PROCS     32
#define ROW_H         16
#define HEADER_H      18
#define SB_W          14

#define SORT_NONE     0
#define SORT_PID      1
#define SORT_CPU      2
#define SORT_MEM      3

typedef struct {
    int win_id;
    int self_pid;
    pxe_r0_mem_info_t mem;
    pxe_r0_proc_info_t procs[MAX_PROCS];
    int proc_cpu_pct[MAX_PROCS];
    int total_cpu_pct;
    int proc_count;

    int has_prev;
    int prev_count;
    uint32_t prev_pids[MAX_PROCS];
    uint64_t prev_ticks[MAX_PROCS];

    int sort_mode;
    int paused;
    int selected_idx;
    uint32_t selected_pid;
    int scroll_offset;
    int press_btn;   /* 0=none, 1=refresh, 2=switch, 3=end_task, 4=live/pause */
    int hover_btn;
    uint64_t last_click_ticks;
    int last_click_row;
    char status_msg[48];

    uint8_t* back_pixels;
    size_t back_cap;
} taskmgr_state_t;

static const char* state_short(uint8_t state) {
    switch (state) {
        case PXE_PROC_READY:   return "RDY";
        case PXE_PROC_RUNNING: return "RUN";
        case PXE_PROC_BLOCKED: return "BLK";
        case PXE_PROC_ZOMBIE:  return "ZMB";
        default:               return "???";
    }
}

static const char* sort_label(int sort_mode) {
    switch (sort_mode) {
        case SORT_PID: return "PID";
        case SORT_CPU: return "CPU%";
        case SORT_MEM: return "Mem";
        default:       return "Default";
    }
}

static int is_proc_killable(const taskmgr_state_t* st, const pxe_r0_proc_info_t* p) {
    if (!p) return 0;
    if (p->pid == 0) return 0;
    if (p->pid == (uint32_t)st->self_pid) return 0;
    if (strcmp(p->name, "kwm_compositor") == 0) return 0;
    if (p->state == PXE_PROC_ZOMBIE) return 0;
    return 1;
}

static void get_table_rect(const pxe_win_surface_t* surf,
                           int* tx, int* ty, int* tw, int* th, int* vis_rows) {
    *tx = 8;
    *ty = 64;
    *tw = surf->width - 16;
    *th = surf->height - 112;
    if (*th < 64) *th = 64;
    int body_h = *th - HEADER_H - 2;
    *vis_rows = body_h / ROW_H;
    if (*vis_rows < 1) *vis_rows = 1;
}

static void clamp_scroll(taskmgr_state_t* st, int vis_rows) {
    int max_scroll = st->proc_count - vis_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (st->scroll_offset > max_scroll) st->scroll_offset = max_scroll;
    if (st->scroll_offset < 0) st->scroll_offset = 0;
}

static uint64_t lookup_prev_ticks(const taskmgr_state_t* st, uint32_t pid, int* found) {
    for (int i = 0; i < st->prev_count; i++) {
        if (st->prev_pids[i] == pid) {
            *found = 1;
            return st->prev_ticks[i];
        }
    }
    *found = 0;
    return 0;
}

static void sort_processes(taskmgr_state_t* st) {
    if (st->sort_mode == SORT_NONE || st->proc_count <= 1) return;
    for (int i = 0; i < st->proc_count - 1; i++) {
        for (int j = i + 1; j < st->proc_count; j++) {
            int swap = 0;
            if (st->sort_mode == SORT_PID) {
                if (st->procs[j].pid < st->procs[i].pid) swap = 1;
            } else if (st->sort_mode == SORT_CPU) {
                if (st->proc_cpu_pct[j] > st->proc_cpu_pct[i] ||
                    (st->proc_cpu_pct[j] == st->proc_cpu_pct[i] &&
                     st->procs[j].cpu_ticks_total > st->procs[i].cpu_ticks_total)) {
                    swap = 1;
                }
            } else if (st->sort_mode == SORT_MEM) {
                if (st->procs[j].mem_bytes_used > st->procs[i].mem_bytes_used) {
                    swap = 1;
                }
            }
            if (swap) {
                pxe_r0_proc_info_t tp = st->procs[i];
                int tc = st->proc_cpu_pct[i];
                st->procs[i] = st->procs[j];
                st->proc_cpu_pct[i] = st->proc_cpu_pct[j];
                st->procs[j] = tp;
                st->proc_cpu_pct[j] = tc;
            }
        }
    }
}

static void reselect_pid(taskmgr_state_t* st) {
    int found = -1;
    if (st->proc_count > 0) {
        for (int i = 0; i < st->proc_count; i++) {
            if (st->procs[i].pid == st->selected_pid) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            st->selected_idx = found;
        } else {
            if (st->selected_idx >= st->proc_count) st->selected_idx = st->proc_count - 1;
            if (st->selected_idx < 0) st->selected_idx = 0;
            st->selected_pid = st->procs[st->selected_idx].pid;
        }
    } else {
        st->selected_idx = -1;
        st->selected_pid = 0;
    }
}

static void refresh_data(taskmgr_state_t* st, const pxe_win_surface_t* surf) {
    pxe_r0_get_memory_summary(&st->mem);
    int n = pxe_r0_enum_processes(st->procs, MAX_PROCS);
    if (n < 0) n = 0;
    if (n > MAX_PROCS) n = MAX_PROCS;
    st->proc_count = n;

    uint64_t deltas[MAX_PROCS];
    uint64_t delta_idle = 0;
    uint64_t delta_busy = 0;

    for (int i = 0; i < st->proc_count; i++) {
        int found = 0;
        uint64_t old_t = st->has_prev ? lookup_prev_ticks(st, st->procs[i].pid, &found) : 0;
        uint64_t dt = (found && st->procs[i].cpu_ticks_total >= old_t)
                          ? (st->procs[i].cpu_ticks_total - old_t)
                          : 0;
        deltas[i] = dt;
        if (st->procs[i].pid == 0) {
            delta_idle += dt;
        } else {
            delta_busy += dt;
        }
    }

    uint64_t delta_total = delta_idle + delta_busy;
    if (!st->has_prev) {
        for (int i = 0; i < st->proc_count; i++) st->proc_cpu_pct[i] = 0;
        st->total_cpu_pct = 0;
        for (int i = 0; i < st->proc_count; i++) {
            st->prev_pids[i] = st->procs[i].pid;
            st->prev_ticks[i] = st->procs[i].cpu_ticks_total;
        }
        st->prev_count = st->proc_count;
        st->has_prev = 1;
    } else if (delta_total >= 5ULL) {
        st->total_cpu_pct = (int)((delta_busy * 100ULL) / delta_total);
        if (st->total_cpu_pct > 100) st->total_cpu_pct = 100;
        for (int i = 0; i < st->proc_count; i++) {
            int pct = (int)((deltas[i] * 100ULL) / delta_total);
            if (pct > 100) pct = 100;
            st->proc_cpu_pct[i] = pct;
        }
        for (int i = 0; i < st->proc_count; i++) {
            st->prev_pids[i] = st->procs[i].pid;
            st->prev_ticks[i] = st->procs[i].cpu_ticks_total;
        }
        st->prev_count = st->proc_count;
    } else {
        for (int i = 0; i < st->proc_count; i++) {
            st->proc_cpu_pct[i] = 0;
        }
    }

    sort_processes(st);
    reselect_pid(st);

    int tx, ty, tw, th, vis_rows;
    get_table_rect(surf, &tx, &ty, &tw, &th, &vis_rows);
    clamp_scroll(st, vis_rows);
}

static const pxe_r0_proc_info_t* get_selected_proc(const taskmgr_state_t* st) {
    if (st->selected_idx < 0 || st->selected_idx >= st->proc_count) return NULL;
    return &st->procs[st->selected_idx];
}

static void compute_buttons(const pxe_win_surface_t* surf,
                            int* b1x, int* b1w,
                            int* b2x, int* b2w,
                            int* b3x, int* b3w,
                            int* b4x, int* b4w,
                            int* by, int* bh) {
    (void)surf;
    *by = surf->height - 38;
    *bh = 24;
    *b1x = 8;
    *b1w = 72;
    *b2x = 86;
    *b2w = 84;
    *b3x = 176;
    *b3w = 80;
    *b4x = 262;
    *b4w = 72;
}

static void render_taskmgr_into(const pxe_win_surface_t* surf, const taskmgr_state_t* st) {
    char line[160];
    int tx, ty, tw, th, vis_rows;

    if (!surf || !surf->pixels) return;

    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    /* Top summary box */
    pxe_draw_bevel_box(surf, 8, 8, surf->width - 16, 50, 0, PXE_COL_WHITE);

    snprintf(line, sizeof(line), "Tasks: %d | Drivers: %llu | CPU: %d%% | Sort: %s",
             st->proc_count,
             (unsigned long long)st->mem.pdv_loaded_count,
             st->total_cpu_pct,
             sort_label(st->sort_mode));
    pxe_draw_text(surf, 16, 12, line, PXE_COL_DARK_BLUE);

    uint64_t total_mb = st->mem.total_bytes / (1024ULL * 1024ULL);
    uint64_t free_mb = st->mem.free_bytes / (1024ULL * 1024ULL);
    uint64_t used_mb = (total_mb >= free_mb) ? (total_mb - free_mb) : 0;
    int mem_pct = (st->mem.total_pages > 0)
                      ? (int)((st->mem.used_pages * 100ULL) / st->mem.total_pages)
                      : 0;
    if (mem_pct > 100) mem_pct = 100;

    snprintf(line, sizeof(line), "Mem: %llu / %llu MB (%d%% used, %llu MB free)",
             (unsigned long long)used_mb,
             (unsigned long long)total_mb,
             mem_pct,
             (unsigned long long)free_mb);
    pxe_draw_text_small(surf, 16, 32, line, PXE_COL_BLACK);

    /* Visual CPU & Mem meters on right side of summary box when wide enough */
    if (surf->width >= 490) {
        int gx = surf->width - 134;
        pxe_draw_bevel_box(surf, gx, 30, 114, 14, 1, PXE_COL_BLACK);
        int fill_w = (mem_pct * 110) / 100;
        if (fill_w > 0) {
            uint32_t mcol = (mem_pct > 85) ? PXE_COL_RED
                                           : ((mem_pct > 65) ? PXE_COL_YELLOW : PXE_COL_GREEN);
            pxe_fill_rect(surf, gx + 2, 32, fill_w, 10, mcol);
        }
    }

    /* Process list table */
    get_table_rect(surf, &tx, &ty, &tw, &th, &vis_rows);
    pxe_draw_bevel_box(surf, tx, ty, tw, th, 1, PXE_COL_WHITE);

    int list_w = tw - SB_W - 2;
    int hx = tx + 1;
    int hy = ty + 1;

    pxe_fill_rect(surf, hx, hy, tw - 2, HEADER_H, PXE_COL_LIGHT_GRAY);
    pxe_draw_hline(surf, hx, hy + HEADER_H - 1, tw - 2, PXE_COL_DARK_GRAY);

    pxe_draw_text(surf, hx + 4,   hy + 1, "PID",   (st->sort_mode == SORT_PID) ? PXE_COL_DARK_BLUE : PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 36,  hy + 1, "R",     PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 58,  hy + 1, "St",    PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 88,  hy + 1, "Win",   PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 118, hy + 1, "CPU",   (st->sort_mode == SORT_CPU) ? PXE_COL_DARK_BLUE : PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 158, hy + 1, "Ticks", PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 214, hy + 1, "Mem",   (st->sort_mode == SORT_MEM) ? PXE_COL_DARK_BLUE : PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 272, hy + 1, "Image", PXE_COL_BLACK);

    int max_name_chars = (list_w - 276) / 8;
    if (max_name_chars < 6) max_name_chars = 6;
    if (max_name_chars > 31) max_name_chars = 31;

    for (int row = 0; row < vis_rows; row++) {
        int idx = st->scroll_offset + row;
        if (idx >= st->proc_count) break;
        const pxe_r0_proc_info_t* p = &st->procs[idx];
        int ry = hy + HEADER_H + row * ROW_H;
        int is_sel = (idx == st->selected_idx);

        if (is_sel) {
            pxe_fill_rect(surf, hx, ry, list_w, ROW_H, PXE_COL_DARK_BLUE);
        }
        uint32_t fg = is_sel ? PXE_COL_WHITE
                             : ((p->exec_ring == 0) ? PXE_COL_DARK_RED : PXE_COL_BLACK);

        char numbuf[16];
        snprintf(numbuf, sizeof(numbuf), "%u", (unsigned)p->pid);
        pxe_draw_text(surf, hx + 4, ry, numbuf, fg);

        snprintf(numbuf, sizeof(numbuf), "R%u", (unsigned)p->exec_ring);
        pxe_draw_text(surf, hx + 36, ry, numbuf, fg);

        pxe_draw_text(surf, hx + 58, ry, state_short(p->state), fg);

        snprintf(numbuf, sizeof(numbuf), "%u", (unsigned)p->window_count);
        pxe_draw_text(surf, hx + 88, ry, numbuf, fg);

        snprintf(numbuf, sizeof(numbuf), "%d%%", st->proc_cpu_pct[idx]);
        pxe_draw_text(surf, hx + 118, ry, numbuf, fg);

        uint64_t ticks = p->cpu_ticks_total;
        if (ticks > 999999ULL) ticks = 999999ULL;
        snprintf(numbuf, sizeof(numbuf), "%llu", (unsigned long long)ticks);
        pxe_draw_text(surf, hx + 158, ry, numbuf, fg);

        uint64_t mem_kb = (p->mem_bytes_used + 1023ULL) / 1024ULL;
        if (mem_kb > 999999ULL) mem_kb = 999999ULL;
        snprintf(numbuf, sizeof(numbuf), "%lluK", (unsigned long long)mem_kb);
        pxe_draw_text(surf, hx + 214, ry, numbuf, fg);

        char nshort[32];
        int ni = 0;
        while (p->name[ni] && ni < max_name_chars) {
            nshort[ni] = p->name[ni];
            ni++;
        }
        nshort[ni] = '\0';
        pxe_draw_text(surf, hx + 272, ry, nshort, fg);
    }

    pxe_draw_scrollbar(surf, tx + tw - SB_W - 1, hy + HEADER_H, SB_W,
                       th - HEADER_H - 1, st->scroll_offset, vis_rows, st->proc_count);

    /* Action buttons */
    int b1x, b1w, b2x, b2w, b3x, b3w, b4x, b4w, by, bh;
    compute_buttons(surf, &b1x, &b1w, &b2x, &b2w, &b3x, &b3w, &b4x, &b4w, &by, &bh);

    const pxe_r0_proc_info_t* sel = get_selected_proc(st);
    int can_focus = (sel && sel->window_count > 0);
    int can_kill = is_proc_killable(st, sel);

    pxe_draw_button(surf, b1x, by, b1w, bh, "Refresh",
                    (st->press_btn == 1 && st->hover_btn == 1), 1);
    pxe_draw_button(surf, b2x, by, b2w, bh, "Switch To",
                    (can_focus && st->press_btn == 2 && st->hover_btn == 2), can_focus);
    pxe_draw_button(surf, b3x, by, b3w, bh, "End Task",
                    (can_kill && st->press_btn == 3 && st->hover_btn == 3), can_kill);
    pxe_draw_button(surf, b4x, by, b4w, bh, st->paused ? "Resume" : "Pause",
                    (st->press_btn == 4 && st->hover_btn == 4), 1);

    if (surf->width >= 480 && st->status_msg[0]) {
        pxe_draw_text_small(surf, b4x + b4w + 10, by + 8, st->status_msg, PXE_COL_DARK_BLUE);
    }
}

static void render_taskmgr(taskmgr_state_t* st, const pxe_win_surface_t* surf) {
    if (!surf || !surf->pixels || surf->width <= 0 || surf->height <= 0 || surf->pitch <= 0) {
        return;
    }
    size_t need = (size_t)surf->height * (size_t)surf->pitch;
    if (need > st->back_cap) {
        uint8_t* nb = (uint8_t*)malloc(need);
        if (nb) {
            if (st->back_pixels) free(st->back_pixels);
            st->back_pixels = nb;
            st->back_cap = need;
        }
    }
    if (!st->back_pixels) {
        render_taskmgr_into(surf, st);
        pxe_win_present(st->win_id);
        return;
    }

    pxe_win_surface_t off = *surf;
    off.pixels = st->back_pixels;
    render_taskmgr_into(&off, st);
    size_t row_bytes = (size_t)surf->width;
    for (int y = 0; y < surf->height; y++) {
        memcpy((uint8_t*)surf->pixels + (size_t)y * (size_t)surf->pitch,
               st->back_pixels + (size_t)y * (size_t)surf->pitch,
               row_bytes);
    }
    pxe_win_present(st->win_id);
}

static void perform_focus(taskmgr_state_t* st) {
    const pxe_r0_proc_info_t* sel = get_selected_proc(st);
    if (!sel || sel->window_count == 0) {
        strcpy(st->status_msg, "No window");
        return;
    }
    if (pxe_r0_focus_process_window(sel->pid) == 0) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Switched PID %u", (unsigned)sel->pid);
    } else {
        strcpy(st->status_msg, "Switch failed");
    }
}

static void perform_kill(taskmgr_state_t* st, const pxe_win_surface_t* surf) {
    const pxe_r0_proc_info_t* sel = get_selected_proc(st);
    if (!is_proc_killable(st, sel)) {
        strcpy(st->status_msg, "Protected task");
        pxe_beep();
        return;
    }
    uint32_t target_pid = sel->pid;
    if (pxe_r0_kill_process(target_pid, -1) == 0) {
        snprintf(st->status_msg, sizeof(st->status_msg), "Ended PID %u", (unsigned)target_pid);
        refresh_data(st, surf);
    } else {
        strcpy(st->status_msg, "End failed");
    }
}

static void toggle_pause(taskmgr_state_t* st) {
    st->paused = !st->paused;
    strcpy(st->status_msg, st->paused ? "Paused" : "Live 1Hz");
}

static void set_sort_mode(taskmgr_state_t* st, int mode, const pxe_win_surface_t* surf) {
    int tx, ty, tw, th, vis_rows;
    st->sort_mode = mode;
    sort_processes(st);
    reselect_pid(st);
    get_table_rect(surf, &tx, &ty, &tw, &th, &vis_rows);
    clamp_scroll(st, vis_rows);
    snprintf(st->status_msg, sizeof(st->status_msg), "Sort: %s", sort_label(mode));
}

static int hit_test_button(const pxe_win_surface_t* surf, int mx, int my) {
    int b1x, b1w, b2x, b2w, b3x, b3w, b4x, b4w, by, bh;
    compute_buttons(surf, &b1x, &b1w, &b2x, &b2w, &b3x, &b3w, &b4x, &b4w, &by, &bh);
    if (pxe_point_in_rect(mx, my, b1x, by, b1w, bh)) return 1;
    if (pxe_point_in_rect(mx, my, b2x, by, b2w, bh)) return 2;
    if (pxe_point_in_rect(mx, my, b3x, by, b3w, bh)) return 3;
    if (pxe_point_in_rect(mx, my, b4x, by, b4w, bh)) return 4;
    return 0;
}

static void ensure_selected_visible(taskmgr_state_t* st, int vis_rows) {
    if (st->selected_idx < 0) return;
    if (st->selected_idx < st->scroll_offset) {
        st->scroll_offset = st->selected_idx;
    } else if (st->selected_idx >= st->scroll_offset + vis_rows) {
        st->scroll_offset = st->selected_idx - vis_rows + 1;
    }
    clamp_scroll(st, vis_rows);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    taskmgr_state_t* st = (taskmgr_state_t*)calloc(1, sizeof(taskmgr_state_t));
    if (!st) return 1;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = -1;
    desc.y = -1;
    desc.w = 540;
    desc.h = 340;
    desc.min_w = 420;
    desc.min_h = 240;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_RESIZABLE;
    strcpy(desc.title, "Task Manager");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) {
        free(st);
        return 1;
    }

    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 2;

    strcpy(mb.menus[0].title, "Task");
    mb.menus[0].item_count = 3;
    strcpy(mb.menus[0].items[0].label, "Switch To");
    mb.menus[0].items[0].enabled = 1;
    strcpy(mb.menus[0].items[1].label, "End Task");
    mb.menus[0].items[1].enabled = 1;
    strcpy(mb.menus[0].items[2].label, "Exit");
    mb.menus[0].items[2].enabled = 1;

    strcpy(mb.menus[1].title, "View");
    mb.menus[1].item_count = 6;
    strcpy(mb.menus[1].items[0].label, "Refresh Now");
    mb.menus[1].items[0].enabled = 1;
    strcpy(mb.menus[1].items[1].label, "Pause/Resume");
    mb.menus[1].items[1].enabled = 1;
    strcpy(mb.menus[1].items[2].label, "Sort by Default");
    mb.menus[1].items[2].enabled = 1;
    strcpy(mb.menus[1].items[3].label, "Sort by PID");
    mb.menus[1].items[3].enabled = 1;
    strcpy(mb.menus[1].items[4].label, "Sort by CPU%");
    mb.menus[1].items[4].enabled = 1;
    strcpy(mb.menus[1].items[5].label, "Sort by Memory");
    mb.menus[1].items[5].enabled = 1;

    pxe_win_set_menu(win_id, &mb);

    st->win_id = win_id;
    st->self_pid = pxe_getpid();
    st->selected_idx = 0;
    st->last_click_row = -1;
    strcpy(st->status_msg, "Live 1Hz");
    refresh_data(st, &surf);
    render_taskmgr(st, &surf);

    uint64_t last_refresh = pxe_get_ticks();
    int running = 1;
    int dirty = 0;

    while (running) {
        pxe_event_t ev;
        int rc;
        while ((rc = pxe_win_poll_event(win_id, &ev)) > 0) {
            int tx, ty, tw, th, vis_rows;
            get_table_rect(&surf, &tx, &ty, &tw, &th, &vis_rows);

            if (ev.type == PXE_EV_CLOSE) {
                running = 0;
                break;
            } else if (ev.type == PXE_EV_REDRAW || ev.type == PXE_EV_ACTIVATE) {
                refresh_data(st, &surf);
                dirty = 1;
            } else if (ev.type == PXE_EV_RESIZE) {
                if (pxe_win_get_surface(win_id, &surf) == 0) {
                    get_table_rect(&surf, &tx, &ty, &tw, &th, &vis_rows);
                    clamp_scroll(st, vis_rows);
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_MOUSE_DOWN && (ev.buttons & PXE_BTN_LEFT)) {
                int hit = hit_test_button(&surf, ev.x, ev.y);
                if (hit != 0) {
                    const pxe_r0_proc_info_t* sel = get_selected_proc(st);
                    int ok = (hit == 1 || hit == 4) ||
                             (hit == 2 && sel && sel->window_count > 0) ||
                             (hit == 3 && is_proc_killable(st, sel));
                    if (ok) {
                        st->press_btn = hit;
                        st->hover_btn = hit;
                        dirty = 1;
                    }
                } else if (pxe_point_in_rect(ev.x, ev.y, tx + 1, ty + 1, tw - SB_W - 2, HEADER_H)) {
                    int hx = ev.x - (tx + 1);
                    if (hx < 36) set_sort_mode(st, SORT_PID, &surf);
                    else if (hx >= 114 && hx < 158) set_sort_mode(st, SORT_CPU, &surf);
                    else if (hx >= 210 && hx < 270) set_sort_mode(st, SORT_MEM, &surf);
                    else set_sort_mode(st, SORT_NONE, &surf);
                    dirty = 1;
                } else if (pxe_point_in_rect(ev.x, ev.y, tx + tw - SB_W - 1, ty + HEADER_H + 1,
                                             SB_W, th - HEADER_H - 1)) {
                    int sb_y = ty + HEADER_H + 1;
                    int sb_h = th - HEADER_H - 1;
                    if (ev.y < sb_y + 14) {
                        st->scroll_offset--;
                    } else if (ev.y >= sb_y + sb_h - 14) {
                        st->scroll_offset++;
                    } else if (ev.y < sb_y + sb_h / 2) {
                        st->scroll_offset -= vis_rows;
                    } else {
                        st->scroll_offset += vis_rows;
                    }
                    clamp_scroll(st, vis_rows);
                    dirty = 1;
                } else if (pxe_point_in_rect(ev.x, ev.y, tx + 1, ty + HEADER_H + 1,
                                             tw - SB_W - 2, vis_rows * ROW_H)) {
                    int row = (ev.y - (ty + HEADER_H + 1)) / ROW_H;
                    int idx = st->scroll_offset + row;
                    if (idx >= 0 && idx < st->proc_count) {
                        uint64_t now_t = pxe_get_ticks();
                        if (st->last_click_row == idx && (now_t - st->last_click_ticks) < 60ULL) {
                            st->selected_idx = idx;
                            st->selected_pid = st->procs[idx].pid;
                            perform_focus(st);
                            st->last_click_row = -1;
                        } else {
                            st->selected_idx = idx;
                            st->selected_pid = st->procs[idx].pid;
                            st->last_click_row = idx;
                            st->last_click_ticks = now_t;
                            if (is_proc_killable(st, &st->procs[idx])) {
                                snprintf(st->status_msg, sizeof(st->status_msg),
                                         "PID %u selected", (unsigned)st->procs[idx].pid);
                            } else {
                                strcpy(st->status_msg, "Protected task");
                            }
                        }
                        dirty = 1;
                    }
                }
            } else if (ev.type == PXE_EV_MOUSE_MOVE) {
                if (st->press_btn != 0) {
                    int hover = hit_test_button(&surf, ev.x, ev.y);
                    if (hover != st->hover_btn) {
                        st->hover_btn = hover;
                        dirty = 1;
                    }
                }
            } else if (ev.type == PXE_EV_MOUSE_UP) {
                if (st->press_btn != 0) {
                    int hit = hit_test_button(&surf, ev.x, ev.y);
                    int act = (hit == st->press_btn) ? st->press_btn : 0;
                    st->press_btn = 0;
                    st->hover_btn = 0;
                    if (act == 1) {
                        refresh_data(st, &surf);
                        strcpy(st->status_msg, "Refreshed");
                    } else if (act == 2) {
                        perform_focus(st);
                    } else if (act == 3) {
                        perform_kill(st, &surf);
                    } else if (act == 4) {
                        toggle_pause(st);
                    }
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_MENU) {
                if (ev.arg0 == 0) {
                    if (ev.arg1 == 0) {
                        perform_focus(st);
                        dirty = 1;
                    } else if (ev.arg1 == 1) {
                        perform_kill(st, &surf);
                        dirty = 1;
                    } else if (ev.arg1 == 2) {
                        running = 0;
                    }
                } else if (ev.arg0 == 1) {
                    if (ev.arg1 == 0) {
                        refresh_data(st, &surf);
                        strcpy(st->status_msg, "Refreshed");
                        dirty = 1;
                    } else if (ev.arg1 == 1) {
                        toggle_pause(st);
                        dirty = 1;
                    } else if (ev.arg1 >= 2 && ev.arg1 <= 5) {
                        set_sort_mode(st, ev.arg1 - 2, &surf);
                        dirty = 1;
                    }
                }
            } else if (ev.type == PXE_EV_KEY_DOWN) {
                if (ev.key == PXE_KEY_UP && st->proc_count > 0) {
                    if (st->selected_idx > 0) st->selected_idx--;
                    st->selected_pid = st->procs[st->selected_idx].pid;
                    ensure_selected_visible(st, vis_rows);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_DOWN && st->proc_count > 0) {
                    if (st->selected_idx + 1 < st->proc_count) st->selected_idx++;
                    st->selected_pid = st->procs[st->selected_idx].pid;
                    ensure_selected_visible(st, vis_rows);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_PGUP && st->proc_count > 0) {
                    st->selected_idx -= vis_rows;
                    if (st->selected_idx < 0) st->selected_idx = 0;
                    st->selected_pid = st->procs[st->selected_idx].pid;
                    ensure_selected_visible(st, vis_rows);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_PGDN && st->proc_count > 0) {
                    st->selected_idx += vis_rows;
                    if (st->selected_idx >= st->proc_count) st->selected_idx = st->proc_count - 1;
                    st->selected_pid = st->procs[st->selected_idx].pid;
                    ensure_selected_visible(st, vis_rows);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_HOME && st->proc_count > 0) {
                    st->selected_idx = 0;
                    st->selected_pid = st->procs[0].pid;
                    ensure_selected_visible(st, vis_rows);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_END && st->proc_count > 0) {
                    st->selected_idx = st->proc_count - 1;
                    st->selected_pid = st->procs[st->selected_idx].pid;
                    ensure_selected_visible(st, vis_rows);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_ENTER) {
                    perform_focus(st);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_DELETE) {
                    perform_kill(st, &surf);
                    dirty = 1;
                } else if (ev.key == 'r' || ev.key == 'R') {
                    refresh_data(st, &surf);
                    strcpy(st->status_msg, "Refreshed");
                    dirty = 1;
                } else if (ev.key == 'p' || ev.key == 'P') {
                    toggle_pause(st);
                    dirty = 1;
                } else if (ev.key == 's' || ev.key == 'S') {
                    set_sort_mode(st, (st->sort_mode + 1) % 4, &surf);
                    dirty = 1;
                } else if (ev.key == PXE_KEY_ESC) {
                    running = 0;
                }
            }
        }
        if (rc < 0) break;

        uint64_t now = pxe_get_ticks();
        if (!st->paused && (now - last_refresh >= 100ULL)) {
            last_refresh = now;
            refresh_data(st, &surf);
            dirty = 1;
        }

        if (dirty && running) {
            render_taskmgr(st, &surf);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(30);
        }
    }

    pxe_win_destroy(win_id);
    if (st->back_pixels) free(st->back_pixels);
    free(st);
    return 0;
}
