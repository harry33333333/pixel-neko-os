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
    int warn_modal;
    uint32_t warn_target_pid;
    char warn_target_name[32];
    int new_task_modal;
    char new_task_input[160];
    int new_task_len;
    uint64_t last_click_ticks;
    int last_click_row;
    char status_msg[48];
    int zh;

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

static const char* sort_label(int sort_mode, int zh) {
    switch (sort_mode) {
        case SORT_PID: return "PID";
        case SORT_CPU: return "CPU%";
        case SORT_MEM: return zh ? "内存" : "Mem";
        default:       return zh ? "默认" : "Default";
    }
}

static int is_proc_killable(const taskmgr_state_t* st, const pxe_r0_proc_info_t* p) {
    (void)st;
    if (!p) return 0;
    if (p->state == PXE_PROC_ZOMBIE) return 0;
    return 1;
}

static int is_critical_system_proc(const taskmgr_state_t* st, const pxe_r0_proc_info_t* p) {
    if (!p) return 0;
    if (p->pid == 0 || p->pid == 1) return 1;
    if (st && p->pid == (uint32_t)st->self_pid) return 0;
    if (strcmp(p->name, "idle") == 0 || strcmp(p->name, "kwm_compositor") == 0) return 1;
    return 0;
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
                            int* b5x, int* b5w,
                            int* by, int* bh) {
    (void)surf;
    *by = surf->height - 38;
    *bh = 24;
    *b1x = 8;   *b1w = 56;
    *b2x = 68;  *b2w = 72;
    *b3x = 144; *b3w = 74;
    *b4x = 222; *b4w = 56;
    *b5x = 282; *b5w = 90;
}

static void render_taskmgr_into(const pxe_win_surface_t* surf, const taskmgr_state_t* st) {
    char line[160];
    int tx, ty, tw, th, vis_rows;

    if (!surf || !surf->pixels) return;

    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    /* Top summary box */
    pxe_draw_bevel_box(surf, 8, 8, surf->width - 16, 50, 0, PXE_COL_WHITE);

    snprintf(line, sizeof(line),
             st->zh ? "任务: %d | 驱动: %llu | CPU: %d%% | 排序: %s"
                    : "Tasks: %d | Drivers: %llu | CPU: %d%% | Sort: %s",
             st->proc_count,
             (unsigned long long)st->mem.pdv_loaded_count,
             st->total_cpu_pct,
             sort_label(st->sort_mode, st->zh));
    pxe_draw_text(surf, 16, 12, line, PXE_COL_DARK_BLUE);

    uint64_t total_mb = st->mem.total_bytes / (1024ULL * 1024ULL);
    uint64_t free_mb = st->mem.free_bytes / (1024ULL * 1024ULL);
    uint64_t used_mb = (total_mb >= free_mb) ? (total_mb - free_mb) : 0;
    int mem_pct = (st->mem.total_pages > 0)
                      ? (int)((st->mem.used_pages * 100ULL) / st->mem.total_pages)
                      : 0;
    if (mem_pct > 100) mem_pct = 100;

    snprintf(line, sizeof(line),
             st->zh ? "内存: %llu / %llu MB (已用 %d%%, 空闲 %llu MB)"
                    : "Mem: %llu / %llu MB (%d%% used, %llu MB free)",
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
    pxe_draw_text(surf, hx + 36,  hy + 1, st->zh ? "环" : "R",     PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 58,  hy + 1, st->zh ? "状态" : "St",    PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 88,  hy + 1, st->zh ? "窗口" : "Win",   PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 118, hy + 1, "CPU",   (st->sort_mode == SORT_CPU) ? PXE_COL_DARK_BLUE : PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 158, hy + 1, st->zh ? "时间片" : "Ticks", PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 214, hy + 1, st->zh ? "内存" : "Mem",   (st->sort_mode == SORT_MEM) ? PXE_COL_DARK_BLUE : PXE_COL_BLACK);
    pxe_draw_text(surf, hx + 272, hy + 1, st->zh ? "映像名" : "Image", PXE_COL_BLACK);

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
    int b1x, b1w, b2x, b2w, b3x, b3w, b4x, b4w, b5x, b5w, by, bh;
    compute_buttons(surf, &b1x, &b1w, &b2x, &b2w, &b3x, &b3w, &b4x, &b4w, &b5x, &b5w, &by, &bh);

    const pxe_r0_proc_info_t* sel = get_selected_proc(st);
    int can_focus = (sel && sel->window_count > 0);
    int can_kill = is_proc_killable(st, sel);

    pxe_draw_button(surf, b1x, by, b1w, bh, st->zh ? "刷新" : "Refresh",
                    (st->press_btn == 1 && st->hover_btn == 1), 1);
    pxe_draw_button(surf, b2x, by, b2w, bh, st->zh ? "切换至" : "Switch To",
                    (can_focus && st->press_btn == 2 && st->hover_btn == 2), can_focus);
    pxe_draw_button(surf, b3x, by, b3w, bh, st->zh ? "结束任务" : "End Task",
                    (can_kill && st->press_btn == 3 && st->hover_btn == 3), can_kill);
    pxe_draw_button(surf, b4x, by, b4w, bh,
                    st->paused ? (st->zh ? "继续" : "Resume") : (st->zh ? "暂停" : "Pause"),
                    (st->press_btn == 4 && st->hover_btn == 4), 1);
    pxe_draw_button(surf, b5x, by, b5w, bh, st->zh ? "新任务..." : "New Task...",
                    (st->press_btn == 5 && st->hover_btn == 5), 1);

    if (surf->width >= 500 && st->status_msg[0]) {
        pxe_draw_text_small(surf, b5x + b5w + 8, by + 8, st->status_msg, PXE_COL_DARK_BLUE);
    }

    if (st->new_task_modal) {
        int dw = 368, dh = 136;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        if (dx < 4) dx = 4;
        if (dy < 4) dy = 4;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3,
                      st->zh ? "创建新任务" : "Create New Task", PXE_COL_WHITE);

        pxe_draw_text_small(surf, dx + 14, dy + 28,
                            st->zh ? "输入程序名或路径 (如 cmd, files, notepad):"
                                   : "Type program name or path (e.g. cmd, files):",
                            PXE_COL_BLACK);
        pxe_draw_text_small(surf, dx + 14, dy + 42,
                            st->zh ? "提示: 输入 file/files 可恢复桌面或打开文件管理器"
                                   : "Tip: file/files restores Desktop Shell or opens Files",
                            PXE_COL_DARK_GRAY);

        int bx = dx + 14, by_in = dy + 58, bw = dw - 28, bh_in = 24;
        pxe_draw_bevel_box(surf, bx, by_in, bw, bh_in, 1, PXE_COL_WHITE);
        pxe_draw_text(surf, bx + 6, by_in + 4, st->new_task_input, PXE_COL_BLACK);
        int cx = bx + 6 + pxe_text_width(st->new_task_input);
        if (cx < bx + bw - 4) {
            pxe_fill_rect(surf, cx, by_in + 4, 2, 16, PXE_COL_DARK_BLUE);
        }

        pxe_draw_button(surf, dx + 82,  dy + 98, 92, 24, st->zh ? "确定" : "OK", 0, 1);
        pxe_draw_button(surf, dx + 194, dy + 98, 92, 24, st->zh ? "取消" : "Cancel", 0, 1);
    }

    if (st->warn_modal) {
        int dw = 360, dh = 132;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        if (dx < 4) dx = 4;
        if (dy < 4) dy = 4;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_RED);
        pxe_draw_text(surf, dx + 8, dy + 3,
                      st->zh ? "任务管理器警告" : "Task Manager Warning", PXE_COL_WHITE);

        /* Yellow warning triangle */
        for (int r = 0; r < 20; r++) {
            int half = r / 2;
            for (int c = -half; c <= half; c++) {
                uint8_t col = (r == 19 || c == -half || c == half) ? PXE_COL_BLACK : PXE_COL_YELLOW;
                pxe_put_pixel(surf, dx + 24 + c, dy + 30 + r, col);
            }
        }
        pxe_fill_rect(surf, dx + 24, dy + 36, 2, 7, PXE_COL_BLACK);
        pxe_fill_rect(surf, dx + 24, dy + 45, 2, 2, PXE_COL_BLACK);

        char tline[64];
        snprintf(tline, sizeof(tline),
                 st->zh ? "确定结束 '%s' (PID %u)?" : "Terminate '%s' (PID %u)?",
                 st->warn_target_name[0] ? st->warn_target_name : (st->zh ? "系统进程" : "System Process"),
                 (unsigned)st->warn_target_pid);
        pxe_draw_text_small(surf, dx + 44, dy + 28, tline, PXE_COL_DARK_RED);
        pxe_draw_text_small(surf, dx + 44, dy + 44,
                            st->zh ? "警告: 强行结束系统进程可能导致"
                                   : "WARNING: Terminating a system process", PXE_COL_BLACK);
        pxe_draw_text_small(surf, dx + 44, dy + 58,
                            st->zh ? "未保存的数据丢失、界面卡死或系统"
                                   : "causes undesired results including loss of", PXE_COL_BLACK);
        pxe_draw_text_small(surf, dx + 44, dy + 72,
                            st->zh ? "不稳定等不可预期的后果。"
                                   : "data and system instability or freeze.", PXE_COL_BLACK);
        pxe_draw_text_small(surf, dx + 44, dy + 86,
                            st->zh ? "确定要强行结束该进程吗?"
                                   : "Are you sure you want to terminate it?", PXE_COL_DARK_BLUE);

        pxe_draw_button(surf, dx + 74,  dy + 102, 92, 22, st->zh ? "是 (结束)" : "Yes (End)", 0, 1);
        pxe_draw_button(surf, dx + 194, dy + 102, 92, 22, st->zh ? "否 (取消)" : "No (Cancel)", 0, 1);
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
        strcpy(st->status_msg, st->zh ? "无窗口" : "No window");
        return;
    }
    if (pxe_r0_focus_process_window(sel->pid) == 0) {
        snprintf(st->status_msg, sizeof(st->status_msg),
                 st->zh ? "已切换 PID %u" : "Switched PID %u", (unsigned)sel->pid);
    } else {
        strcpy(st->status_msg, st->zh ? "切换失败" : "Switch failed");
    }
}

static void execute_kill_pid(taskmgr_state_t* st, uint32_t target_pid, const pxe_win_surface_t* surf) {
    if (target_pid == (uint32_t)st->self_pid) {
        int wid = st->win_id;
        uint8_t* bp = st->back_pixels;
        st->back_pixels = NULL;
        pxe_win_destroy(wid);
        if (bp) free(bp);
        free(st);
        pxe_r0_kill_process(target_pid, 0);
        pxe_exit(0);
        return;
    }
    if (pxe_r0_kill_process(target_pid, -1) == 0) {
        snprintf(st->status_msg, sizeof(st->status_msg),
                 st->zh ? "已结束 PID %u" : "Ended PID %u", (unsigned)target_pid);
        refresh_data(st, surf);
    } else {
        strcpy(st->status_msg, st->zh ? "结束失败" : "End failed");
    }
}

static void perform_kill(taskmgr_state_t* st, const pxe_win_surface_t* surf) {
    const pxe_r0_proc_info_t* sel = get_selected_proc(st);
    if (!is_proc_killable(st, sel)) {
        strcpy(st->status_msg, st->zh ? "受保护的任务" : "Protected task");
        pxe_beep();
        return;
    }
    if (is_critical_system_proc(st, sel)) {
        st->warn_modal = 1;
        st->warn_target_pid = sel->pid;
        strncpy(st->warn_target_name, sel->name, sizeof(st->warn_target_name) - 1);
        st->warn_target_name[sizeof(st->warn_target_name) - 1] = '\0';
        pxe_beep();
        return;
    }
    execute_kill_pid(st, sel->pid, surf);
}

static void toggle_pause(taskmgr_state_t* st) {
    st->paused = !st->paused;
    strcpy(st->status_msg, st->paused ? (st->zh ? "已暂停" : "Paused")
                                      : (st->zh ? "实时 1Hz" : "Live 1Hz"));
}

static void set_sort_mode(taskmgr_state_t* st, int mode, const pxe_win_surface_t* surf) {
    int tx, ty, tw, th, vis_rows;
    st->sort_mode = mode;
    sort_processes(st);
    reselect_pid(st);
    get_table_rect(surf, &tx, &ty, &tw, &th, &vis_rows);
    clamp_scroll(st, vis_rows);
    snprintf(st->status_msg, sizeof(st->status_msg),
             st->zh ? "排序: %s" : "Sort: %s", sort_label(mode, st->zh));
}

static int tm_resolve_app(const char* name, char* out, size_t cap) {
    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);

    char drives[26];
    int dn = 0;
    if (info.boot_mode == 2) {
        if (info.system_drive) drives[dn++] = info.system_drive;
        if (info.cdrom_drive && info.cdrom_drive != info.system_drive) drives[dn++] = info.cdrom_drive;
    } else {
        if (info.cdrom_drive) drives[dn++] = info.cdrom_drive;
        if (info.system_drive && info.system_drive != info.cdrom_drive) drives[dn++] = info.system_drive;
    }
    pxe_drive_info_t all_d[24];
    int adn = pxe_get_drives(all_d, 24);
    for (int i = 0; i < adn; i++) {
        if (all_d[i].present && all_d[i].mounted) {
            int seen = 0;
            for (int j = 0; j < dn; j++) {
                if (drives[j] == all_d[i].letter) { seen = 1; break; }
            }
            if (!seen && dn < 26) drives[dn++] = all_d[i].letter;
        }
    }

    char with_ext[96];
    const char* dot = strrchr(name, '.');
    if (dot) snprintf(with_ext, sizeof(with_ext), "%s", name);
    else snprintf(with_ext, sizeof(with_ext), "%s.pxe", name);

    pxe_dirent_t de;
    for (int i = 0; i < dn; i++) {
        snprintf(out, cap, "%c:\\APPS\\%s", drives[i], with_ext);
        if (pxe_stat(out, &de) == 0 && !de.is_dir) return 0;
        snprintf(out, cap, "%c:\\GAME\\%s", drives[i], with_ext);
        if (pxe_stat(out, &de) == 0 && !de.is_dir) return 0;
    }
    return -1;
}

static void launch_new_task_cmd(taskmgr_state_t* st, const char* raw) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", raw ? raw : "");
    char* p = buf;
    while (*p == ' ' || *p == '\t') p++;
    size_t bl = strlen(p);
    while (bl > 0 && (p[bl - 1] == ' ' || p[bl - 1] == '\t')) p[--bl] = '\0';
    if (!p[0]) return;

    /* Split command token and optional tail argument */
    char cmd_tok[128];
    char* rest = NULL;
    if (p[0] == '"') {
        p++;
        size_t ci = 0;
        while (*p && *p != '"' && ci + 1 < sizeof(cmd_tok)) cmd_tok[ci++] = *p++;
        cmd_tok[ci] = '\0';
        if (*p == '"') p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p) rest = p;
    } else {
        size_t ci = 0;
        while (*p && *p != ' ' && *p != '\t' && ci + 1 < sizeof(cmd_tok)) cmd_tok[ci++] = *p++;
        cmd_tok[ci] = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p) rest = p;
    }

    if (strcasecmp(cmd_tok, "file") == 0 || strcasecmp(cmd_tok, "files") == 0 ||
        strcasecmp(cmd_tok, "file.pxe") == 0 || strcasecmp(cmd_tok, "files.pxe") == 0 ||
        strcasecmp(cmd_tok, "explorer") == 0 || strcasecmp(cmd_tok, "explorer.exe") == 0) {
        if (!pxe_desktop_shell_ctl(0)) {
            pxe_desktop_shell_ctl(1);
            strcpy(st->status_msg, st->zh ? "桌面已启动" : "Desktop Started");
            return;
        }
        char fpath[128];
        if (tm_resolve_app("files.pxe", fpath, sizeof(fpath)) == 0) {
            const char* argv[2] = { fpath, rest };
            if (pxe_spawn(fpath, rest ? 2 : 1, argv) >= 0) {
                strcpy(st->status_msg, st->zh ? "已打开文件管理器" : "File Manager Opened");
                return;
            }
        }
        strcpy(st->status_msg, st->zh ? "启动文件管理器失败" : "Launch files failed");
        pxe_beep();
        return;
    }

    const char* mapped = cmd_tok;
    if (strcasecmp(cmd_tok, "terminal") == 0 || strcasecmp(cmd_tok, "cmd.exe") == 0) mapped = "cmd.pxe";
    else if (strcasecmp(cmd_tok, "control") == 0) mapped = "cpanel.pxe";

    /* If ends with .pat, launch cmd.pxe /c <target> */
    const char* dot = strrchr(mapped, '.');
    if (dot && strcasecmp(dot, ".pat") == 0) {
        char cpath[128];
        if (tm_resolve_app("cmd.pxe", cpath, sizeof(cpath)) == 0) {
            const char* argv[3] = { cpath, "/c", p };
            if (pxe_spawn(cpath, 3, argv) >= 0) {
                strcpy(st->status_msg, st->zh ? "已启动脚本" : "Script started");
                return;
            }
        }
    }

    char target[160];
    pxe_dirent_t de;
    if (strchr(mapped, ':') || strchr(mapped, '\\') || strchr(mapped, '/')) {
        snprintf(target, sizeof(target), "%s", mapped);
        if (pxe_stat(target, &de) != 0 && !dot) {
            snprintf(target, sizeof(target), "%s.pxe", mapped);
        }
    } else if (tm_resolve_app(mapped, target, sizeof(target)) != 0) {
        strcpy(st->status_msg, st->zh ? "找不到程序" : "Program not found");
        pxe_beep();
        return;
    }

    const char* argv[2] = { target, rest };
    if (pxe_spawn(target, rest ? 2 : 1, argv) >= 0) {
        snprintf(st->status_msg, sizeof(st->status_msg),
                 st->zh ? "已启动: %s" : "Started: %s", cmd_tok);
    } else {
        strcpy(st->status_msg, st->zh ? "启动失败" : "Launch failed");
        pxe_beep();
    }
}

static int hit_test_button(const pxe_win_surface_t* surf, int mx, int my) {
    int b1x, b1w, b2x, b2w, b3x, b3w, b4x, b4w, b5x, b5w, by, bh;
    compute_buttons(surf, &b1x, &b1w, &b2x, &b2w, &b3x, &b3w, &b4x, &b4w, &b5x, &b5w, &by, &bh);
    if (pxe_point_in_rect(mx, my, b1x, by, b1w, bh)) return 1;
    if (pxe_point_in_rect(mx, my, b2x, by, b2w, bh)) return 2;
    if (pxe_point_in_rect(mx, my, b3x, by, b3w, bh)) return 3;
    if (pxe_point_in_rect(mx, my, b4x, by, b4w, bh)) return 4;
    if (pxe_point_in_rect(mx, my, b5x, by, b5w, bh)) return 5;
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

static void sync_taskmgr_menu(int win_id, int zh) {
    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 2;

    strcpy(mb.menus[0].title, zh ? "任务" : "Task");
    mb.menus[0].item_count = 4;
    strcpy(mb.menus[0].items[0].label, zh ? "运行新任务..." : "New Task...");
    mb.menus[0].items[0].enabled = 1;
    strcpy(mb.menus[0].items[1].label, zh ? "切换至" : "Switch To");
    mb.menus[0].items[1].enabled = 1;
    strcpy(mb.menus[0].items[2].label, zh ? "结束任务" : "End Task");
    mb.menus[0].items[2].enabled = 1;
    strcpy(mb.menus[0].items[3].label, zh ? "退出" : "Exit");
    mb.menus[0].items[3].enabled = 1;

    strcpy(mb.menus[1].title, zh ? "视图" : "View");
    mb.menus[1].item_count = 6;
    strcpy(mb.menus[1].items[0].label, zh ? "立即刷新" : "Refresh Now");
    mb.menus[1].items[0].enabled = 1;
    strcpy(mb.menus[1].items[1].label, zh ? "暂停/继续" : "Pause/Resume");
    mb.menus[1].items[1].enabled = 1;
    strcpy(mb.menus[1].items[2].label, zh ? "默认排序" : "Sort by Default");
    mb.menus[1].items[2].enabled = 1;
    strcpy(mb.menus[1].items[3].label, zh ? "按 PID 排序" : "Sort by PID");
    mb.menus[1].items[3].enabled = 1;
    strcpy(mb.menus[1].items[4].label, zh ? "按 CPU% 排序" : "Sort by CPU%");
    mb.menus[1].items[4].enabled = 1;
    strcpy(mb.menus[1].items[5].label, zh ? "按内存排序" : "Sort by Memory");
    mb.menus[1].items[5].enabled = 1;

    pxe_win_set_menu(win_id, &mb);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    taskmgr_state_t* st = (taskmgr_state_t*)calloc(1, sizeof(taskmgr_state_t));
    if (!st) return 1;
    st->zh = pxe_is_zh();

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
    strcpy(desc.title, st->zh ? "任务管理器" : "Task Manager");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) {
        free(st);
        return 1;
    }

    sync_taskmgr_menu(win_id, st->zh);

    st->win_id = win_id;
    st->self_pid = pxe_getpid();
    st->selected_idx = 0;
    st->last_click_row = -1;
    strcpy(st->status_msg, st->zh ? "实时 1Hz" : "Live 1Hz");
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
                if (ev.type == PXE_EV_REDRAW) {
                    int new_zh = pxe_is_zh();
                    if (new_zh != st->zh) {
                        st->zh = new_zh;
                        pxe_win_set_title(win_id, st->zh ? "任务管理器" : "Task Manager");
                        sync_taskmgr_menu(win_id, st->zh);
                    }
                }
                refresh_data(st, &surf);
                dirty = 1;
            } else if (ev.type == PXE_EV_RESIZE) {
                if (pxe_win_get_surface(win_id, &surf) == 0) {
                    get_table_rect(&surf, &tx, &ty, &tw, &th, &vis_rows);
                    clamp_scroll(st, vis_rows);
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_MOUSE_DOWN && (ev.buttons & PXE_BTN_LEFT)) {
                if (st->new_task_modal) {
                    int dw = 368, dh = 136;
                    int dx = (surf.width - dw) / 2;
                    int dy = (surf.height - dh) / 2;
                    if (dx < 4) dx = 4;
                    if (dy < 4) dy = 4;
                    if (pxe_point_in_rect(ev.x, ev.y, dx + 82, dy + 98, 92, 24)) {
                        st->new_task_modal = 0;
                        launch_new_task_cmd(st, st->new_task_input);
                        refresh_data(st, &surf);
                    } else if (pxe_point_in_rect(ev.x, ev.y, dx + 194, dy + 98, 92, 24)) {
                        st->new_task_modal = 0;
                        strcpy(st->status_msg, st->zh ? "已取消" : "Cancelled");
                    }
                    dirty = 1;
                    continue;
                }
                if (st->warn_modal) {
                    int dw = 360, dh = 132;
                    int dx = (surf.width - dw) / 2;
                    int dy = (surf.height - dh) / 2;
                    if (dx < 4) dx = 4;
                    if (dy < 4) dy = 4;
                    if (pxe_point_in_rect(ev.x, ev.y, dx + 74, dy + 102, 92, 22)) {
                        uint32_t pid = st->warn_target_pid;
                        st->warn_modal = 0;
                        execute_kill_pid(st, pid, &surf);
                    } else if (pxe_point_in_rect(ev.x, ev.y, dx + 194, dy + 102, 92, 22)) {
                        st->warn_modal = 0;
                        strcpy(st->status_msg, st->zh ? "已取消" : "Cancelled");
                    }
                    dirty = 1;
                    continue;
                }
                int hit = hit_test_button(&surf, ev.x, ev.y);
                if (hit != 0) {
                    const pxe_r0_proc_info_t* sel = get_selected_proc(st);
                    int ok = (hit == 1 || hit == 4 || hit == 5) ||
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
                                         st->zh ? "已选中 PID %u" : "PID %u selected",
                                         (unsigned)st->procs[idx].pid);
                            } else {
                                strcpy(st->status_msg, st->zh ? "受保护的任务" : "Protected task");
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
                        strcpy(st->status_msg, st->zh ? "已刷新" : "Refreshed");
                    } else if (act == 2) {
                        perform_focus(st);
                    } else if (act == 3) {
                        perform_kill(st, &surf);
                    } else if (act == 4) {
                        toggle_pause(st);
                    } else if (act == 5) {
                        st->new_task_modal = 1;
                        st->new_task_input[0] = '\0';
                        st->new_task_len = 0;
                    }
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_MENU) {
                if (ev.arg0 == 0) {
                    if (ev.arg1 == 0) {
                        st->new_task_modal = 1;
                        st->new_task_input[0] = '\0';
                        st->new_task_len = 0;
                        dirty = 1;
                    } else if (ev.arg1 == 1) {
                        perform_focus(st);
                        dirty = 1;
                    } else if (ev.arg1 == 2) {
                        perform_kill(st, &surf);
                        dirty = 1;
                    } else if (ev.arg1 == 3) {
                        running = 0;
                    }
                } else if (ev.arg0 == 1) {
                    if (ev.arg1 == 0) {
                        refresh_data(st, &surf);
                        strcpy(st->status_msg, st->zh ? "已刷新" : "Refreshed");
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
                if (st->new_task_modal) {
                    if (ev.key == PXE_KEY_ENTER) {
                        st->new_task_modal = 0;
                        launch_new_task_cmd(st, st->new_task_input);
                        refresh_data(st, &surf);
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_ESC) {
                        st->new_task_modal = 0;
                        strcpy(st->status_msg, st->zh ? "已取消" : "Cancelled");
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_BACKSPACE) {
                        while (st->new_task_len > 0 &&
                               ((uint8_t)st->new_task_input[st->new_task_len - 1] & 0xC0u) == 0x80u) {
                            st->new_task_len--;
                        }
                        if (st->new_task_len > 0) st->new_task_len--;
                        st->new_task_input[st->new_task_len] = '\0';
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_UTF8 && ev.text[0]) {
                        size_t tl = strlen(ev.text);
                        if ((size_t)st->new_task_len + tl < sizeof(st->new_task_input)) {
                            memcpy(st->new_task_input + st->new_task_len, ev.text, tl);
                            st->new_task_len += (int)tl;
                            st->new_task_input[st->new_task_len] = '\0';
                            dirty = 1;
                        }
                    } else if ((ev.modifiers & PXE_MOD_CTRL) && (ev.key == 'v' || ev.key == 'V' || ev.key == 22)) {
                        char cb[128];
                        int n = pxe_clipboard_get(cb, sizeof(cb));
                        if (n > 0) {
                            for (int i = 0; i < n && (size_t)st->new_task_len + 1 < sizeof(st->new_task_input); i++) {
                                if (cb[i] != '\r' && cb[i] != '\n') {
                                    st->new_task_input[st->new_task_len++] = cb[i];
                                }
                            }
                            st->new_task_input[st->new_task_len] = '\0';
                            dirty = 1;
                        }
                    } else if (ev.key >= 32 && ev.key <= 126 &&
                               st->new_task_len + 1 < (int)sizeof(st->new_task_input)) {
                        st->new_task_input[st->new_task_len++] = (char)ev.key;
                        st->new_task_input[st->new_task_len] = '\0';
                        dirty = 1;
                    }
                    continue;
                }
                if (st->warn_modal) {
                    if (ev.key == PXE_KEY_ENTER || ev.key == 'y' || ev.key == 'Y') {
                        uint32_t pid = st->warn_target_pid;
                        st->warn_modal = 0;
                        execute_kill_pid(st, pid, &surf);
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_ESC || ev.key == 'n' || ev.key == 'N') {
                        st->warn_modal = 0;
                        strcpy(st->status_msg, st->zh ? "已取消" : "Cancelled");
                        dirty = 1;
                    }
                    continue;
                }
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
                    strcpy(st->status_msg, st->zh ? "已刷新" : "Refreshed");
                    dirty = 1;
                } else if (ev.key == 'p' || ev.key == 'P') {
                    toggle_pause(st);
                    dirty = 1;
                } else if (ev.key == 's' || ev.key == 'S') {
                    set_sort_mode(st, (st->sort_mode + 1) % 4, &surf);
                    dirty = 1;
                } else if (ev.key == 'n' || ev.key == 'N') {
                    st->new_task_modal = 1;
                    st->new_task_input[0] = '\0';
                    st->new_task_len = 0;
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
