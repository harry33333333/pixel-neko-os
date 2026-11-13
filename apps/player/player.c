#include <pxe/abi.h>
#include <pxe/sys.h>
#include <pxe/gui.h>
#include <pxe/gfx.h>
#include <pxe/fs.h>
#include <pxe/audio.h>
#include <pxe/mem.h>
#include <pxe/str.h>

#define MAX_PLAYLIST 64

typedef struct {
    char name[128];
    char path[256];
} track_item_t;

typedef struct {
    pxe_audio_status_t st;
    char current_path[256];
    char display_title[128];
    int  marquee_pos;
    int  marquee_tick;
    int  blink_tick;

    int  drag_seek;
    int  drag_vol;
    int  seek_target_sec;
    int  pressed_btn; /* 0..6 or -1 */

    track_item_t playlist[MAX_PLAYLIST];
    int          track_count;
    int          track_sel;
    int          track_scroll;
    uint64_t     last_click_ticks;
    int          last_click_idx;

    int  open_modal;
    char open_input[256];
    int  open_len;
} player_t;

static int has_audio_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".wav") == 0 ||
           strcasecmp(dot, ".mp3") == 0 ||
           strcasecmp(dot, ".flac") == 0;
}

static void extract_dir(const char* path, char* out_dir, size_t cap) {
    strncpy(out_dir, path, cap - 1);
    out_dir[cap - 1] = '\0';
    size_t len = strlen(out_dir);
    while (len > 0 && out_dir[len - 1] != '\\' && out_dir[len - 1] != '/') {
        len--;
    }
    if (len == 0) {
        strncpy(out_dir, "C:\\", cap - 1);
        out_dir[cap - 1] = '\0';
        return;
    }
    if (len > 3 && (out_dir[len - 1] == '\\' || out_dir[len - 1] == '/')) {
        len--;
    }
    out_dir[len] = '\0';
}

static void scan_dir_tracks(player_t* pl, const char* dir_path) {
    pxe_dirent_t* ents = (pxe_dirent_t*)malloc(sizeof(pxe_dirent_t) * 128);
    if (!ents) return;
    int n = pxe_listdir(dir_path, ents, 128);
    if (n > 0) {
        size_t dlen = strlen(dir_path);
        const char* sep = (dlen > 0 && (dir_path[dlen - 1] == '\\' || dir_path[dlen - 1] == '/'))
                          ? "" : "\\";
        for (int i = 0; i < n && pl->track_count < MAX_PLAYLIST; i++) {
            if (ents[i].is_dir) continue;
            if (!has_audio_ext(ents[i].name)) continue;
            track_item_t* it = &pl->playlist[pl->track_count];
            strncpy(it->name, ents[i].name, sizeof(it->name) - 1);
            it->name[sizeof(it->name) - 1] = '\0';
            snprintf(it->path, sizeof(it->path), "%s%s%s", dir_path, sep, ents[i].name);
            if (pl->current_path[0] && strcasecmp(it->path, pl->current_path) == 0) {
                pl->track_sel = pl->track_count;
            }
            pl->track_count++;
        }
    }
    free(ents);
}

static void refresh_playlist_around(player_t* pl, const char* path_hint) {
    pl->track_count = 0;
    pl->track_sel = -1;
    pl->track_scroll = 0;

    if (path_hint && path_hint[0]) {
        char dir[256];
        extract_dir(path_hint, dir, sizeof(dir));
        scan_dir_tracks(pl, dir);
        return;
    }

    pxe_basic_sysinfo_t info;
    if (pxe_sys_get_basic_info(&info) == 0) {
        if (info.system_drive) {
            char root[8];
            snprintf(root, sizeof(root), "%c:\\", info.system_drive);
            scan_dir_tracks(pl, root);
        }
        if (info.cdrom_drive && info.cdrom_drive != info.system_drive) {
            char root[8];
            snprintf(root, sizeof(root), "%c:\\", info.cdrom_drive);
            scan_dir_tracks(pl, root);
        }
    }
}

static void play_track(player_t* pl, const char* path) {
    if (!path || !path[0]) return;
    pxe_dirent_t dst;
    if (pxe_stat(path, &dst) == 0 && dst.is_dir) {
        pl->track_count = 0;
        pl->track_sel = -1;
        pl->track_scroll = 0;
        scan_dir_tracks(pl, path);
        return;
    }

    strncpy(pl->current_path, path, sizeof(pl->current_path) - 1);
    pl->current_path[sizeof(pl->current_path) - 1] = '\0';

    const char* bn = path;
    for (const char* p = path; *p; p++) {
        if (*p == '\\' || *p == '/') bn = p + 1;
    }
    strncpy(pl->display_title, bn, sizeof(pl->display_title) - 1);
    pl->display_title[sizeof(pl->display_title) - 1] = '\0';
    pl->marquee_pos = 0;

    pxe_audio_play_file(path);
    pxe_audio_get_status(&pl->st);
    if (pl->st.title[0]) {
        strncpy(pl->display_title, pl->st.title, sizeof(pl->display_title) - 1);
        pl->display_title[sizeof(pl->display_title) - 1] = '\0';
    }
    refresh_playlist_around(pl, path);
}

static void play_delta_track(player_t* pl, int delta) {
    if (pl->track_count <= 0) {
        pxe_audio_seek(0);
        return;
    }
    if (delta < 0 && pl->st.position_sec > 3) {
        pxe_audio_seek(0);
        return;
    }
    int next = pl->track_sel + delta;
    if (next < 0) next = pl->track_count - 1;
    if (next >= pl->track_count) next = 0;
    pl->track_sel = next;
    play_track(pl, pl->playlist[next].path);
}

static void launch_files_browser(void) {
    pxe_basic_sysinfo_t info;
    memset(&info, 0, sizeof(info));
    pxe_sys_get_basic_info(&info);
    char path[64];
    pxe_dirent_t st;
    if (info.system_drive) {
        snprintf(path, sizeof(path), "%c:\\APPS\\files.pxe", info.system_drive);
        if (pxe_stat(path, &st) == 0) {
            pxe_spawn(path, 0, NULL);
            return;
        }
    }
    if (info.cdrom_drive) {
        snprintf(path, sizeof(path), "%c:\\APPS\\files.pxe", info.cdrom_drive);
        if (pxe_stat(path, &st) == 0) {
            pxe_spawn(path, 0, NULL);
        }
    }
}

static const int k_btn_x[7] = { 10, 50, 96, 142, 188, 228, 278 };
static const int k_btn_w[7] = { 36, 42, 42,  42,  36,  46,  54 };
static const char* k_btn_lbl[7] = { "|<<", ">", "||", "[]", ">>|", "Open", "Mute" };

static void render_player(const pxe_win_surface_t* surf, const player_t* pl) {
    /* Classic dithered dark platinum panel */
    for (int y = 0; y < surf->height; y++) {
        for (int x = 0; x < surf->width; x++) {
            pxe_put_pixel(surf, x, y, ((x + y) & 1) ? PXE_COL_LIGHT_GRAY : PXE_COL_DARK_GRAY);
        }
    }

    /* 1. Retro LCD panel */
    int lcd_x = 10, lcd_y = 8, lcd_w = surf->width - 20, lcd_h = 68;
    pxe_draw_bevel_box(surf, lcd_x, lcd_y, lcd_w, lcd_h, 1, PXE_COL_BLACK);

    if (!pl->st.has_hardware) {
        uint32_t wc = ((pl->blink_tick / 10) & 1) ? PXE_COL_RED : PXE_COL_DARK_RED;
        pxe_draw_text(surf, lcd_x + 12, lcd_y + 8, "[!] NO OUTPUT DEVICE", wc);
        pxe_draw_text(surf, lcd_x + 12, lcd_y + 26, "Supported PCI Audio Card", PXE_COL_YELLOW);
        pxe_draw_text_small(surf, lcd_x + 12, lcd_y + 48, "Hardware not detected on bus", PXE_COL_DARK_GRAY);
    } else {
        const char* tag = "[STOP]";
        uint32_t tcol = PXE_COL_DARK_GRAY;
        if (pl->st.state == 1) { tag = "[PLAY]"; tcol = PXE_COL_GREEN; }
        else if (pl->st.state == 2) { tag = "[PAUS]"; tcol = PXE_COL_YELLOW; }
        pxe_draw_text(surf, lcd_x + 6, lcd_y + 5, tag, tcol);

        int csec = (pl->drag_seek && pl->seek_target_sec >= 0)
                   ? pl->seek_target_sec : pl->st.position_sec;
        int dsec = pl->st.duration_sec;
        char tbuf[24];
        if (dsec > 0) {
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d / %02d:%02d",
                     csec / 60, csec % 60, dsec / 60, dsec % 60);
        } else {
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d / --:--", csec / 60, csec % 60);
        }
        pxe_draw_text(surf, lcd_x + 60, lcd_y + 5, tbuf, PXE_COL_GREEN);

        char dsp[24];
        if (pl->st.sample_rate > 0) {
            snprintf(dsp, sizeof(dsp), "%dkHz %db %dch",
                     pl->st.sample_rate / 1000, pl->st.bit_depth, pl->st.channels);
        } else {
            snprintf(dsp, sizeof(dsp), "44.1k/48k DSP");
        }
        pxe_draw_text_small(surf, lcd_x + 186, lcd_y + 9, dsp, PXE_COL_BLUE);

        const char* title = pl->display_title[0] ? pl->display_title : "NekoPlayer ready";
        int tlen = (int)strlen(title);
        int max_c = (lcd_w - 14) / 8;
        if (max_c < 10) max_c = 10;
        if (tlen <= max_c) {
            pxe_draw_text(surf, lcd_x + 6, lcd_y + 23, title, PXE_COL_WHITE);
        } else {
            char mbuf[64];
            if (max_c > (int)sizeof(mbuf) - 1) max_c = (int)sizeof(mbuf) - 1;
            for (int ci = 0; ci < max_c; ci++) {
                int idx = pl->marquee_pos + ci;
                if (idx < tlen) mbuf[ci] = title[idx];
                else if (idx < tlen + 4) mbuf[ci] = ' ';
                else {
                    int w = idx - (tlen + 4);
                    mbuf[ci] = (w < tlen) ? title[w] : ' ';
                }
            }
            mbuf[max_c] = '\0';
            pxe_draw_text(surf, lcd_x + 6, lcd_y + 23, mbuf, PXE_COL_WHITE);
        }

        /* 10-band Spectrum Analyzer */
        for (int bi = 0; bi < 10; bi++) {
            int bx = lcd_x + 6 + bi * 16;
            int val = pl->st.spectrum[bi];
            if (val > 15) val = 15;
            for (int seg = 0; seg < 15; seg++) {
                int sy = lcd_y + 62 - seg * 2;
                uint32_t col = (seg < val)
                    ? ((seg < 8) ? PXE_COL_DARK_GREEN : (seg < 12) ? PXE_COL_YELLOW : PXE_COL_RED)
                    : PXE_COL_DARK_GRAY;
                pxe_draw_hline(surf, bx, sy, 12, col);
            }
        }

        /* Stereo VU meters */
        pxe_draw_text_small(surf, lcd_x + 174, lcd_y + 43, "L", PXE_COL_GREEN);
        pxe_draw_text_small(surf, lcd_x + 174, lcd_y + 53, "R", PXE_COL_GREEN);
        int vu_w = lcd_w - 196;
        if (vu_w < 40) vu_w = 40;
        int vl = (pl->st.vu_left * vu_w) / 255;
        int vr = (pl->st.vu_right * vu_w) / 255;
        for (int vx = 0; vx < vu_w; vx++) {
            uint32_t cl = (vx < vl) ? ((vx > vu_w * 4 / 5) ? PXE_COL_RED :
                                       (vx > vu_w * 3 / 5) ? PXE_COL_YELLOW : PXE_COL_GREEN) : PXE_COL_DARK_GRAY;
            uint32_t cr = (vx < vr) ? ((vx > vu_w * 4 / 5) ? PXE_COL_RED :
                                       (vx > vu_w * 3 / 5) ? PXE_COL_YELLOW : PXE_COL_GREEN) : PXE_COL_DARK_GRAY;
            pxe_draw_vline(surf, lcd_x + 186 + vx, lcd_y + 44, 5, cl);
            pxe_draw_vline(surf, lcd_x + 186 + vx, lcd_y + 54, 5, cr);
        }
    }

    /* 2. Seek bar (y=84) */
    int sb_x = 10, sb_y = 84, sb_w = surf->width - 20, sb_h = 10;
    pxe_draw_bevel_box(surf, sb_x, sb_y, sb_w, sb_h, 1, PXE_COL_BLACK);
    int csec = (pl->drag_seek && pl->seek_target_sec >= 0) ? pl->seek_target_sec : pl->st.position_sec;
    int prog_w = 0;
    if (pl->st.duration_sec > 0) {
        prog_w = (csec * (sb_w - 8)) / pl->st.duration_sec;
        if (prog_w < 0) prog_w = 0;
        if (prog_w > sb_w - 8) prog_w = sb_w - 8;
    }
    if (prog_w > 0) {
        pxe_fill_rect(surf, sb_x + 2, sb_y + 2, prog_w, sb_h - 4, PXE_COL_BLUE);
    }
    pxe_draw_bevel_box(surf, sb_x + 2 + prog_w, sb_y - 2, 7, sb_h + 4, 0, PXE_COL_LIGHT_GRAY);

    /* 3. Transport buttons (y=104) */
    int dis = !pl->st.has_hardware;
    for (int i = 0; i < 7; i++) {
        int en = (i == 5) ? 1 : !dis;
        int prs = (pl->pressed_btn == i) || (i == 6 && pl->st.muted);
        pxe_draw_button(surf, k_btn_x[i], 104, k_btn_w[i], 22, k_btn_lbl[i], prs, en);
    }

    /* 4. Volume & Device info (y=134..168) */
    pxe_fill_rect(surf, 10, 134, 38, 16, PXE_COL_LIGHT_GRAY);
    pxe_draw_text(surf, 10, 134, "VOL:", PXE_COL_BLACK);
    pxe_draw_bevel_box(surf, 50, 137, 100, 10, 1, PXE_COL_BLACK);
    int vfill = (pl->st.volume * 96) / 100;
    if (vfill > 0) pxe_fill_rect(surf, 52, 139, vfill, 6, pl->st.muted ? PXE_COL_DARK_GRAY : PXE_COL_GREEN);
    char vtxt[16];
    snprintf(vtxt, sizeof(vtxt), "%d%%%s", pl->st.volume, pl->st.muted ? " [MUTE]" : "");
    pxe_fill_rect(surf, 156, 135, 92, 14, PXE_COL_LIGHT_GRAY);
    pxe_draw_text_small(surf, 158, 139, vtxt, PXE_COL_BLACK);

    pxe_fill_rect(surf, 10, 154, surf->width - 20, 18, PXE_COL_LIGHT_GRAY);
    char dev_str[80];
    if (pl->st.has_hardware) {
        snprintf(dev_str, sizeof(dev_str), "DEV: %s",
                 pl->st.hw_name[0] ? pl->st.hw_name : "Audio DSP");
    } else {
        snprintf(dev_str, sizeof(dev_str), "DEV: NO AUDIO HARDWARE");
    }
    int max_dc = (surf->width - 28) / 6;
    if (max_dc > 0 && (int)strlen(dev_str) > max_dc) dev_str[max_dc] = '\0';
    pxe_draw_text_small(surf, 14, 159, dev_str, pl->st.has_hardware ? PXE_COL_BLACK : PXE_COL_DARK_RED);

    /* 5. Playlist panel (y=176..height-8) */
    int py = 176;
    int ph = surf->height - py - 8;
    if (ph >= 28) {
        pxe_draw_bevel_box(surf, 10, py, surf->width - 20, ph, 1, PXE_COL_WHITE);
        int sbw = 14;
        int list_w = surf->width - 24 - sbw;
        int vis = (ph - 4) / 14;
        if (vis < 1) vis = 1;
        if (pl->track_count == 0) {
            pxe_draw_text_small(surf, 18, py + 8,
                                "No .wav/.mp3/.flac in playlist (click Open).", PXE_COL_DARK_GRAY);
        } else {
            for (int r = 0; r < vis; r++) {
                int idx = pl->track_scroll + r;
                if (idx >= pl->track_count) break;
                int ry = py + 2 + r * 14;
                int sel = (idx == pl->track_sel);
                if (sel) {
                    pxe_fill_rect(surf, 12, ry, list_w, 14, PXE_COL_BLUE);
                }
                char line[64];
                snprintf(line, sizeof(line), "%02d. %s", idx + 1, pl->playlist[idx].name);
                int max_lc = (list_w - 8) / 6;
                if (max_lc > 0 && (int)strlen(line) > max_lc) line[max_lc] = '\0';
                pxe_draw_text_small(surf, 16, ry + 3, line, sel ? PXE_COL_WHITE : PXE_COL_BLACK);
            }
        }
        pxe_draw_scrollbar(surf, surf->width - 12 - sbw, py + 2, sbw, ph - 4,
                           pl->track_scroll, vis, pl->track_count);
    }

    /* 6. Open modal */
    if (pl->open_modal) {
        int dw = 290, dh = 96;
        int dx = (surf->width - dw) / 2;
        int dy = (surf->height - dh) / 2;
        pxe_draw_bevel_box(surf, dx, dy, dw, dh, 0, PXE_COL_LIGHT_GRAY);
        pxe_fill_rect(surf, dx + 2, dy + 2, dw - 4, 18, PXE_COL_DARK_BLUE);
        pxe_draw_text(surf, dx + 8, dy + 3, "Open Audio / Folder", PXE_COL_WHITE);
        pxe_draw_bevel_box(surf, dx + 10, dy + 30, dw - 20, 22, 1, PXE_COL_WHITE);
        int max_c = (dw - 28) / 6;
        int start = (pl->open_len > max_c) ? (pl->open_len - max_c) : 0;
        pxe_draw_text_small(surf, dx + 14, dy + 38, pl->open_input + start, PXE_COL_BLACK);
        pxe_draw_vline(surf, dx + 14 + (pl->open_len - start) * 6, dy + 34, 14, PXE_COL_BLACK);
        pxe_draw_button(surf, dx + 14,  dy + 62, 76, 22, "Play", 0, 1);
        pxe_draw_button(surf, dx + 100, dy + 62, 92, 22, "Files...", 0, 1);
        pxe_draw_button(surf, dx + 202, dy + 62, 74, 22, "Cancel", 0, 1);
    }
}

int main(int argc, char** argv) {
    player_t* pl = (player_t*)calloc(1, sizeof(player_t));
    if (!pl) return 1;
    pl->pressed_btn = -1;
    pl->seek_target_sec = -1;
    pl->track_sel = -1;
    pl->last_click_idx = -1;

    pxe_win_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.x = 130;
    desc.y = 56;
    desc.w = 342;
    desc.h = 292;
    desc.min_w = 342;
    desc.min_h = 212;
    desc.format = PXE_CANVAS_PAL8;
    desc.flags = PXE_WIN_RESIZABLE;
    strcpy(desc.title, "NekoPlayer");

    pxe_win_surface_t surf;
    int win_id = pxe_win_create(&desc, &surf);
    if (win_id < 0) {
        free(pl);
        return 1;
    }

    pxe_audio_get_status(&pl->st);
    if (!pl->st.has_hardware) {
        strcpy(pl->display_title, "No Output Device");
    } else {
        strcpy(pl->display_title, "NekoPlayer ready. Open a file.");
    }

    if (argc > 1 && argv[1] && argv[1][0]) {
        play_track(pl, argv[1]);
    } else {
        refresh_playlist_around(pl, NULL);
    }

    snprintf(pl->open_input, sizeof(pl->open_input), "C:\\");
    pl->open_len = 3;

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
                if (pxe_win_get_surface(win_id, &surf) == 0) dirty = 1;
            } else if (ev.type == PXE_EV_REDRAW) {
                dirty = 1;
            } else if (ev.type == PXE_EV_ACTIVATE) {
                if (ev.text[0]) play_track(pl, ev.text);
                dirty = 1;
            } else if (ev.type == PXE_EV_KEY_DOWN) {
                if (pl->open_modal) {
                    if (ev.key == PXE_KEY_ESC) {
                        pl->open_modal = 0;
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_ENTER) {
                        pl->open_modal = 0;
                        if (pl->open_len > 0) play_track(pl, pl->open_input);
                        dirty = 1;
                    } else if (ev.key == PXE_KEY_BACKSPACE) {
                        if (pl->open_len > 0) {
                            pl->open_input[--pl->open_len] = '\0';
                            dirty = 1;
                        }
                    } else if (ev.key >= 32 && ev.key <= 126 && pl->open_len < 250) {
                        pl->open_input[pl->open_len++] = (char)ev.key;
                        pl->open_input[pl->open_len] = '\0';
                        dirty = 1;
                    }
                } else {
                    if (ev.key == ' ' && pl->st.has_hardware) {
                        if (pl->st.state == 1) pxe_audio_pause();
                        else if (pl->st.state == 2) pxe_audio_resume();
                        else if (pl->current_path[0]) play_track(pl, pl->current_path);
                        dirty = 1;
                    } else if (ev.key == 'm' || ev.key == 'M') {
                        if (pl->st.has_hardware) { pxe_audio_toggle_mute(); dirty = 1; }
                    } else if (ev.key == PXE_KEY_ESC) {
                        running = 0;
                    }
                }
            } else if (ev.type == PXE_EV_MOUSE_MOVE && (ev.buttons & PXE_BTN_LEFT)) {
                if (pl->drag_seek && pl->st.duration_sec > 0) {
                    int sb_w = surf.width - 20;
                    int ox = ev.x - 10;
                    if (ox < 0) ox = 0;
                    if (ox > sb_w) ox = sb_w;
                    pl->seek_target_sec = (ox * pl->st.duration_sec) / sb_w;
                    dirty = 1;
                } else if (pl->drag_vol) {
                    int vx = ev.x - 50;
                    if (vx < 0) vx = 0;
                    if (vx > 100) vx = 100;
                    pxe_audio_set_volume(vx);
                    pxe_audio_get_status(&pl->st);
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_MOUSE_UP) {
                if (pl->drag_seek) {
                    if (pl->seek_target_sec >= 0) {
                        pxe_audio_seek((uint32_t)pl->seek_target_sec);
                    }
                    pl->drag_seek = 0;
                    pl->seek_target_sec = -1;
                    dirty = 1;
                }
                pl->drag_vol = 0;
                if (pl->pressed_btn >= 0) {
                    int bi = pl->pressed_btn;
                    pl->pressed_btn = -1;
                    if (pxe_point_in_rect(ev.x, ev.y, k_btn_x[bi], 104, k_btn_w[bi], 22)) {
                        if (bi == 0 && pl->st.has_hardware) play_delta_track(pl, -1);
                        else if (bi == 1 && pl->st.has_hardware) {
                            if (pl->st.state == 2) pxe_audio_resume();
                            else if (pl->current_path[0]) play_track(pl, pl->current_path);
                            else if (pl->track_count > 0) {
                                int idx = (pl->track_sel >= 0) ? pl->track_sel : 0;
                                play_track(pl, pl->playlist[idx].path);
                            }
                        } else if (bi == 2 && pl->st.has_hardware) {
                            if (pl->st.state == 1) pxe_audio_pause();
                            else if (pl->st.state == 2) pxe_audio_resume();
                        } else if (bi == 3 && pl->st.has_hardware) {
                            pxe_audio_stop();
                        } else if (bi == 4 && pl->st.has_hardware) {
                            play_delta_track(pl, 1);
                        } else if (bi == 5) {
                            pl->open_modal = 1;
                        } else if (bi == 6 && pl->st.has_hardware) {
                            pxe_audio_toggle_mute();
                        }
                    }
                    dirty = 1;
                }
            } else if (ev.type == PXE_EV_MOUSE_DOWN && (ev.buttons & PXE_BTN_LEFT)) {
                int mx = ev.x, my = ev.y;
                if (pl->open_modal) {
                    int dw = 290, dh = 96;
                    int dx = (surf.width - dw) / 2;
                    int dy = (surf.height - dh) / 2;
                    if (pxe_point_in_rect(mx, my, dx + 14, dy + 62, 76, 22)) {
                        pl->open_modal = 0;
                        if (pl->open_len > 0) play_track(pl, pl->open_input);
                    } else if (pxe_point_in_rect(mx, my, dx + 100, dy + 62, 92, 22)) {
                        pl->open_modal = 0;
                        launch_files_browser();
                    } else if (pxe_point_in_rect(mx, my, dx + 202, dy + 62, 74, 22)) {
                        pl->open_modal = 0;
                    }
                    dirty = 1;
                    continue;
                }

                /* Seek bar */
                if (pxe_point_in_rect(mx, my, 10, 82, surf.width - 20, 14) && pl->st.has_hardware) {
                    if (pl->st.duration_sec > 0) {
                        int sb_w = surf.width - 20;
                        int ox = mx - 10;
                        if (ox < 0) ox = 0;
                        if (ox > sb_w) ox = sb_w;
                        pl->seek_target_sec = (ox * pl->st.duration_sec) / sb_w;
                    }
                    pl->drag_seek = 1;
                    dirty = 1;
                    continue;
                }

                /* Volume slider */
                if (pxe_point_in_rect(mx, my, 50, 134, 100, 16)) {
                    int vx = mx - 50;
                    if (vx < 0) vx = 0;
                    if (vx > 100) vx = 100;
                    pxe_audio_set_volume(vx);
                    pl->drag_vol = 1;
                    dirty = 1;
                    continue;
                }

                /* Transport buttons */
                int hit_btn = -1;
                for (int i = 0; i < 7; i++) {
                    if (pxe_point_in_rect(mx, my, k_btn_x[i], 104, k_btn_w[i], 22)) {
                        if (i == 5 || pl->st.has_hardware) hit_btn = i;
                        break;
                    }
                }
                if (hit_btn >= 0) {
                    pl->pressed_btn = hit_btn;
                    dirty = 1;
                    continue;
                }

                /* Playlist */
                int py = 176, ph = surf.height - py - 8;
                if (ph >= 28 && pxe_point_in_rect(mx, my, 10, py, surf.width - 20, ph)) {
                    int sbw = 14;
                    int vis = (ph - 4) / 14;
                    if (vis < 1) vis = 1;
                    int max_scr = (pl->track_count > vis) ? (pl->track_count - vis) : 0;
                    if (mx >= surf.width - 12 - sbw) {
                        if (my < py + 16) {
                            if (pl->track_scroll > 0) pl->track_scroll--;
                        } else if (my >= py + ph - 16) {
                            if (pl->track_scroll < max_scr) pl->track_scroll++;
                        }
                    } else if (pl->track_count > 0) {
                        int idx = pl->track_scroll + (my - (py + 2)) / 14;
                        if (idx >= 0 && idx < pl->track_count) {
                            uint64_t now = pxe_get_ticks();
                            if (idx == pl->last_click_idx && (now - pl->last_click_ticks) < 40) {
                                pl->track_sel = idx;
                                play_track(pl, pl->playlist[idx].path);
                                pl->last_click_idx = -1;
                            } else {
                                pl->track_sel = idx;
                                pl->last_click_idx = idx;
                                pl->last_click_ticks = now;
                            }
                        }
                    }
                    dirty = 1;
                }
            }
        }
        if (rc < 0) break;

        /* Periodic telemetry update for spectrum / VU / marquee / clock */
        pxe_audio_status_t cur;
        if (pxe_audio_get_status(&cur) == 0) {
            if (cur.state != pl->st.state ||
                cur.position_sec != pl->st.position_sec ||
                cur.volume != pl->st.volume ||
                cur.muted != pl->st.muted ||
                cur.has_hardware != pl->st.has_hardware ||
                cur.state == 1) {
                pl->st = cur;
                dirty = 1;
            }
        }

        if (!pl->st.has_hardware) {
            pl->blink_tick++;
            if ((pl->blink_tick % 10) == 0) dirty = 1;
        } else if (pl->st.state == 1) {
            int tlen = (int)strlen(pl->display_title);
            if (tlen > 28) {
                pl->marquee_tick++;
                if ((pl->marquee_tick % 3) == 0) {
                    pl->marquee_pos++;
                    if (pl->marquee_pos >= tlen + 4) pl->marquee_pos = 0;
                    dirty = 1;
                }
            }
        }

        if (dirty && running) {
            render_player(&surf, pl);
            pxe_win_present(win_id);
            dirty = 0;
        } else if (running) {
            pxe_sleep_ms(50);
        }
    }

    pxe_win_destroy(win_id);
    free(pl);
    return 0;
}
