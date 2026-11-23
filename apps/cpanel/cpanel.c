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
    int drag_res_sb;
    int res_sb_drag_y;
    int res_sb_drag_off;

    int revert_sec;
    int last_rtc_sec;
    int prev_w;
    int prev_h;
    int prev_bpp;
    int pending_reboot;
    int pending_w;
    int pending_h;
    int pending_bpp;
    int restart_confirm;

    int wp_modal;
    char wp_path[128];
    int wp_len;
    char notice[80];
    int zh;
} cpanel_t;

#define CP_TR(cp, zh_s, en_s) ((cp)->zh ? (zh_s) : (en_s))

static const char* k_pat_names_zh[6] = {
    "0: 50% 网点", "1: 斜纹纹理", "2: 微点阵",
    "3: 砖格纹理", "4: 石板灰", "5: 纯灰背景"
};
static const char* k_bpp_names_zh[4] = {
    "32位 (真彩色)", "24位 (真彩色)", "16位 (增强色)", "8位 (256色)"
};

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

static int cpanel_vol_from_x(int mx) {
    int off = mx - 128;
    if (off < 0) off = 0;
    if (off > 96) off = 96;
    return (off * 100 + 48) / 96;
}

static int sync_volume_from_sysinfo(cpanel_t* cp) {
    pxe_audio_status_t st;
    int vol = cp->volume;
    if (pxe_audio_get_status(&st) == 0) {
        vol = st.muted ? 0 : (int)st.volume;
    } else {
        pxe_basic_sysinfo_t info;
        if (pxe_sys_get_basic_info(&info) == 0) {
            vol = (int)info.volume;
        }
    }
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    if (vol != cp->volume) {
        cp->volume = vol;
        cp->info.volume = (uint8_t)vol;
        return 1;
    }
    return 0;
}

static void cpanel_apply_volume(cpanel_t* cp, int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    pxe_audio_set_volume(vol);
    pxe_audio_status_t st;
    if (pxe_audio_get_status(&st) == 0) {
        cp->volume = st.muted ? 0 : (int)st.volume;
    } else {
        cp->volume = vol;
    }
    if (cp->volume < 0) cp->volume = 0;
    if (cp->volume > 100) cp->volume = 100;
    cp->info.volume = (uint8_t)cp->volume;
}

static void clamp_res_scroll(cpanel_t* cp) {
    int max_scroll = cp->caps.mode_count - CPANEL_DROPDOWN_VIS;
    if (max_scroll < 0) max_scroll = 0;
    if (cp->res_scroll > max_scroll) cp->res_scroll = max_scroll;
    if (cp->res_scroll < 0) cp->res_scroll = 0;
}

static void sync_from_sysinfo(cpanel_t* cp) {
    pxe_sys_get_basic_info(&cp->info);
    cp->zh = (cp->info.ui_lang != PXE_LANG_EN);
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
    if (!cp->drag_vol) {
        cp->volume = (int)cp->info.volume;
        if (cp->volume < 0) cp->volume = 0;
        if (cp->volume > 100) cp->volume = 100;
        (void)sync_volume_from_sysinfo(cp);
    }

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
        int tw = pxe_text_width_small(label);
        pxe_draw_text_small(surf, bx + (bw - tw) / 2, by + 6, label, PXE_COL_WHITE);
    } else {
        pxe_draw_bevel_box(surf, bx, by, bw, bh, 0, PXE_COL_LIGHT_GRAY);
        int tw = pxe_text_width_small(label);
        pxe_draw_text_small(surf, bx + (bw - tw) / 2, by + 6, label, PXE_COL_BLACK);
    }
}

static void render_cpanel(const pxe_win_surface_t* surf, const cpanel_t* cp) {
    pxe_fill_rect(surf, 0, 0, surf->width, surf->height, PXE_COL_LIGHT_GRAY);

    /* 1. Mouse Speed Slider (y=12) */
    pxe_draw_text(surf, 10, 10, CP_TR(cp, "鼠标速度:", "Mouse Speed:"), PXE_COL_BLACK);
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
    pxe_draw_text(surf, 10, 34, CP_TR(cp, "系统音量:", "Sound Vol:"), PXE_COL_BLACK);
    int vx_tx = 126, vx_ty = 39;
    pxe_draw_bevel_box(surf, vx_tx, vx_ty, 100, 8, 1, PXE_COL_BLACK);
    int fill_w = (cp->volume * 96 + 50) / 100;
    if (fill_w > 0) pxe_fill_rect(surf, vx_tx + 2, vx_ty + 2, fill_w, 4, PXE_COL_GREEN);
    char v_str[12];
    snprintf(v_str, sizeof(v_str), "%d%%", cp->volume);
    pxe_draw_text_small(surf, 236, 38, v_str, PXE_COL_BLACK);

    /* 3. Resolution & Color Depth (y=60, 86) */
    pxe_draw_text(surf, 10, 60, CP_TR(cp, "屏幕分辨率:", "Resolution:"), PXE_COL_BLACK);
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

    pxe_draw_text(surf, 10, 86, CP_TR(cp, "颜色深度:", "Color Depth:"), PXE_COL_BLACK);
    draw_combo(surf, 126, 84, 204, 20,
               cp->zh ? k_bpp_names_zh[cp->sel_bpp] : k_bpp_names[cp->sel_bpp]);

    /* 4. Desktop Pattern (y=114) */
    pxe_draw_text(surf, 10, 114, CP_TR(cp, "桌面图案:", "Pattern:"), PXE_COL_BLACK);
    pxe_draw_button(surf, 126, 112, 22, 20, "<", 0, 1);
    pxe_draw_button(surf, 308, 112, 22, 20, ">", 0, 1);
    pxe_draw_bevel_box(surf, 150, 112, 156, 20, 1, PXE_COL_WHITE);
    pxe_draw_text_small(surf, 158, 116,
                        cp->zh ? k_pat_names_zh[cp->pattern] : k_pat_names[cp->pattern],
                        PXE_COL_BLACK);

    /* 5. Wallpaper Mode / Clear / Browse (y=142) */
    pxe_draw_text(surf, 10, 142, CP_TR(cp, "桌面壁纸:", "Wallpaper:"), PXE_COL_BLACK);
    if (cp->info.wallpaper_active) {
        int m = (int)cp->info.wallpaper_mode;
        draw_mode_btn(surf, 96,  140, 34, 20, CP_TR(cp, "填充", "Fill"), m == 0);
        draw_mode_btn(surf, 132, 140, 32, 20, CP_TR(cp, "适应", "Fit"),  m == 1);
        draw_mode_btn(surf, 166, 140, 40, 20, CP_TR(cp, "拉伸", "Strt"), m == 2);
        draw_mode_btn(surf, 208, 140, 34, 20, CP_TR(cp, "平铺", "Tile"), m == 3);
        draw_mode_btn(surf, 244, 140, 40, 20, CP_TR(cp, "居中", "Cntr"), m == 4);
        draw_mode_btn(surf, 286, 140, 30, 20, CP_TR(cp, "关闭", "Off"),  0);
        draw_mode_btn(surf, 318, 140, 34, 20, CP_TR(cp, "路径", "Path"), 0);
    } else {
        pxe_draw_text_small(surf, 96, 144,
                            CP_TR(cp, "无 (当前使用图案)", "None (Pattern Active)"),
                            PXE_COL_DARK_GRAY);
        draw_mode_btn(surf, 270, 140, 80, 20, CP_TR(cp, "选择文件..", "Set File.."), 0);
    }

    /* 6. Default appearance and top-bar clock */
    draw_mode_btn(surf, 96, 168, 102, 20, CP_TR(cp, "默认壁纸", "Default Wall"), 0);
    draw_mode_btn(surf, 204, 168, 56, 20, CP_TR(cp, "日期", "Date"),
                  cp->info.clock_show_date ? 1 : 0);
    draw_mode_btn(surf, 264, 168, 74, 20, CP_TR(cp, "显示秒", "Seconds"),
                  cp->info.clock_show_seconds ? 1 : 0);

    /* 6b. System Language Switch (y=194) */
    pxe_draw_text(surf, 10, 196, CP_TR(cp, "界面语言:", "Language:"), PXE_COL_BLACK);
    draw_mode_btn(surf, 126, 194, 88, 20, "简体中文", cp->zh ? 1 : 0);
    draw_mode_btn(surf, 222, 194, 88, 20, "English",  cp->zh ? 0 : 1);

    /* 7. Driver / VRAM / Persistence info */
    pxe_draw_hline(surf, 10, 220, surf->width - 20, PXE_COL_DARK_GRAY);
    pxe_draw_hline(surf, 10, 221, surf->width - 20, PXE_COL_WHITE);

    const char* drv_name = cp->caps.driver_name[0]
        ? cp->caps.driver_name
        : (cp->info.gfx_backend[0] ? cp->info.gfx_backend : "Framebuffer");
    pxe_draw_text_small(surf, 10, 228, CP_TR(cp, "显示适配器:", "Display:"), PXE_COL_BLACK);
    pxe_draw_text_small(surf, 82, 228, drv_name,
                        (cp->caps.flags & PXE_DISP_FLAG_DRIVER_ACTIVE)
                            ? PXE_COL_DARK_GREEN : PXE_COL_DARK_BLUE);

    char vram_str[16];
    char cap_line[64];
    format_vram_str(cp->caps.vram_bytes, vram_str, sizeof(vram_str));
    if (cp->caps.flags & PXE_DISP_FLAG_UNLIMITED_RES) {
        snprintf(cap_line, sizeof(cap_line),
                 cp->zh ? "显存:%s  上限:%dx%d" : "VRAM:%s  Max:%dx%d  Min:640x480",
                 vram_str, cp->caps.max_w, cp->caps.max_h);
    } else if (cp->caps.flags & PXE_DISP_FLAG_DRIVER_ACTIVE) {
        snprintf(cap_line, sizeof(cap_line),
                 cp->zh ? "固定扫描输出: %dx%d" : "Fixed Scanout (No Modeset): %dx%d",
                 cp->caps.current_w, cp->caps.current_h);
    } else {
        snprintf(cap_line, sizeof(cap_line),
                 cp->zh ? "固定帧缓冲: %dx%d" : "Fixed GOP/VBE LFB: %dx%d",
                 cp->caps.current_w, cp->caps.current_h);
    }
    pxe_draw_text_small(surf, 10, 246, CP_TR(cp, "能力限制:", "Limits:"), PXE_COL_BLACK);
    pxe_draw_text_small(surf, 82, 246, cap_line,
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
             (sel_mask == 0) ? CP_TR(cp, "无", "None") : "");
    pxe_draw_text_small(surf, 10, 264, CP_TR(cp, "支持色深:", "BPP Mode:"), PXE_COL_BLACK);
    pxe_draw_text_small(surf, 82, 264, bpp_line, PXE_COL_DARK_BLUE);

    if (cp->notice[0]) {
        pxe_draw_text_small(surf, 10, 286, cp->notice, PXE_COL_DARK_GREEN);
    } else if (!cp->info.settings_writable) {
        pxe_draw_text_small(surf, 10, 286,
                            CP_TR(cp, "实时模式: 设置仅在本次会话生效",
                                      "Live Mode: settings apply for this session"),
                            PXE_COL_BROWN);
    } else {
        pxe_draw_text_small(surf, 10, 286,
                            CP_TR(cp, "设置变更将持久保存至 SYSTEM.PSE",
                                      "Changes persist to SYSTEM.PSE"),
                            PXE_COL_DARK_GRAY);
    }

    /* 8. Bottom Actions (y=312) */
    if (cp->revert_sec > 0) {
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf),
                 CP_TR(cp, "%2d 秒后还原...", "Revert in %2ds..."), cp->revert_sec);
        pxe_draw_text(surf, 12, 316, rbuf, PXE_COL_DARK_RED);
        pxe_draw_button(surf, 188, 312, 156, 24, CP_TR(cp, "保留更改", "Keep (OK)"), 0, 1);
    } else {
        pxe_draw_button(surf, 10,  312, 108, 24, CP_TR(cp, "应用模式", "Apply Mode"), 0, 1);
        pxe_draw_button(surf, 126, 312, 108, 24, CP_TR(cp, "保存 (.pse)", "Save (.pse)"), 0, 1);
        pxe_draw_button(surf, 242, 312, 108, 24, CP_TR(cp, "确定", "OK"), 0, 1);
    }

    /* 9. Open Dropdowns on top */
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
            pxe_draw_scrollbar(surf, dx + dw - 17, dy + 1, 16, dh - 2,
                               cp->res_scroll, vis, cp->caps.mode_count);
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
            char ubuf[40];
            const char* lbl = cp->zh ? k_bpp_names_zh[i] : k_bpp_names[i];
            if (!sup) {
                snprintf(ubuf, sizeof(ubuf),
                         cp->zh ? "%d位 (不支持)" : "%d-bit (Unsupported)", k_bpps[i]);
                lbl = ubuf;
            }
            uint32_t fg = !sup ? PXE_COL_DARK_GRAY :
                          (i == cp->sel_bpp ? PXE_COL_WHITE : PXE_COL_BLACK);
            pxe_draw_text_small(surf, dx + 6, iy + 4, lbl, fg);
        }
    }

    /* 10. Wallpaper File Path Prompt Modal */
    if (cp->wp_modal) {
        int dw = 280, dh = 96;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3, CP_TR(cp, "设置壁纸", "Set Wallpaper"), PXE_COL_WHITE);
        pxe_draw_text_small(surf, dx + 10, dy + 25, "BMP / PNG / JPG:", PXE_COL_BLACK);
        pxe_draw_bevel_box(surf, dx + 10, dy + 38, dw - 20, 20, 1, PXE_COL_WHITE);
        const char* vis = cp->wp_path;
        while (*vis && pxe_text_width_small(vis) > dw - 30) {
            (void)utf8_decode_next(&vis);
        }
        pxe_draw_text_small(surf, dx + 14, dy + 42, vis, PXE_COL_BLACK);
        pxe_draw_vline(surf, dx + 14 + pxe_text_width_small(vis), dy + 42, 12, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + 54, dy + 66, 72, 22, CP_TR(cp, "应用", "Apply"), 0, 1);
        pxe_draw_button(surf, dx + 154, dy + 66, 72, 22, CP_TR(cp, "取消", "Cancel"), 0, 1);
    }

    if (cp->restart_confirm) {
        int dw = 300, dh = 104;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3, CP_TR(cp, "需要重启", "Restart Required"), PXE_COL_WHITE);
        pxe_draw_text_small(surf, dx + 12, dy + 26,
                            CP_TR(cp, "所选显示模式需要重启生效。",
                                      "The selected display mode needs a restart."),
                            PXE_COL_BLACK);
        char mode_line[48];
        snprintf(mode_line, sizeof(mode_line),
                 cp->zh ? "%d x %d, %d位色深" : "%d x %d, %d-bit",
                 cp->pending_w, cp->pending_h, cp->pending_bpp);
        pxe_draw_text_small(surf, dx + 12, dy + 42, mode_line, PXE_COL_DARK_BLUE);
        pxe_draw_text_small(surf, dx + 12, dy + 56,
                            CP_TR(cp, "立即重启？取消将保持当前模式。",
                                      "Restart now? Cancel keeps the current mode."),
                            PXE_COL_DARK_GRAY);
        pxe_draw_button(surf, dx + 50, dy + 76, 86, 22, CP_TR(cp, "立即重启", "Restart"), 0, 1);
        pxe_draw_button(surf, dx + 164, dy + 76, 86, 22, CP_TR(cp, "取消", "Cancel"), 0, 1);
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
    cp->pending_reboot = 0;
    int mode_result = pxe_display_apply_mode(tw, th, tb);
    if (mode_result == 0) {
        pxe_rtc_time_t rtc;
        pxe_get_rtc_time(&rtc);
        cp->revert_sec = 15;
        cp->last_rtc_sec = (int)rtc.second;
        sync_from_sysinfo(cp);
        snprintf(cp->notice, sizeof(cp->notice), "Click Keep (OK) to confirm mode.");
    } else if (mode_result > 0) {
        cp->pending_reboot = 1;
        cp->pending_w = tw;
        cp->pending_h = th;
        cp->pending_bpp = tb;
        pxe_rtc_time_t rtc;
        pxe_get_rtc_time(&rtc);
        cp->revert_sec = 15;
        cp->last_rtc_sec = (int)rtc.second;
        snprintf(cp->notice, sizeof(cp->notice),
                 "Restart required. Click Keep to confirm.");
    } else {
        pxe_beep();
        snprintf(cp->notice, sizeof(cp->notice), "Display mode switch rejected.");
    }
}

static void discard_display_change(cpanel_t* cp) {
    if (!cp) return;
    cp->restart_confirm = 0;
    cp->pending_reboot = 0;
    cp->revert_sec = 0;
    if (cp->prev_w >= 640 && cp->prev_h >= 480) {
        (void)pxe_display_apply_mode(cp->prev_w, cp->prev_h, cp->prev_bpp);
    }
    sync_from_sysinfo(cp);
}

static void confirm_restart_mode(cpanel_t* cp) {
    if (!cp || !cp->pending_reboot) return;
    int width = cp->pending_w;
    int height = cp->pending_h;
    int bpp = cp->pending_bpp;
    cp->restart_confirm = 0;
    if (pxe_display_set_mode(width, height, bpp) != 0 ||
        pxe_system_reboot() != 0) {
        pxe_beep();
        discard_display_change(cp);
        snprintf(cp->notice, sizeof(cp->notice),
                 "Restart failed; current display mode was restored.");
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
    desc.h = 356;
    desc.min_w = 360;
    desc.min_h = 356;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_DEFAULT;
    strcpy(desc.title, pxe_is_zh() ? "控制面板" : "Control Panel");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) return 1;

    cpanel_t cp;
    memset(&cp, 0, sizeof(cp));
    sync_from_sysinfo(&cp);
    pxe_win_set_title(win_id, cp.zh ? "控制面板" : "Control Panel");
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
                if (cp.revert_sec > 0 || cp.pending_reboot ||
                    cp.restart_confirm) {
                    discard_display_change(&cp);
                }
                running = 0;
                break;
            } else if (ev.type == PXE_EV_REDRAW || ev.type == PXE_EV_ACTIVATE) {
                int old_zh = cp.zh;
                int old_w = cp.info.screen_w;
                int old_h = cp.info.screen_h;
                int old_bpp = cp.info.screen_bpp;
                int old_sel_res = cp.sel_res;
                int old_sel_bpp = cp.sel_bpp;
                int old_scroll = cp.res_scroll;
                sync_from_sysinfo(&cp);
                if (cp.zh != old_zh) {
                    pxe_win_set_title(win_id, cp.zh ? "控制面板" : "Control Panel");
                }
                if (cp.info.screen_w == old_w &&
                    cp.info.screen_h == old_h &&
                    cp.info.screen_bpp == old_bpp &&
                    old_sel_res >= 0 && old_sel_res < cp.caps.mode_count &&
                    mode_supported(&cp, old_sel_res, old_sel_bpp)) {
                    cp.sel_res = old_sel_res;
                    cp.sel_bpp = old_sel_bpp;
                    cp.res_scroll = old_scroll;
                    clamp_res_scroll(&cp);
                }
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
                    int vol = cpanel_vol_from_x(ev.x);
                    if (vol != cp.volume) {
                        cpanel_apply_volume(&cp, vol);
                        dirty = 1;
                    }
                } else if (cp.drag_res_sb && cp.res_open && cp.caps.mode_count > CPANEL_DROPDOWN_VIS) {
                    int vis = CPANEL_DROPDOWN_VIS;
                    int dh = vis * 18 + 2;
                    int track_h = (dh - 2) - 28;
                    int thumb_h = (track_h * vis) / cp.caps.mode_count;
                    if (thumb_h < 12) thumb_h = 12;
                    if (thumb_h > track_h) thumb_h = track_h;
                    int avail = track_h - thumb_h;
                    int max_sc = cp.caps.mode_count - vis;
                    if (avail > 0 && max_sc > 0) {
                        int dy_drag = ev.y - cp.res_sb_drag_y;
                        int half = (dy_drag >= 0) ? (avail / 2) : -(avail / 2);
                        int next_off = cp.res_sb_drag_off + (dy_drag * max_sc + half) / avail;
                        if (next_off < 0) next_off = 0;
                        if (next_off > max_sc) next_off = max_sc;
                        if (next_off != cp.res_scroll) {
                            cp.res_scroll = next_off;
                            dirty = 1;
                        }
                    }
                }
            } else if (ev.type == PXE_EV_MOUSE_UP) {
                if (cp.drag_mouse) {
                    cp.drag_mouse = 0;
                    pxe_settings_save();
                }
                if (cp.drag_vol) {
                    cp.drag_vol = 0;
                    pxe_settings_save();
                }
                cp.drag_res_sb = 0;
            } else if (ev.type == PXE_EV_KEY_DOWN && cp.restart_confirm) {
                if (ev.key == PXE_KEY_ENTER) {
                    confirm_restart_mode(&cp);
                } else if (ev.key == PXE_KEY_ESC) {
                    discard_display_change(&cp);
                    snprintf(cp.notice, sizeof(cp.notice),
                             "Restart cancelled; display mode unchanged.");
                }
                dirty = 1;
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
                        int d = utf8_prev_char_bytes(cp.wp_path, cp.wp_len);
                        cp.wp_len -= d;
                        cp.wp_path[cp.wp_len] = '\0';
                        dirty = 1;
                    }
                } else if (ev.key == PXE_KEY_UTF8 && ev.text[0]) {
                    int tl = (int)strlen(ev.text);
                    if (cp.wp_len + tl < 120) {
                        memcpy(cp.wp_path + cp.wp_len, ev.text, (size_t)tl);
                        cp.wp_len += tl;
                        cp.wp_path[cp.wp_len] = '\0';
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
                    cp.drag_res_sb = 0;
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
            } else if (ev.type == PXE_EV_MOUSE_WHEEL && ev.arg0 != 0) {
                if (cp.res_open && cp.caps.mode_count > 0) {
                    cp.res_scroll -= ev.arg0;
                    int max_scroll = cp.caps.mode_count -
                                     CPANEL_DROPDOWN_VIS;
                    if (max_scroll < 0) max_scroll = 0;
                    if (cp.res_scroll < 0) cp.res_scroll = 0;
                    if (cp.res_scroll > max_scroll) cp.res_scroll = max_scroll;
                    dirty = 1;
                } else if (pxe_point_in_rect(ev.x, ev.y, 124, 33, 104, 20)) {
                    int step = (ev.arg0 > 0) ? 5 : -5;
                    cpanel_apply_volume(&cp, cp.volume + step);
                    pxe_settings_save();
                    dirty = 1;
                } else if (pxe_point_in_rect(ev.x, ev.y, 126, 58, 204, 20) &&
                           cp.caps.mode_count > 0) {
                    cp.sel_res -= ev.arg0;
                    if (cp.sel_res < 0) cp.sel_res = 0;
                    if (cp.sel_res >= cp.caps.mode_count) {
                        cp.sel_res = cp.caps.mode_count - 1;
                    }
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_MOUSE_DOWN && (ev.buttons & PXE_BTN_LEFT)) {
                int mx = ev.x, my = ev.y;

                if (cp.restart_confirm) {
                    int dw = 300, dh = 104;
                    int dx = (surf.width - dw) / 2;
                    int dy = (surf.height - dh) / 2;
                    if (pxe_point_in_rect(mx, my, dx + 50, dy + 76, 86, 22)) {
                        confirm_restart_mode(&cp);
                    } else if (pxe_point_in_rect(mx, my,
                                                 dx + 164, dy + 76, 86, 22)) {
                        discard_display_change(&cp);
                        snprintf(cp.notice, sizeof(cp.notice),
                                 "Restart cancelled; display mode unchanged.");
                    }
                    dirty = 1;
                    continue;
                }

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
                    int sbx = dx + dw - 17;
                    int sby = dy + 1;
                    int sbw = 16;
                    int sbh = dh - 2;
                    if (has_sb && pxe_point_in_rect(mx, my, sbx - 1, sby, sbw + 1, sbh)) {
                        int max_sc = cp.caps.mode_count - CPANEL_DROPDOWN_VIS;
                        if (max_sc < 1) max_sc = 1;
                        int btn_h = 14;
                        int track_y = sby + btn_h;
                        int track_bot = sby + sbh - btn_h;
                        int track_h = track_bot - track_y;
                        if (my < track_y) {
                            if (cp.res_scroll > 0) cp.res_scroll--;
                        } else if (my >= track_bot) {
                            if (cp.res_scroll < max_sc) cp.res_scroll++;
                        } else {
                            int thumb_h = (track_h * vis) / cp.caps.mode_count;
                            if (thumb_h < 12) thumb_h = 12;
                            if (thumb_h > track_h) thumb_h = track_h;
                            int avail = track_h - thumb_h;
                            int thumb_y = track_y + ((avail > 0) ? (cp.res_scroll * avail) / max_sc : 0);
                            if (my >= thumb_y && my < thumb_y + thumb_h) {
                                cp.drag_res_sb = 1;
                                cp.res_sb_drag_y = my;
                                cp.res_sb_drag_off = cp.res_scroll;
                            } else if (my < thumb_y) {
                                cp.res_scroll -= (vis > 1) ? (vis - 1) : 1;
                                if (cp.res_scroll < 0) cp.res_scroll = 0;
                            } else {
                                cp.res_scroll += (vis > 1) ? (vis - 1) : 1;
                                if (cp.res_scroll > max_sc) cp.res_scroll = max_sc;
                            }
                        }
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
                    cp.drag_res_sb = 0;
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
                    int vol = cpanel_vol_from_x(mx);
                    cp.drag_vol = 1;
                    cpanel_apply_volume(&cp, vol);
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
                    if (pxe_point_in_rect(mx, my, 96, 140, 34, 20)) {
                        pxe_desktop_set_wallpaper_mode(0);
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice), "Wallpaper mode: Fill.");
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 132, 140, 32, 20)) {
                        pxe_desktop_set_wallpaper_mode(1);
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice), "Wallpaper mode: Fit.");
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 166, 140, 40, 20)) {
                        pxe_desktop_set_wallpaper_mode(2);
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice), "Wallpaper mode: Stretch.");
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 208, 140, 34, 20)) {
                        pxe_desktop_set_wallpaper_mode(3);
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice), "Wallpaper mode: Tile.");
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 244, 140, 40, 20)) {
                        pxe_desktop_set_wallpaper_mode(4);
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice), "Wallpaper mode: Center.");
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 286, 140, 30, 20)) {
                        pxe_desktop_clear_wallpaper();
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice), "Wallpaper cleared.");
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 318, 140, 34, 20)) {
                        cp.wp_modal = 1;
                        dirty = 1;
                    }
                } else if (pxe_point_in_rect(mx, my, 270, 140, 80, 20)) {
                    cp.wp_modal = 1;
                    dirty = 1;
                }

                /* Default appearance and clock options */
                if (pxe_point_in_rect(mx, my, 96, 168, 102, 20)) {
                    if (pxe_desktop_restore_default_wallpaper() == 0) {
                        cp.pattern = 0;
                        pxe_settings_save();
                        sync_from_sysinfo(&cp);
                        snprintf(cp.notice, sizeof(cp.notice),
                                 "Default wallpaper restored.");
                    } else {
                        pxe_beep();
                        snprintf(cp.notice, sizeof(cp.notice),
                                 "Failed to restore default wallpaper.");
                    }
                    dirty = 1;
                    continue;
                }
                if (pxe_point_in_rect(mx, my, 204, 168, 56, 20)) {
                    pxe_clock_set_options(!cp.info.clock_show_date,
                                          cp.info.clock_show_seconds);
                    pxe_settings_save();
                    sync_from_sysinfo(&cp);
                    dirty = 1;
                    continue;
                }
                if (pxe_point_in_rect(mx, my, 264, 168, 74, 20)) {
                    pxe_clock_set_options(cp.info.clock_show_date,
                                          !cp.info.clock_show_seconds);
                    pxe_settings_save();
                    sync_from_sysinfo(&cp);
                    dirty = 1;
                    continue;
                }

                /* Language selector (y=194) */
                if (pxe_point_in_rect(mx, my, 126, 194, 88, 20)) {
                    pxe_set_language(PXE_LANG_ZH);
                    pxe_settings_save();
                    sync_from_sysinfo(&cp);
                    pxe_win_set_title(win_id, "控制面板");
                    snprintf(cp.notice, sizeof(cp.notice), "界面语言已切换为简体中文。");
                    dirty = 1;
                    continue;
                }
                if (pxe_point_in_rect(mx, my, 222, 194, 88, 20)) {
                    pxe_set_language(PXE_LANG_EN);
                    pxe_settings_save();
                    sync_from_sysinfo(&cp);
                    pxe_win_set_title(win_id, "Control Panel");
                    snprintf(cp.notice, sizeof(cp.notice), "UI language set to English.");
                    dirty = 1;
                    continue;
                }

                /* Bottom actions */
                if (cp.revert_sec > 0) {
                    if (pxe_point_in_rect(mx, my, 188, 312, 156, 24)) {
                        cp.revert_sec = 0;
                        if (cp.pending_reboot) {
                            cp.restart_confirm = 1;
                        } else {
                            if (pxe_display_set_mode(cp.info.screen_w,
                                                     cp.info.screen_h,
                                                     cp.info.screen_bpp) == 0) {
                                pxe_settings_save();
                                sync_from_sysinfo(&cp);
                                snprintf(cp.notice, sizeof(cp.notice),
                                         CP_TR(&cp, "显示模式已确认保存。", "Display mode confirmed."));
                            } else {
                                pxe_beep();
                                snprintf(cp.notice, sizeof(cp.notice),
                                         CP_TR(&cp, "无法持久化保存显示模式。",
                                                    "Could not persist the display mode."));
                            }
                        }
                        dirty = 1;
                    }
                } else {
                    if (pxe_point_in_rect(mx, my, 10, 312, 108, 24)) {
                        start_apply_mode(&cp);
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 126, 312, 108, 24)) {
                        if (pxe_settings_save() == 0) {
                            snprintf(cp.notice, sizeof(cp.notice),
                                     CP_TR(&cp, "已保存设置到 .pse 文件。", "Saved settings to .pse."));
                        } else {
                            pxe_beep();
                            snprintf(cp.notice, sizeof(cp.notice),
                                     CP_TR(&cp, "保存失败 (当前介质只读?)。", "Save failed (read-only medium?)."));
                        }
                        dirty = 1;
                    } else if (pxe_point_in_rect(mx, my, 242, 312, 108, 24)) {
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
                    cp.pending_reboot = 0;
                    cp.restart_confirm = 0;
                    pxe_display_apply_mode(cp.prev_w, cp.prev_h, cp.prev_bpp);
                    sync_from_sysinfo(&cp);
                    snprintf(cp.notice, sizeof(cp.notice), "Reverted display mode.");
                }
                dirty = 1;
            }
        }

        if (running && !cp.drag_vol && sync_volume_from_sysinfo(&cp)) {
            dirty = 1;
        }

        if (dirty && running) {
            render_cpanel(&surf, &cp);
            pxe_win_present(win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(15);
        }
    }

    pxe_win_destroy(win_id);
    return 0;
}

