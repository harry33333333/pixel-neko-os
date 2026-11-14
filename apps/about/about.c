#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/str.h>

static void render_about(const pxe_win_surface_t* surf,
                         const pxe_basic_sysinfo_t* info,
                         int ok_pressed) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    /* Classic TBMK 4-square emblem */
    int lx = (surf->width - 41) / 2;
    int ly = 8;
    pxe_fill_rect(surf, lx - 7, ly + 3, 55, 15, PXE_COL_DARK_BLUE);
    pxe_draw_hline(surf, lx - 4, ly + 3, 49, PXE_COL_BLUE);

    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12; x++) {
            uint32_t c = (y == 0 || x == 0) ? PXE_COL_RED : PXE_COL_DARK_RED;
            pxe_put_pixel(surf, lx + x, ly + y, c);
        }
    }
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12; x++) {
            uint32_t c = (y == 0 || x == 0) ? PXE_COL_GREEN : PXE_COL_DARK_GREEN;
            pxe_put_pixel(surf, lx + 10 + x, ly + y, c);
        }
    }
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12; x++) {
            uint32_t c = (y == 0 || x == 0) ? PXE_COL_CYAN : PXE_COL_DARK_CYAN;
            pxe_put_pixel(surf, lx + 19 + x, ly + 7 + y, c);
        }
    }
    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12; x++) {
            uint32_t c = (y == 0 || x == 0) ? PXE_COL_MAGENTA : PXE_COL_DARK_MAGENTA;
            pxe_put_pixel(surf, lx + 29 + x, ly + 7 + y, c);
        }
    }

    pxe_draw_text(surf, lx + 2,  ly + 24, "T", PXE_COL_DARK_BLUE);
    pxe_draw_text(surf, lx + 10, ly + 24, "B", PXE_COL_DARK_GREEN);
    pxe_draw_text(surf, lx + 18, ly + 24, "M", PXE_COL_DARK_CYAN);
    pxe_draw_text(surf, lx + 26, ly + 24, "K", PXE_COL_DARK_MAGENTA);

    char line[96];
    snprintf(line, sizeof(line), "Codename %s",
             info->os_codename[0] ? info->os_codename : "Pixel Neko Next");
    pxe_draw_text(surf, 10, 44, line, PXE_COL_BLACK);

    snprintf(line, sizeof(line), "Version %s",
             info->os_build[0] ? info->os_build : "Unknown");
    pxe_draw_text(surf, 10, 64, line, PXE_COL_BLACK);

    pxe_draw_text(surf, 10, 84, "(C) 2026 Tairitsu_tty / XNZM", PXE_COL_BLACK);

    pxe_draw_text(surf, 10, 104, "CPU:", PXE_COL_BLACK);
    char cpu_disp[64];
    int max_chars = (surf->width - 58) / 8;
    if (max_chars < 1) max_chars = 1;
    if (max_chars > (int)sizeof(cpu_disp) - 1) max_chars = (int)sizeof(cpu_disp) - 1;
    strncpy(cpu_disp, info->cpu_model[0] ? info->cpu_model : "x86_64", (size_t)max_chars);
    cpu_disp[max_chars] = '\0';
    pxe_draw_text(surf, 50, 104, cpu_disp, PXE_COL_BLACK);

    snprintf(line, sizeof(line), "Total RAM: %u MB / Usable: %u MB",
             (unsigned)info->total_ram_mb, (unsigned)info->usable_ram_mb);
    pxe_draw_text(surf, 10, 124, line, PXE_COL_BLACK);

    pxe_draw_button(surf, surf->width - 62, surf->height - 30, 50, 20, "OK", ok_pressed, 1);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 80;
    desc.y = 52;
    desc.w = 420;
    desc.h = 180;
    desc.min_w = 420;
    desc.min_h = 180;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_DEFAULT;
    strcpy(desc.title, "About");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);

    int ok_pressed = 0;
    render_about(&surf, &info, ok_pressed);
    pxe_win_present(win_id);

    int running = 1;
    while (running) {
        pxe_event_t ev;
        int rc = pxe_win_wait_event(win_id, &ev);
        if (rc < 0) break;
        if (rc == 0) continue;

        int dirty = 0;
        int bx = surf.width - 62;
        int by = surf.height - 30;

        switch (ev.type) {
            case PXE_EV_CLOSE:
                running = 0;
                break;
            case PXE_EV_REDRAW:
            case PXE_EV_ACTIVATE:
                pxe_sys_get_basic_info(&info);
                dirty = 1;
                break;
            case PXE_EV_KEY_DOWN:
                if (ev.key == PXE_KEY_ENTER || ev.key == ' ' || ev.key == PXE_KEY_ESC) {
                    running = 0;
                }
                break;
            case PXE_EV_MOUSE_DOWN:
                if ((ev.buttons & PXE_BTN_LEFT) &&
                    pxe_point_in_rect(ev.x, ev.y, bx, by, 50, 20)) {
                    ok_pressed = 1;
                    dirty = 1;
                }
                break;
            case PXE_EV_MOUSE_UP:
                if (ok_pressed) {
                    int inside = pxe_point_in_rect(ev.x, ev.y, bx, by, 50, 20);
                    ok_pressed = 0;
                    if (inside) {
                        running = 0;
                    } else {
                        dirty = 1;
                    }
                }
                break;
            default:
                break;
        }

        if (dirty && running) {
            render_about(&surf, &info, ok_pressed);
            pxe_win_present(win_id);
        }
    }

    pxe_win_destroy(win_id);
    return 0;
}
