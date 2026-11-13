#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/audio.h>
#include <pxe/str.h>

static const int k_bpps[4] = { 32, 24, 16, 8 };
static const char* k_bpp_names[4] = {
    "32-bit (TrueCol)", "24-bit (TrueCol)", "16-bit (HighCol)", "8-bit  (256 Col)"
};

static const char* k_pat_names[6] = {
    "0: Stipple 50%", "1: Diagonal", "2: Micro Dots",
    "3: Brick Tile", "4: Slate Gray", "5: Solid Gray"
};

#define CPANEL_DROPDOWN_VIS 8

typedef struct {
    pxe_basic_sysinfo_t info;
    pxe_display_caps_t  caps;
    int sel_res;
    int sel_bpp;
    int res_scroll;
    int pattern;
    int mouse_speed; /* 1..5 */
    int volume;      /* 0..100 */
    int res_open;
    int bpp_open;
    int drag_mouse;
    int drag_vol;

    int revert_sec;
    int last_rtc_sec;
    int prev_w;
    int prev_h;
    int prev_bpp;

    int wp_modal;
    char wp_path[128];
    int wp_len;
    char notice[56];
} cpanel_t;

static uint8_t bpp_idx_to_mask(int b_idx) {
    if (b_idx == 0) return PXE_DISP_BPP_32;
    if (b_idx == 1) return PXE_DISP_BPP_24;
    if (b_idx == 2) return PXE_DISP_BPP_16;
    if (b_idx == 3) return PXE_DISP_BPP_8;
    return 0u;
}

static int mode_supported(const cpanel_t* cp, int r_idx, int b_idx) {
    if (!cp || r_idx < 0 || r_idx >= cp->caps.mode_count || b_idx < 0 || b_idx > 3) {
        return 0;
    }
    uint8_t bit = bpp_idx_to_mask(b_idx);
    return (cp->caps.modes[r_idx].bpp_mask & bit) ? 1 : 0;
}

static void format_vram_str(uint64_t bytes, char* out, size_t cap) {
    uint64_t kb = bytes / 1024ULL;
    if (kb >= 1024ULL) {
        snprintf(out, cap, "%u.%uMB",
                 (unsigned)(kb / 1024ULL),
                 (unsigned)(((kb % 1024ULL) * 10ULL) / 1024ULL));
    } else {
        snprintf(out, cap, "%uKB", (unsigned)kb);
    }
}

static void clamp_res_scroll(cpanel_t* cp) {
    int max_scroll = cp->caps.mode_count - CPANEL_DROPDOWN_VIS;
    if (max_scroll < 0) max_scroll = 0;
    if (cp->res_scroll > max_scroll) cp->res_scroll = max_scroll;
    if (cp->res_scroll < 0) cp->res_scroll = 0;
}

static void sync_from_sysinfo(cpanel_t* cp) {
    pxe_sys_get_basic_info(&cp->info);
    if (pxe_display_get_caps(&cp->caps) != 0 || cp->caps.mode_count <= 0) {
        memset(&cp->caps, 0, sizeof(cp->caps));
        cp->caps.struct_size = sizeof(cp->caps);
        cp->caps.current_w = cp->info.screen_w > 0 ? cp->info.screen_w : 640;
        cp->caps.current_h = cp->info.screen_h > 0 ? cp->info.screen_h : 480;
        cp->caps.current_bpp = (cp->info.screen_bpp == 8 || cp->info.screen_bpp == 16)
                               ? cp->info.screen_bpp : 16;
        cp->caps.min_w = 640;
        cp->caps.min_h = 480;
        cp->caps.max_w = 1024;
        cp->caps.max_h = 768;
        cp->caps.bpp_mask = PXE_DISP_BPP_8 | PXE_DISP_BPP_16;
        cp->caps.vram_bytes = cp->info.vram_bytes;
        cp->caps.modes[0].width = 640;  cp->caps.modes[0].height = 480;
        cp->caps.modes[0].bpp_mask = PXE_DISP_BPP_8 | PXE_DISP_BPP_16;
        cp->caps.modes[1].width = 800;  cp->caps.modes[1].height = 600;
        cp->caps.modes[1].bpp_mask = PXE_DISP_BPP_8 | PXE_DISP_BPP_16;
        cp->caps.modes[2].width = 1024; cp->caps.modes[2].height = 768;
        cp->caps.modes[2].bpp_mask = PXE_DISP_BPP_8 | PXE_DISP_BPP_16;
        cp->caps.mode_count = 3;
    }

    cp->mouse_speed = (int)cp->info.mouse_speed;
    if (cp->mouse_speed < 1) cp->mouse_speed = 1;
    if (cp->mouse_speed > 5) cp->mouse_speed = 5;
    cp->pattern = (int)cp->info.desktop_pattern % 6;
    cp->volume = (int)cp->info.volume;
    if (cp->volume < 0) cp->volume = 0;
    if (cp->volume > 100) cp->volume = 100;

    int found_res = -1;
    for (int r = 0; r < cp->caps.mode_count; r++) {
        if ((int)cp->caps.modes[r].width == cp->info.screen_w &&
            (int)cp->caps.modes[r].height == cp->info.screen_h) {
            found_res = r;
            break;
        }
    }
    if (found_res < 0 && cp->caps.mode_count > 0) {
        found_res = cp->caps.mode_count - 1;
    }
    cp->sel_res = (found_res >= 0) ? found_res : 0;

    cp->sel_bpp = 2; /* 16-bit fallback default */
    for (int b = 0; b < 4; b++) {
        if (cp->info.screen_bpp == k_bpps[b] && mode_supported(cp, cp->sel_res, b)) {
            cp->sel_bpp = b;
            break;
        }
    }
    if (!mode_supported(cp, cp->sel_res, cp->sel_bpp)) {
        for (int b = 0; b < 4; b++) {
            if (mode_supported(cp, cp->sel_res, b)) {
                cp->sel_bpp = b;
                break;
            }
        }
    }
    if (cp->sel_res >= cp->res_scroll + CPANEL_DROPDOWN_VIS) {
        cp->res_scroll = cp->sel_res - CPANEL_DROPDOWN_VIS + 1;
    } else if (cp->sel_res < cp->res_scroll) {
        cp->res_scroll = cp->sel_res;
    }
    clamp_res_scroll(cp);
}

static void draw_combo(const pxe_win_surface_t* surf, int bx, int by, int bw, int bh,
                       const char* text) {
    pxe_draw_bevel_box(surf, bx, by, bw, bh, 1, PXE_COL_WHITE);
    int abx = bx + bw - 18;
    pxe_draw_bevel_box(surf, abx, by + 1, 17, bh - 2, 0, PXE_COL_LIGHT_GRAY);
    pxe_draw_text_small(surf, abx + 6, by + 6, "v", PXE_COL_BLACK);
    pxe_draw_text_small(surf, bx + 6, by + 6, text, PXE_COL_BLACK);
}

static void draw_mode_btn(const pxe_win_surface_t* surf, int bx, int by, int bw, int bh,
                          const char* label, int active) {
    if (active) {
        pxe_draw_bevel_box(surf, bx, by, bw, bh, 1, PXE_COL_BLUE);
        int tw = (int)strlen(label) * 6;
        pxe_draw_text_small(surf, bx + (bw - tw) / 2, by + 6, label, PXE_COL_WHITE);
    } else {
        pxe_draw_bevel_box(surf, bx, by, bw, bh, 0, PXE_COL_LIGHT_GRAY);
        int tw = (int)strlen(label) * 6;
        pxe_draw_text_small(surf, bx + (bw - tw) / 2, by + 6, label, PXE_COL_BLACK);
    }
}

static void render_cpanel(const pxe_win_surface_t* surf, const cpanel_t* cp) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    /* 1. Mouse Speed Slider (y=12) */
    pxe_draw_text(surf, 10, 10, "Mouse Speed:", PXE_COL_BLACK);
    int mx_tx = 126, mx_ty = 15;
    pxe_draw_bevel_box(surf, mx_tx, mx_ty, 100, 6, 1, PXE_COL_DARK_GRAY);
    for (int i = 0; i < 5; i++) {
        pxe_draw_vline(surf, mx_tx + i * 20 + 9, mx_ty - 2, 10, PXE_COL_BLACK);
    }
    int hx = mx_tx + (cp->mouse_speed - 1) * 20 + 4;
    pxe_draw_bevel_box(surf, hx, mx_ty - 4, 11, 14, 0, PXE_COL_BLUE);

    char sp_str[8];
    snprintf(sp_str, sizeof(sp_str), "%dx", cp->mouse_speed);
    pxe_draw_text_small(surf, 236, 14, sp_str, PXE_COL_BLACK);

    /* 2. Master Volume Slider (y=36) */
    pxe_draw_text(surf, 10, 34, "Sound Vol:", PXE_COL_BLACK);
    int vx_tx = 126, vx_ty = 39;
    pxe_draw_bevel_box(surf, vx_tx, vx_ty, 100, 8, 1, PXE_COL_BLACK);
    int fill_w = (cp->volume * 96) / 100;
    if (fill_w > 0) pxe_fill_rect(surf, vx_tx + 2, vx_ty + 2, fill_w, 4, PXE_COL_GREEN);
    char v_str[12];
    snprintf(v_str, sizeof(v_str), "%d%%", cp->volume);
    pxe_draw_text_small(surf, 236, 38, v_str, PXE_COL_BLACK);

    /* 3. Resolution & Color Depth (y=60, 86) */
    pxe_draw_text(surf, 10, 60, "Resolution:", PXE_COL_BLACK);
    char res_lbl[32];
    if (cp->sel_res >= 0 && cp->sel_res < cp->caps.mode_count) {
        snprintf(res_lbl, sizeof(res_lbl), "%u x %u%s",
                 (unsigned)cp->caps.modes[cp->sel_res].width,
                 (unsigned)cp->caps.modes[cp->sel_res].height,
                 cp->caps.modes[cp->sel_res].preferred ? " *" : "");
    } else {
        snprintf(res_lbl, sizeof(res_lbl), "%dx%d",
                 cp->info.screen_w, cp->info.screen_h);
    }
    draw_combo(surf, 126, 58, 204, 20, res_lbl);

    pxe_draw_text(surf, 10, 86, "Color Depth:", PXE_COL_BLACK);
    draw_combo(surf, 126, 84, 204, 20, k_bpp_names[cp->sel_bpp]);

    /* 4. Desktop Pattern (y=114) */
    pxe_draw_text(surf, 10, 114, "Pattern:", PXE_COL_BLACK);
    pxe_draw_button(surf, 126, 112, 22, 20, "<", 0, 1);
    pxe_draw_button(surf, 308, 112, 22, 20, ">", 0, 1);
    pxe_draw_bevel_box(surf, 150, 112, 156, 20, 1, PXE_COL_WHITE);
    pxe_draw_text_small(surf, 158, 119, k_pat_names[cp->pattern], PXE_COL_BLACK);

    /* 5. Wallpaper Mode / Clear / Browse (y=142) */
    pxe_draw_text(surf, 10, 142, "Wallpaper:", PXE_COL_BLACK);
    if (cp->info.wallpaper_active) {
        int m = (int)cp->info.wallpaper_mode;
        draw_mode_btn(surf, 96,  140, 38, 20, "Fill",  m == 0);
        draw_mode_btn(surf, 136, 140, 36, 20, "Fit",   m == 1);
        draw_mode_btn(surf, 174, 140, 48, 20, "Stret", m == 2);
        draw_mode_btn(surf, 224, 140, 38, 20, "Tile",  m == 3);
        draw_mode_btn(surf, 264, 140, 36, 20, "Off",   0);
        draw_mode_btn(surf, 302, 140, 48, 20, "Path..", 0);
    } else {
        pxe_draw_text_small(surf, 96, 146, "None (Pattern Active)", PXE_COL_DARK_GRAY);
        draw_mode_btn(surf, 270, 140, 80, 20, "Set File..", 0);
    }

    /* 6. Driver / VRAM / Persistence info (y=168..246) */
    pxe_draw_hline(surf, 10, 168, surf->width - 20, PXE_COL_DARK_GRAY);
    pxe_draw_hline(surf, 10, 169, surf->width - 20, PXE_COL_WHITE);

    const char* drv_name = cp->caps.driver_name[0]
        ? cp->caps.driver_name
        : (cp->info.gfx_backend[0] ? cp->info.gfx_backend : "Framebuffer");
    pxe_draw_text_small(surf, 10, 176, "Display:", PXE_COL_BLACK);
    pxe_draw_text_small(surf, 70, 176, drv_name,
                        (cp->caps.flags & PXE_DISP_FLAG_DRIVER_ACTIVE)
                            ? PXE_COL_DARK_GREEN : PXE_COL_DARK_BLUE);

    char vram_str[16];
    char cap_line[64];
    format_vram_str(cp->caps.vram_bytes, vram_str, sizeof(vram_str));
    if (cp->caps.flags & PXE_DISP_FLAG_DRIVER_ACTIVE) {
        snprintf(cap_line, sizeof(cap_line), "VRAM:%s  Max:%dx%d  Min:640x480",
                 vram_str, cp->caps.max_w, cp->caps.max_h);
    } else {
        snprintf(cap_line, sizeof(cap_line), "Fallback(No Drv): Max 1024x768 @ 8/16b");
    }
    pxe_draw_text_small(surf, 10, 194, "Limits:", PXE_COL_BLACK);
    pxe_draw_text_small(surf, 70, 194, cap_line,
                        (cp->caps.flags & PXE_DISP_FLAG_DRIVER_ACTIVE)
                            ? PXE_COL_BLACK : PXE_COL_BROWN);

    uint8_t sel_mask = (cp->sel_res >= 0 && cp->sel_res < cp->caps.mode_count)
                       ? cp->caps.modes[cp->sel_res].bpp_mask : 0u;
    char bpp_line[48];
    snprintf(bpp_line, sizeof(bpp_line), "%s%s%s%s%s",
             (sel_mask & PXE_DISP_BPP_8)  ? "8b "  : "",
             (sel_mask & PXE_DISP_BPP_16) ? "16b " : "",
             (sel_mask & PXE_DISP_BPP_24) ? "24b " : "",
             (sel_mask & PXE_DISP_BPP_32) ? "32b"  : "",
             (sel_mask == 0) ? "None" : "");
    pxe_draw_text_small(surf, 10, 212, "BPP Mode:", PXE_COL_BLACK);
    pxe_draw_text_small(surf, 70, 212, bpp_line, PXE_COL_DARK_BLUE);

    if (cp->notice[0]) {
        pxe_draw_text_small(surf, 10, 234, cp->notice, PXE_COL_DARK_GREEN);
    } else if (!cp->info.settings_writable) {
        pxe_draw_text_small(surf, 10, 234, "Live Mode: .pse settings read-only", PXE_COL_BROWN);
    } else {
        pxe_draw_text_small(surf, 10, 234, "Changes persist to SYSTEM.PSE", PXE_COL_DARK_GRAY);
    }

    /* 7. Bottom Actions (y=260) */
    if (cp->revert_sec > 0) {
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf), "Revert in %2ds...", cp->revert_sec);
        pxe_draw_text(surf, 12, 264, rbuf, PXE_COL_DARK_RED);
        pxe_draw_button(surf, 188, 260, 156, 24, "Keep (OK)", 0, 1);
    } else {
        pxe_draw_button(surf, 10,  260, 108, 24, "Apply Mode", 0, 1);
        pxe_draw_button(surf, 126, 260, 108, 24, "Save (.pse)", 0, 1);
        pxe_draw_button(surf, 242, 260, 108, 24, "OK", 0, 1);
    }

    /* 8. Open Dropdowns on top */
    if (cp->res_open) {
        int vis = (cp->caps.mode_count <= CPANEL_DROPDOWN_VIS)
                  ? cp->caps.mode_count : CPANEL_DROPDOWN_VIS;
        if (vis < 1) vis = 1;
        int has_sb = (cp->caps.mode_count > CPANEL_DROPDOWN_VIS);
        int dx = 126, dy = 78, dw = 204, dh = vis * 18 + 2;
        int list_w = has_sb ? (dw - 18) : (dw - 2);
        pxe_fill_rect(surf, dx, dy, dw, dh, PXE_COL_WHITE);
        pxe_draw_rect(surf, dx, dy, dw, dh, PXE_COL_BLACK);
        for (int row = 0; row < vis; row++) {
            int idx = cp->res_scroll + row;
            if (idx >= cp->caps.mode_count) break;
            int iy = dy + 1 + row * 18;
            if (idx == cp->sel_res) {
                pxe_fill_rect(surf, dx + 1, iy, list_w, 18, PXE_COL_BLUE);
            }
            char item_lbl[32];
            snprintf(item_lbl, sizeof(item_lbl), "%u x %u%s",
                     (unsigned)cp->caps.modes[idx].width,
                     (unsigned)cp->caps.modes[idx].height,
                     cp->caps.modes[idx].preferred ? " *" : "");
            uint32_t fg = (idx == cp->sel_res) ? PXE_COL_WHITE : PXE_COL_BLACK;
            pxe_draw_text_small(surf, dx + 6, iy + 5, item_lbl, fg);
        }
        if (has_sb) {
            int sbx = dx + dw - 18;
            pxe_draw_vline(surf, sbx, dy + 1, dh - 2, PXE_COL_DARK_GRAY);
            pxe_draw_bevel_box(surf, sbx + 1, dy + 1, 16, 18, 0, PXE_COL_LIGHT_GRAY);
            pxe_draw_text_small(surf, sbx + 6, dy + 5, "^", PXE_COL_BLACK);
            pxe_draw_bevel_box(surf, sbx + 1, dy + dh - 19, 16, 18, 0, PXE_COL_LIGHT_GRAY);
            pxe_draw_text_small(surf, sbx + 6, dy + dh - 15, "v", PXE_COL_BLACK);
        }
    } else if (cp->bpp_open) {
        int dx = 126, dy = 104, dw = 204, dh = 4 * 18 + 2;
        pxe_fill_rect(surf, dx, dy, dw, dh, PXE_COL_WHITE);
        pxe_draw_rect(surf, dx, dy, dw, dh, PXE_COL_BLACK);
        for (int i = 0; i < 4; i++) {
            int iy = dy + 1 + i * 18;
            int sup = mode_supported(cp, cp->sel_res, i);
            if (i == cp->sel_bpp && sup) {
                pxe_fill_rect(surf, dx + 1, iy, dw - 2, 18, PXE_COL_BLUE);
            }
            char ubuf[32];
            const char* lbl = k_bpp_names[i];
            if (!sup) {
                snprintf(ubuf, sizeof(ubuf), "%d-bit (Unsupported)", k_bpps[i]);
                lbl = ubuf;
            }
            uint32_t fg = !sup ? PXE_COL_DARK_GRAY :
                          (i == cp->sel_bpp ? PXE_COL_WHITE : PXE_COL_BLACK);
            pxe_draw_text_small(surf, dx + 6, iy + 5, lbl, fg);
        }
    }

    /* 9. Wallpaper File Path Prompt Modal */
    if (cp->wp_modal) {
        int dw = 280, dh = 96;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3, "Set Wallpaper", PXE_COL_WHITE);
        pxe_draw_text_small(surf, dx + 10, dy + 26, "BMP / PNG path:", PXE_COL_BLACK);
        pxe_draw_bevel_box(surf, dx + 10, dy + 38, dw - 20, 20, 1, PXE_COL_WHITE);
        int max_c = (dw - 28) / 6;
        int start = (cp->wp_len > max_c) ? (cp->wp_len - max_c) : 0;
        pxe_draw_text_small(surf, dx + 14, dy + 45, cp->wp_path + start, PXE_COL_BLACK);
        pxe_draw_vline(surf, dx + 14 + (cp->wp_len - start) * 6, dy + 42, 12, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + 54, dy + 66, 72, 22, "Apply", 0, 1);
        pxe_draw_button(surf, dx + 154, dy + 66, 72, 22, "Cancel", 0, 1);
    }
}

static void start_apply_mode(cpanel_t* cp) {
    if (cp->sel_res < 0 || cp->sel_res >= cp->caps.mode_count) {
        pxe_beep();
        snprintf(cp->notice, sizeof(cp->notice), "No valid resolution selected.");
        return;
    }
    int tw = (int)cp->caps.modes[cp->sel_res].width;
    int th = (int)cp->caps.modes[cp->sel_res].height;
    int tb = k_bpps[cp->sel_bpp];
    if (!mode_supported(cp, cp->sel_res, cp->sel_bpp)) {
        pxe_beep();
        snprintf(cp->notice, sizeof(cp->notice), "Unsupported mode / VRAM limit.");
        return;
    }
    if (tw == cp->info.screen_w && th == cp->info.screen_h && tb == cp->info.screen_bpp) {
        snprintf(cp->notice, sizeof(cp->notice), "Already using this display mode.");
        return;
    }
    cp->prev_w = cp->info.screen_w;
    cp->prev_h = cp->info.screen_h;
    cp->prev_bpp = cp->info.screen_bpp;
    if (pxe_display_apply_mode(tw, th, tb) == 0) {
        pxe_rtc_time_t rtc;
        pxe_get_rtc_time(&rtc);
        cp->revert_sec = 15;
        cp->last_rtc_sec = (int)rtc.second;
        sync_from_sysinfo(cp);
        snprintf(cp->notice, sizeof(cp->notice), "Click Keep (OK) to confirm mode.");
    } else {
        pxe_beep();
        snprintf(cp->notice, sizeof(cp->notice), "Display mode switch rejected.");
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 120;
    desc.y = 48;
    desc.w = 360;
    desc.h = 304;
    desc.min_w = 360;
    desc.min_h = 304;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_DEFAULT;
    strcpy(desc.title, "Control Panel");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    cpanel_t cp;
    memset(&cp, 0, sizeof(cp));
    sync_from_sysinfo(&cp);
    snprintf(cp.wp_path, sizeof(cp.wp_path), "%c:\\WALLPAPER.BMP",
             cp.info.system_drive ? cp.info.system_drive : 'C');
    cp.wp_len = (int)strlen(cp.wp_path);

    int dirty = 1;
    int running = 1;

    while (running) {
        pxe_event_t ev;
        int rc;
        while ((rc = pxe_win_poll_event(win_id, &ev)) > 0) {
            if (ev.type == PXE_EV_CLOSE) {
                if (cp.revert_sec > 0) {
                    pxe_display_apply_mode(cp.prev_w, cp.prev_h, cp.prev_bpp);
                }
                running = 0;
                break;
            } else if (ev.type == PXE_EV_REDRAW || ev.type == PXE_EV_ACTIVATE) {
                sync_from_sysinfo(&cp);
                dirty = 1;
            } else if (ev.type == PXE_EV_MOUSE_MOVE && (ev.buttons & PXE_BTN_LEFT)) {
                if (cp.drag_mouse) {
                    int val = ((ev.x - 126) / 20) + 1;
                    if (val < 1) val = 1;
                    if (val > 5) val = 5;
                    if (val != cp.mouse_speed) {
                        cp.mouse_speed = val;
                        pxe_mouse_set_speed(val);
                        dirty = 1;
                    }
                } else if (cp.drag_vol) {
                    int vol = ((ev.x - 126) * 100) / 100;
                    if (vol < 0) vol = 0;
                    if (vol > 100) vol = 100;
                    if (vol != cp.volume) {
                        cp.volume = vol;
                        pxe_audio_set_volume(vol);
                        dirty = 1;
                    }
                }
            } else if (ev.type == PXE_EV_MOUSE_UP) {
                if (cp.drag_mouse) {
                    cp.drag_mouse = 0;
                    pxe_settings_save();
                }
                cp.drag_vol = 0;
            } else if (ev.type == PXE_EV_KEY_DOWN && cp.wp_modal) {
                if (ev.key == PXE_KEY_ESC) {
                    cp.wp_modal = 0;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_ENTER) {
                    cp.wp_modal = 0;
                    int mode = cp.info.wallpaper_active ? (int)cp.info.wallpaper_mode : 0;
                    if (pxe_desktop_set_wallpaper(cp.wp_path, mode) == 0) {
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice), "Wallpaper applied.");
                    } else {
                        pxe_beep();
                        snprintf(cp.notice, sizeof(cp.notice), "Failed to load wallpaper.");
                    }
                    dirty = 1;
                } else if (ev.key == PXE_KEY_BACKSPACE) {
                    if (cp.wp_len > 0) {
                        cp.wp_path[--cp.wp_len] = '\0';
                        dirty = 1;
                    }
                } else if (ev.key >= 32 && ev.key <= 126 && cp.wp_len < 120) {
                    cp.wp_path[cp.wp_len++] = (char)ev.key;
                    cp.wp_path[cp.wp_len] = '\0';
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_KEY_DOWN && cp.res_open) {
                if (ev.key == PXE_KEY_ESC || ev.key == PXE_KEY_ENTER) {
                    cp.res_open = 0;
                    dirty = 1;
                } else if (ev.key == PXE_KEY_UP && cp.sel_res > 0) {
                    cp.sel_res--;
                    if (cp.sel_res < cp.res_scroll) cp.res_scroll = cp.sel_res;
                    if (!mode_supported(&cp, cp.sel_res, cp.sel_bpp)) {
                        for (int b = 0; b < 4; b++) {
                            if (mode_supported(&cp, cp.sel_res, b)) {
                                cp.sel_bpp = b;
                                break;
                            }
                        }
                    }
                    dirty = 1;
                } else if (ev.key == PXE_KEY_DOWN && cp.sel_res + 1 < cp.caps.mode_count) {
                    cp.sel_res++;
                    if (cp.sel_res >= cp.res_scroll + CPANEL_DROPDOWN_VIS) {
                        cp.res_scroll = cp.sel_res - CPANEL_DROPDOWN_VIS + 1;
                    }
                    if (!mode_supported(&cp, cp.sel_res, cp.sel_bpp)) {
                        for (int b = 0; b < 4; b++) {
                            if (mode_supported(&cp, cp.sel_res, b)) {
                                cp.sel_bpp = b;
                                break;
                            }
                        }
                    }
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_MOUSE_DOWN && (ev.buttons & PXE_BTN_LEFT)) {
                int mx = ev.x, my = ev.y;

                if (cp.wp_modal) {
                    int dw = 280, dh = 96;
                    int dx = (surf.width - dw) / 2;
                    int dy = (surf.height - dh) / 2;
                    if (pxe_point_in_rect(mx, my, dx + 54, dy + 66, 72, 22)) {
                        cp.wp_modal = 0;
                        int mode = cp.info.wallpaper_active ? (int)cp.info.wallpaper_mode : 0;
                        if (pxe_desktop_set_wallpaper(cp.wp_path, mode) == 0) {
                            pxe_settings_save();
                            sync_from_sysinfo(&cp);
                            snprintf(cp.notice, sizeof(cp.notice), "Wallpaper applied.");
                        } else {
                            pxe_beep();
                            snprintf(cp.notice, sizeof(cp.notice), "Failed to load wallpaper.");
                        }
                    } else if (pxe_point_in_rect(mx, my, dx + 154, dy + 66, 72, 22)) {
                        cp.wp_modal = 0;
                    }
                    dirty = 1;
                    continue;
                }

                if (cp.res_open) {
                    int vis = (cp.caps.mode_count <= CPANEL_DROPDOWN_VIS)
                              ? cp.caps.mode_count : CPANEL_DROPDOWN_VIS;
                    if (vis < 1) vis = 1;
                    int has_sb = (cp.caps.mode_count > CPANEL_DROPDOWN_VIS);
                    int dx = 126, dy = 78, dw = 204, dh = vis * 18 + 2;
                    int list_w = has_sb ? (dw - 18) : (dw - 2);
                    int sbx = dx + dw - 18;
                    if (has_sb && pxe_point_in_rect(mx, my, sbx, dy + 1, 18, 18)) {
                        if (cp.res_scroll > 0) cp.res_scroll--;
                        dirty = 1;
                        continue;
                    }
                    if (has_sb && pxe_point_in_rect(mx, my, sbx, dy + dh - 19, 18, 18)) {
                        int max_sc = cp.caps.mode_count - CPANEL_DROPDOWN_VIS;
                        if (cp.res_scroll < max_sc) cp.res_scroll++;
                        dirty = 1;
                        continue;
                    }
                    if (pxe_point_in_rect(mx, my, dx + 1, dy + 1, list_w, vis * 18)) {
                        int row = (my - (dy + 1)) / 18;
                        int idx = cp.res_scroll + row;
                        if (idx >= 0 && idx < cp.caps.mode_count) {
                            cp.sel_res = idx;
                            if (!mode_supported(&cp, cp.sel_res, cp.sel_bpp)) {
                                for (int b = 0; b < 4; b++) {
                                    if (mode_supported(&cp, cp.sel_res, b)) {
                                        cp.sel_bpp = b;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    cp.res_open = 0;
                    dirty = 1;
                    continue;
                }

                if (cp.bpp_open) {
                    if (pxe_point_in_rect(mx, my, 126, 105, 204, 4 * 18)) {
                        int idx = (my - 105) / 18;
                        if (idx >= 0 && idx < 4 && mode_supported(&cp, cp.sel_res, idx)) {
                            cp.sel_bpp = idx;
                        }
                    }
                    cp.bpp_open = 0;
                    dirty = 1;
                    continue;
                }

                /* Mouse speed slider */
                if (pxe_point_in_rect(mx, my, 122, 8, 108, 22)) {
                    int val = ((mx - 126) / 20) + 1;
                    if (val < 1) val = 1;
                    if (val > 5) val = 5;
                    cp.mouse_speed = val;
                    cp.drag_mouse = 1;
                    pxe_mouse_set_speed(val);
                    dirty = 1;
                    continue;
                }

                /* Volume slider */
                if (pxe_point_in_rect(mx, my, 124, 33, 104, 20)) {
                    int vol = ((mx - 126) * 100) / 100;
                    if (vol < 0) vol = 0;
                    if (vol > 100) vol = 100;
                    cp.volume = vol;
                    cp.drag_vol = 1;
                    pxe_audio_set_volume(vol);
                    dirty = 1;
                    continue;
                }

                /* Dropdowns */
                if (pxe_point_in_rect(mx, my, 126, 58, 204, 20)) {
                    cp.res_open = 1;
                    dirty = 1;
                    continue;
                }
                if (pxe_point_in_rect(mx, my, 126, 84, 204, 20)) {
                    cp.bpp_open = 1;
                    dirty = 1;
                    continue;
                }

                /* Pattern arrows */
                if (pxe_point_in_rect(mx, my, 126, 112, 22, 20)) {
                    cp.pattern = (cp.pattern + 5) % 6;
                    pxe_desktop_set_pattern(cp.pattern);
                    pxe_settings_save();
                    snprintf(cp.notice, sizeof(cp.notice), "Pattern updated.");
                    dirty = 1;
                    continue;
                }
                if (pxe_point_in_rect(mx, my, 308, 112, 22, 20)) {
                    cp.pattern = (cp.pattern + 1) % 6;
                    pxe_desktop_set_pattern(cp.pattern);
                    pxe_settings_save();
                    snprintf(cp.notice, sizeof(cp.notice), "Pattern updated.");
                    dirty = 1;
                    continue;
                }

                /* Wallpaper row */
                if (cp.info.wallpaper_active) {
                    if (pxe_point_in_rect(mx, my, 96, 140, 38, 20)) {
                        pxe_desktop_set_wallpaper_mode(0);
                        sync_from_sysinfo(&cp);
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 136, 140, 36, 20)) {
                        pxe_desktop_set_wallpaper_mode(1);
                        sync_from_sysinfo(&cp);
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 174, 140, 48, 20)) {
                        pxe_desktop_set_wallpaper_mode(2);
                        sync_from_sysinfo(&cp);
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 224, 140, 38, 20)) {
                        pxe_desktop_set_wallpaper_mode(3);
                        sync_from_sysinfo(&cp);
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 264, 140, 36, 20)) {
                        pxe_desktop_clear_wallpaper();
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice), "Wallpaper cleared.");
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 302, 140, 48, 20)) {
                        cp.wp_modal = 1;
                        dirty = 1;
                    }
                } else if (pxe_point_in_rect(mx, my, 270, 140, 80, 20)) {
                    cp.wp_modal = 1;
                    dirty = 1;
                }

                /* Bottom actions */
                if (cp.revert_sec > 0) {
                    if (pxe_point_in_rect(mx, my, 188, 260, 156, 24)) {
                        cp.revert_sec = 0;
                        pxe_display_set_mode(cp.info.screen_w, cp.info.screen_h, cp.info.screen_bpp);
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice), "Display mode confirmed.");
                        dirty = 1;
                    }
                } else {
                    if (pxe_point_in_rect(mx, my, 10, 260, 108, 24)) {
                        start_apply_mode(&cp);
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 126, 260, 108, 24)) {
                        if (pxe_settings_save() == 0) {
                            snprintf(cp.notice, sizeof(cp.notice), "Saved settings to .pse.");
                        } else {
                            pxe_beep();
                            snprintf(cp.notice, sizeof(cp.notice), "Save failed (read-only medium?).");
                        }
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 242, 260, 108, 24)) {
                        int tw = (cp.sel_res >= 0 && cp.sel_res < cp.caps.mode_count)
                                 ? (int)cp.caps.modes[cp.sel_res].width : cp.info.screen_w;
                        int th = (cp.sel_res >= 0 && cp.sel_res < cp.caps.mode_count)
                                 ? (int)cp.caps.modes[cp.sel_res].height : cp.info.screen_h;
                        int tb = k_bpps[cp.sel_bpp];
                        if (tw != cp.info.screen_w || th != cp.info.screen_h || tb != cp.info.screen_bpp) {
                            start_apply_mode(&cp);
                            dirty = 1;
                        } else {
                            pxe_desktop_set_pattern(cp.pattern);
                            pxe_mouse_set_speed(cp.mouse_speed);
                            pxe_settings_save();
                            running = 0;
                        }
                    }
                }
            }
        }
        if (rc < 0) break;

        if (running && cp.revert_sec > 0) {
            pxe_rtc_time_t rtc;
            if (pxe_get_rtc_time(&rtc) == 0 && (int)rtc.second != cp.last_rtc_sec) {
                cp.last_rtc_sec = (int)rtc.second;
                cp.revert_sec--;
                if (cp.revert_sec <= 0) {
                    cp.revert_sec = 0;
                    pxe_display_apply_mode(cp.prev_w, cp.prev_h, cp.prev_bpp);
                    sync_from_sysinfo(&cp);
                    snprintf(cp.notice, sizeof(cp.notice), "Reverted display mode.");
                }
                dirty = 1;
            }
        }

        if (dirty && running) {
            render_cpanel(&surf, &cp);
            pxe_win_present(win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(50);
        }
    }

    pxe_win_destroy(win_id);
    return 0;
}

