#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>

typedef struct {
    int win_id;
    int pid;
    int clicks;
    int press_btn; /* 0=none, 1=click_me, 2=crash */
    int hover_btn;
    pxe_rtc_time_t rtc;
    pxe_basic_sysinfo_t sys;
} hello_state_t;

static void compute_buttons(const pxe_win_surface_t* surf,
                            int* bx1, int* bw1,
                            int* bx2, int* bw2,
                            int* by, int* bh) {
    int right_margin = 16; /* Stay clear of bottom-right 10x10 resize grip */
    int gap = 8;
    *bh = 24;
    *by = surf->height - 38;
    *bx1 = 10;
    *bw1 = 106;

    int avail2 = surf->width - *bx1 - *bw1 - gap - right_margin;
    if (avail2 > 160) avail2 = 160;
    if (avail2 < 96) avail2 = 96;
    *bw2 = avail2;
    *bx2 = surf->width - right_margin - *bw2;
}

static void render_hello(const pxe_win_surface_t* surf, const hello_state_t* st) {
    char line[160];
    int bx1, bw1, bx2, bw2, by, bh;

    if (!surf || !surf->pixels) return;

    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    /* Top header banner */
    pxe_draw_bevel_box(surf, 10, 10, surf->width - 20, 44, 0, PXE_COL_WHITE);
    snprintf(line, sizeof(line), "Hello from Ring 3 PXE! PID=%d", st->pid);
    pxe_draw_text(surf, 18, 13, line, PXE_COL_DARK_BLUE);

    snprintf(line, sizeof(line), "RTC: %04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)st->rtc.year, (unsigned)st->rtc.month,
             (unsigned)st->rtc.day, (unsigned)st->rtc.hour,
             (unsigned)st->rtc.minute, (unsigned)st->rtc.second);
    pxe_draw_text(surf, 18, 32, line, PXE_COL_BLACK);

    /* Inner system & counter box */
    int panel_y = 62;
    int panel_h = surf->height - panel_y - 48;
    if (panel_h < 42) panel_h = 42;
    pxe_draw_bevel_box(surf, 10, panel_y, surf->width - 20, panel_h, 1, PXE_COL_WHITE);

    snprintf(line, sizeof(line), "Build: %.20s | Clicks: %d",
             st->sys.os_build[0] ? st->sys.os_build : "unknown",
             st->clicks);
    pxe_draw_text(surf, 18, panel_y + 6, line, PXE_COL_BLACK);

    snprintf(line, sizeof(line), "RAM Free: %u / %u MB",
             (unsigned)st->sys.free_ram_mb,
             (unsigned)st->sys.total_ram_mb);
    pxe_draw_text(surf, 18, panel_y + 24, line, PXE_COL_DARK_GREEN);

    if (panel_h >= 66) {
        int max_chars = (surf->width - 80) / 8;
        if (max_chars < 12) max_chars = 12;
        if (max_chars > 48) max_chars = 48;
        char cpu_short[52];
        int i = 0;
        const char* src = st->sys.cpu_model[0] ? st->sys.cpu_model : "x86_64";
        while (src[i] && i < max_chars) {
            cpu_short[i] = src[i];
            i++;
        }
        cpu_short[i] = '\0';
        snprintf(line, sizeof(line), "CPU: %s", cpu_short);
        pxe_draw_text(surf, 18, panel_y + 44, line, PXE_COL_DARK_GRAY);
    }

    if (panel_h >= 84) {
        pxe_draw_text_small(surf, 18, panel_y + 66,
                            "Space/Enter:+1  R:Reset  Menu:Demo",
                            PXE_COL_DARK_BLUE);
    }

    /* Action buttons (kept well away from bottom-right resize corner) */
    compute_buttons(surf, &bx1, &bw1, &bx2, &bw2, &by, &bh);
    pxe_draw_button(surf, bx1, by, bw1, bh, "Click Me!",
                    (st->press_btn == 1 && st->hover_btn == 1), 1);
    pxe_draw_button(surf, bx2, by, bw2, bh,
                    (bw2 < 148) ? "Test Crash" : "Test Ring3 Crash",
                    (st->press_btn == 2 && st->hover_btn == 2), 1);
}

static int hit_test_button(const pxe_win_surface_t* surf, int mx, int my) {
    int bx1, bw1, bx2, bw2, by, bh;
    compute_buttons(surf, &bx1, &bw1, &bx2, &bw2, &by, &bh);
    if (pxe_point_in_rect(mx, my, bx1, by, bw1, bh)) return 1;
    if (pxe_point_in_rect(mx, my, bx2, by, bw2, bh)) return 2;
    return 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = -1;
    desc.y = -1;
    desc.w = 360;
    desc.h = 220;
    desc.min_w = 260;
    desc.min_h = 160;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_RESIZABLE;
    strcpy(desc.title, "Hello PXE (Ring 3)");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) {
        pxe_notify_banner("hello: failed to create window");
        return 1;
    }

    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 1;
    strcpy(mb.menus[0].title, "Demo");
    mb.menus[0].item_count = 4;
    strcpy(mb.menus[0].items[0].label, "Increment");
    mb.menus[0].items[0].enabled = 1;
    strcpy(mb.menus[0].items[1].label, "Reset Counter");
    mb.menus[0].items[1].enabled = 1;
    strcpy(mb.menus[0].items[2].label, "Beep");
    mb.menus[0].items[2].enabled = 1;
    strcpy(mb.menus[0].items[3].label, "Exit");
    mb.menus[0].items[3].enabled = 1;
    pxe_win_set_menu(win_id, &mb);

    hello_state_t st;
    memset(&st, 0, sizeof(st));
    st.win_id = win_id;
    st.pid = pxe_getpid();
    pxe_get_rtc_time(&st.rtc);
    pxe_sys_get_basic_info(&st.sys);

    render_hello(&surf, &st);
    pxe_win_present(win_id);

    uint64_t last_tick = pxe_get_ticks();
    int running = 1;
    int dirty = 0;

    while (running) {
        pxe_event_t ev;
        int rc;
        while ((rc = pxe_win_poll_event(win_id, &ev)) > 0) {
            if (ev.type == PXE_EV_CLOSE) {
                running = 0;
                break;
            } else if (ev.type == PXE_EV_REDRAW) {
                dirty = 1;
            } else if (ev.type == PXE_EV_RESIZE) {
                if (pxe_win_get_surface(win_id, &surf) == 0) {
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_MOUSE_DOWN) {
                if (ev.buttons & PXE_BTN_LEFT) {
                    int hit = hit_test_button(&surf, ev.x, ev.y);
                    st.press_btn = hit;
                    st.hover_btn = hit;
                    if (hit != 0) dirty = 1;
                }
            } else if (ev.type == PXE_EV_MOUSE_MOVE) {
                if (st.press_btn != 0) {
                    int hover = hit_test_button(&surf, ev.x, ev.y);
                    if (hover != st.hover_btn) {
                        st.hover_btn = hover;
                        dirty = 1;
                    }
                }
            } else if (ev.type == PXE_EV_MOUSE_UP) {
                if (st.press_btn != 0) {
                    int hit = hit_test_button(&surf, ev.x, ev.y);
                    int act = (hit == st.press_btn) ? st.press_btn : 0;
                    st.press_btn = 0;
                    st.hover_btn = 0;
                    dirty = 1;
                    if (act == 1) {
                        st.clicks++;
                    } else if (act == 2) {
                        /* Deliberate Ring 3 NULL dereference to verify Crash Bomb dialog */
                        *(volatile int*)0 = 42;
                    }
                }
            } else if (ev.type == PXE_EV_MENU) {
                if (ev.arg0 == 0) {
                    if (ev.arg1 == 0) {
                        st.clicks++;
                        dirty = 1;
                    } else if (ev.arg1 == 1) {
                        st.clicks = 0;
                        dirty = 1;
                    } else if (ev.arg1 == 2) {
                        pxe_beep();
                    } else if (ev.arg1 == 3) {
                        running = 0;
                    }
                }
            } else if (ev.type == PXE_EV_KEY_DOWN) {
                if (ev.key == 'r' || ev.key == 'R') {
                    st.clicks = 0;
                    dirty = 1;
                } else if (ev.key == ' ' || ev.key == PXE_KEY_ENTER) {
                    st.clicks++;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_ESC) {
                    running = 0;
                }
            }
        }
        if (rc < 0) {
            break;
        }

        uint64_t now = pxe_get_ticks();
        if (now - last_tick >= 100) {
            last_tick = now;
            pxe_get_rtc_time(&st.rtc);
            pxe_sys_get_basic_info(&st.sys);
            dirty = 1;
        }

        if (dirty && running) {
            render_hello(&surf, &st);
            pxe_win_present(win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(20);
        }
    }

    pxe_win_destroy(win_id);
    return 0;
}
