#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>

static void render_clock(const pxe_win_surface_t* surf,
                         const pxe_rtc_time_t* rtc,
                         int show_date) {
    char buf[32];
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);
    pxe_draw_bevel_box(surf, 8, 8, surf->width - 16, surf->height - 16, 1, PXE_COL_WHITE);

    snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
             (unsigned)rtc->hour, (unsigned)rtc->minute, (unsigned)rtc->second);
    int tx = (surf->width - 64) / 2;
    int ty = show_date ? (surf->height / 2 - 14) : (surf->height / 2 - 8);
    if (tx < 12) tx = 12;
    if (ty < 12) ty = 12;
    pxe_draw_text(surf, tx, ty, buf, PXE_COL_DARK_BLUE);

    if (show_date && surf->height >= 64) {
        snprintf(buf, sizeof(buf), "%04u-%02u-%02u",
                 (unsigned)rtc->year, (unsigned)rtc->month, (unsigned)rtc->day);
        int dx = (surf->width - 60) / 2;
        if (dx < 12) dx = 12;
        pxe_draw_text_small(surf, dx, ty + 20, buf, PXE_COL_DARK_GRAY);
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 100;
    desc.y = 70;
    desc.w = 180;
    desc.h = 80;
    desc.min_w = 140;
    desc.min_h = 70;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_RESIZABLE;
    strcpy(desc.title, "Clock");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    pxe_menu_bar_t mb;
    memset(&mb, 0, sizeof(mb));
    mb.menu_count = 1;
    strcpy(mb.menus[0].title, "Clock");
    mb.menus[0].item_count = 2;
    strcpy(mb.menus[0].items[0].label, "Toggle Date");
    mb.menus[0].items[0].enabled = 1;
    strcpy(mb.menus[0].items[1].label, "Exit");
    mb.menus[0].items[1].enabled = 1;
    pxe_win_set_menu(win_id, &mb);

    pxe_rtc_time_t rtc;
    memset(&rtc, 0, sizeof(rtc));
    pxe_get_rtc_time(&rtc);
    int last_sec = -1;
    int show_date = 1;
    int dirty = 1;
    int running = 1;

    while (running) {
        pxe_event_t ev;
        int rc;
        while ((rc = pxe_win_poll_event(win_id, &ev)) > 0) {
            if (ev.type == PXE_EV_CLOSE) {
                running = 0;
                break;
            } else if (ev.type == PXE_EV_RESIZE) {
                if (pxe_win_get_surface(win_id, &surf) == 0) {
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_REDRAW || ev.type == PXE_EV_ACTIVATE) {
                dirty = 1;
            } else if (ev.type == PXE_EV_MENU && ev.arg0 == 0) {
                if (ev.arg1 == 0) {
                    show_date = !show_date;
                    dirty = 1;
                } else if (ev.arg1 == 1) {
                    running = 0;
                }
            } else if (ev.type == PXE_EV_KEY_DOWN) {
                if (ev.key == 'd' || ev.key == 'D') {
                    show_date = !show_date;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_ESC) {
                    running = 0;
                }
            }
        }
        if (rc < 0) break;

        if (pxe_get_rtc_time(&rtc) == 0) {
            if ((int)rtc.second != last_sec) {
                last_sec = (int)rtc.second;
                dirty = 1;
            }
        }

        if (dirty && running) {
            render_clock(&surf, &rtc, show_date);
            pxe_win_present(win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(50);
        }
    }

    pxe_win_destroy(win_id);
    return 0;
}
