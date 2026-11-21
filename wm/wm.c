// wm.c - 完整版（含文件管理器）
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "wm.h"
static int in_cl(int mx, int my, int cx, int cy);
static int in_mi(int mx, int my, int cx, int cy);
static int rs_dir(int mx, int my, int wx, int wy, int ww, int wh);
#include "font.h"
#include "version.h"
#include "port.h"
#include "audio.h"
#include "wallpaper.h"
#include "speaker.h"
#include "input.h"
#include "keyboard.h"
#include "mm.h"
#include "pmm.h"
#include "vmm.h"
#include "process.h"
#include "scheduler.h"
#include "neko_abi.h"
#include "pxe.h"
#include "string.h"
#include "power.h"
#include "vfs.h"
#include "cpu.h"
#include "ui_compat.h"
#include "part.h"
#include "desktop.h"
#include "settings.h"

extern int total_ram_mb;
extern int usable_ram_mb;

int redraw = 0;
static int need_mr = 0;

// Opened message for visual confirmation when an app is opened
static char opened_msg[48] = "";
static int opened_msg_timer = 0; // ticks

static void show_opened(const char* name)
{
    int i=0;
    while(name[i] && i < (int)sizeof(opened_msg)-1) { opened_msg[i] = name[i]; i++; }
    opened_msg[i] = 0;
    opened_msg_timer = 120; // show for ~120 ticks
    redraw = 1; need_mr = 1;
}

static int icon_dragging = -1;
static int icon_drag_ox = 0, icon_drag_oy = 0;
static int icon_drag_active = 0, icon_drag_bx = 0, icon_drag_by = 0;
static int icon_pressed = -1;
static int icon_press_ox = 0, icon_press_oy = 0;
static int icon_press_moved = 0;
static int menu_open = 0, menu_index = -1;
extern volatile int global_tick;
static int input_event_tick = -1;
static int last_click_icon = -1, last_s = -1;
static int desktop_menu_mode;
static int desktop_menu_item = -1;
static int desktop_menu_x;
static int desktop_menu_y;
static void draw_desktop_context_menu(unsigned char* v);
// Track previous click release for double-click timing (separate from visual selection)
static int last_click_icon_prev = -1, last_click_time_prev = 0;

#define WM_LEGACY_COUNT 0
#define WM_DYN_MAX      32
#define WM_DYN_BASE     100
#define WM_Z_COUNT      (WM_LEGACY_COUNT + WM_DYN_MAX)
#define WM_EV_QUEUE_CAP 64

typedef struct {
    bool            used;
    int             win_id;
    uint32_t        owner_pid;
    int             x, y;
    int             client_w;
    int             client_h;
    int             min_w;
    int             min_h;
    int             alloc_stride;
    int             alloc_h;
    int             pitch;
    uint32_t        format;
    uint32_t        flags;
    int             shaded;
    char            title[48];
    phys_addr_t     canvas_phys;
    size_t          canvas_pages;
    void*           canvas_kvirt;
    uint64_t        canvas_uvirt;
    bool            has_menu;
    pxe_menu_bar_t  menu_bar;
    bool            present_dirty;
    int             dirty_rx;
    int             dirty_ry;
    int             dirty_rw;
    int             dirty_rh;
    pxe_event_t     ev_queue[WM_EV_QUEUE_CAP];
    int             ev_head;
    int             ev_tail;
    int             ev_count;
} wm_dyn_win_t;

static wm_dyn_win_t g_dyn_wins[WM_DYN_MAX];
static volatile int g_pxe_present_dirty = 0;
static volatile int g_pxe_layout_dirty = 0;

#define WM_DIRTY_RECT_CAP 32
typedef struct {
    int x, y, w, h;
} wm_dirty_rect_t;
static wm_dirty_rect_t g_dirty_rects[WM_DIRTY_RECT_CAP];
static int g_dirty_rect_count = 0;

static void wm_push_dirty_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;
    if (w <= 0 || h <= 0) return;
    if (g_dirty_rect_count < WM_DIRTY_RECT_CAP) {
        g_dirty_rects[g_dirty_rect_count].x = x;
        g_dirty_rects[g_dirty_rect_count].y = y;
        g_dirty_rects[g_dirty_rect_count].w = w;
        g_dirty_rects[g_dirty_rect_count].h = h;
        g_dirty_rect_count++;
    } else {
        redraw = 1;
        need_mr = 1;
    }
}

int wm_pop_dirty_rect(int* x, int* y, int* w, int* h) {
    if (g_dirty_rect_count <= 0) return 0;
    g_dirty_rect_count--;
    if (x) *x = g_dirty_rects[g_dirty_rect_count].x;
    if (y) *y = g_dirty_rects[g_dirty_rect_count].y;
    if (w) *w = g_dirty_rects[g_dirty_rect_count].w;
    if (h) *h = g_dirty_rects[g_dirty_rect_count].h;
    return 1;
}
static int g_pxe_capture_win = -1;
static int g_last_pxe_mx = -1;
static int g_last_pxe_my = -1;

/* Crash Bomb Dialog State */
static volatile int crash_dialog_active = 0;
static int crash_ok_pressed = 0;
static uint32_t crash_pid = 0;
static char crash_app_name[32];
static char crash_exc_name[40];
static uint64_t crash_rip = 0;
static uint64_t crash_cr2 = 0;

/* Ring 0 Execution Confirmation Dialog State */
static volatile int r0_confirm_active = 0;
static int r0_cancel_pressed = 0;
static int r0_run_pressed = 0;
static char r0_confirm_path[128];
static char r0_confirm_arg1[128];
static int r0_confirm_has_arg1 = 0;

static int z_order[WM_Z_COUNT] = {
    100, 101, 102, 103, 104, 105, 106, 107,
    108, 109, 110, 111, 112, 113, 114, 115,
    116, 117, 118, 119, 120, 121, 122, 123,
    124, 125, 126, 127, 128, 129, 130, 131
};

static void get_win_geom(int id, int* rx, int* ry, int* rw, int* rh, int* vis, int* min);

void bring_to_top(int id) {
    int i;
    for (i = 0; i < WM_Z_COUNT; i++) {
        if (z_order[i] == id) break;
    }
    if (i < WM_Z_COUNT) {
        for (int j = i; j < WM_Z_COUNT - 1; j++) z_order[j] = z_order[j + 1];
        z_order[WM_Z_COUNT - 1] = id;
    }
}

static int top_visible_win_id(void) {
    for (int i = WM_Z_COUNT - 1; i >= 0; i--) {
        int id = z_order[i];
        int rx = 0, ry = 0, rw = 0, rh = 0, vis = 0, min = 0;
        get_win_geom(id, &rx, &ry, &rw, &rh, &vis, &min);
        if (vis) return id;
    }
    return -1;
}

static int dragging=0, dgo=0, dgoy=0, dgw=0;
static int drag_active=0, drag_bx=0, drag_by=0, drag_bw=0, drag_bh=0;
static int clp=0, clcx=0, clcy=0, clw=0;
static int mip=0, miw=0;

static int resizing=0, rs_win_id=-1;
static int rs_active=0, rs_bx=0, rs_by=0, rs_bw=0, rs_bh=0;
static int power_confirm=0;
static int power_cancel_pressed=0, power_ok_pressed=0;
static int volume_popup_open=0;
static int volume_drag_active=0;
static int uninit_dialog_active=0;
static int uninit_dialog_checked=0;
static int uninit_ignore_pressed=0, uninit_init_pressed=0;

static void icon(unsigned char* v, int x, int y, int desktop_index,
                 unsigned char bg, const char* lb, int sel, bool shortcut);

static unsigned char cmos_read(unsigned char a){outb(0x70,a);return inb(0x71);}
static int bcd_to_int(unsigned char b){return(b>>4)*10+(b&0x0F);}
static void get_time(int*h,int*m,int*s){
    int timeout = 10000;
    while ((cmos_read(0x0A) & 0x80) && --timeout);
    unsigned char sec = cmos_read(0x00);
    unsigned char min = cmos_read(0x02);
    unsigned char hr  = cmos_read(0x04);
    unsigned char regb = cmos_read(0x0B);
    if (!(regb & 0x04)) {
        *h = bcd_to_int(hr);
        *m = bcd_to_int(min);
        *s = bcd_to_int(sec);
    } else {
        *h = hr;
        *m = min;
        *s = sec;
    }
}

static void get_date(int* year,int* month,int* day){
    int timeout = 10000;
    while ((cmos_read(0x0A) & 0x80) && --timeout);
    unsigned char day_raw = cmos_read(0x07);
    unsigned char month_raw = cmos_read(0x08);
    unsigned char year_raw = cmos_read(0x09);
    unsigned char regb = cmos_read(0x0B);
    if (!(regb & 0x04)) {
        *day = bcd_to_int(day_raw);
        *month = bcd_to_int(month_raw);
        *year = 2000 + bcd_to_int(year_raw);
    } else {
        *day = day_raw;
        *month = month_raw;
        *year = 2000 + year_raw;
    }
}

static void put_two_digits(char* out, int value) {
    out[0] = (char)('0' + (value / 10) % 10);
    out[1] = (char)('0' + value % 10);
}

static int format_clock_text(char* out, int capacity) {
    if (!out || capacity < 6) return 0;
    out[0] = '\0';
    int hour = 0, minute = 0, second = 0;
    int year = 0, month = 0, day = 0;
    get_time(&hour, &minute, &second);
    int used = 0;
    if (settings_get_clock_show_date()) {
        get_date(&year, &month, &day);
        if (capacity < used + 11) {
            out[used] = '\0';
            return used;
        }
        char year_text[5];
        year_text[0] = (char)('0' + (year / 1000) % 10);
        year_text[1] = (char)('0' + (year / 100) % 10);
        year_text[2] = (char)('0' + (year / 10) % 10);
        year_text[3] = (char)('0' + year % 10);
        year_text[4] = '\0';
        for (int i = 0; i < 4; i++) out[used++] = year_text[i];
        out[used++] = '-';
        put_two_digits(out + used, month);
        used += 2;
        out[used++] = '-';
        put_two_digits(out + used, day);
        used += 2;
        out[used++] = ' ';
    }
    put_two_digits(out + used, hour);
    used += 2;
    out[used++] = ':';
    put_two_digits(out + used, minute);
    used += 2;
    if (settings_get_clock_show_seconds() && used + 3 <= capacity - 1) {
        out[used++] = ':';
        put_two_digits(out + used, second);
        used += 2;
    }
    out[used] = '\0';
    return used;
}

static void notification_volume_span(int* out_x, int* out_width) {
    char clock_text[32];
    int clock_length = format_clock_text(clock_text, sizeof(clock_text));
    int clock_x = screen_w - clock_length * 8 - 10;
    if (clock_x < 24) clock_x = 24;
    int x = clock_x - 28;
    if (x < 18) x = 18;
    if (out_x) *out_x = x;
    if (out_width) *out_width = 24;
}

static void notification_volume_panel(int* out_x, int* out_y,
                                      int* out_width, int* out_height) {
    int icon_x = 0, icon_width = 0;
    notification_volume_span(&icon_x, &icon_width);
    int x = icon_x - 16;
    if (x + 56 > screen_w) x = screen_w - 56;
    if (x < 2) x = 2;
    if (out_x) *out_x = x;
    if (out_y) *out_y = 22;
    if (out_width) *out_width = 56;
    if (out_height) *out_height = 144;
}

static void draw_volume_icon(unsigned char* v, int x, int y) {
    int muted = audio_is_muted();
    int volume = audio_get_volume();
    for (int row = 0; row < 5; row++) {
        for (int column = 0; column <= row; column++) {
            PUT_PIXEL(v, x + 2 + column, y + 7 + row, 0);
            PUT_PIXEL(v, x + 2 + column, y + 7 - row, 0);
        }
    }
    for (int row = 3; row < 14; row++) {
        PUT_PIXEL(v, x + 5, y + row, 0);
        PUT_PIXEL(v, x + 6, y + row, 0);
    }
    if (!muted && volume > 0) {
        PUT_PIXEL(v, x + 9, y + 7, 0);
        PUT_PIXEL(v, x + 10, y + 8, 0);
        PUT_PIXEL(v, x + 10, y + 9, 0);
        PUT_PIXEL(v, x + 9, y + 10, 0);
    }
    if (!muted && volume > 35) {
        PUT_PIXEL(v, x + 12, y + 5, 0);
        PUT_PIXEL(v, x + 13, y + 6, 0);
        PUT_PIXEL(v, x + 14, y + 7, 0);
        PUT_PIXEL(v, x + 14, y + 8, 0);
        PUT_PIXEL(v, x + 14, y + 9, 0);
        PUT_PIXEL(v, x + 13, y + 10, 0);
        PUT_PIXEL(v, x + 12, y + 11, 0);
    }
    if (muted) {
        for (int offset = 0; offset < 8; offset++) {
            PUT_PIXEL(v, x + 9 + offset, y + 6 + offset, 4);
            PUT_PIXEL(v, x + 16 - offset, y + 6 + offset, 4);
        }
    }
}

static void wm_sync_volume_state(void);

static void volume_set_from_y(int y) {
    int panel_x = 0, panel_y = 0, panel_w = 0, panel_h = 0;
    notification_volume_panel(&panel_x, &panel_y, &panel_w, &panel_h);
    (void)panel_x;
    (void)panel_w;
    (void)panel_h;
    int track_top = panel_y + 28;
    int track_height = 78;
    int span = track_height - 1;
    int offset = track_top + track_height - 1 - y;
    if (offset < 0) offset = 0;
    if (offset > span) offset = span;
    int volume = (offset * 100 + span / 2) / span;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    audio_set_volume(volume);
    wm_sync_volume_state();
    redraw = 1;
    need_mr = 1;
}

static void draw_volume_popup(unsigned char* v) {
    if (!volume_popup_open) return;
    int x = 0, y = 0, w = 0, h = 0;
    notification_volume_panel(&x, &y, &w, &h);
    for (int py = y; py < y + h && py < screen_h; py++) {
        for (int px = x; px < x + w && px < screen_w; px++) {
            PUT_PIXEL(v, px, py, 15);
        }
    }
    for (int px = x; px < x + w; px++) {
        PUT_PIXEL(v, px, y, 0);
        PUT_PIXEL(v, px, y + h - 1, 0);
    }
    for (int py = y; py < y + h; py++) {
        PUT_PIXEL(v, x, py, 0);
        PUT_PIXEL(v, x + w - 1, py, 0);
    }

    char volume_text[12];
    int volume = audio_get_volume();
    if (audio_is_muted()) {
        volume_text[0] = 'M';
        volume_text[1] = 'U';
        volume_text[2] = 'T';
        volume_text[3] = 'E';
        volume_text[4] = '\0';
    } else if (volume == 100) {
        volume_text[0] = '1';
        volume_text[1] = '0';
        volume_text[2] = '0';
        volume_text[3] = '%';
        volume_text[4] = '\0';
    } else {
        volume_text[0] = (char)('0' + volume / 10);
        volume_text[1] = (char)('0' + volume % 10);
        volume_text[2] = '%';
        volume_text[3] = '\0';
    }
    font_draw_text(v, x + 13, y + 6, volume_text, 0);

    int track_x = x + 24;
    int track_y = y + 28;
    int track_w = 8;
    int track_h = 78;
    for (int py = track_y; py < track_y + track_h; py++) {
        for (int px = track_x; px < track_x + track_w; px++) {
            PUT_PIXEL(v, px, py, 8);
        }
    }
    int eff_vol = audio_is_muted() ? 0 : volume;
    int thumb_y = track_y + track_h - 1 -
                  (eff_vol * (track_h - 1) + 50) / 100;
    if (eff_vol > 0) {
        int fill_top = thumb_y;
        if (fill_top < track_y + 1) fill_top = track_y + 1;
        for (int py = fill_top; py < track_y + track_h - 1; py++) {
            for (int px = track_x + 1; px < track_x + track_w - 1; px++) {
                PUT_PIXEL(v, px, py, 1);
            }
        }
    }
    for (int py = thumb_y - 4; py <= thumb_y + 4; py++) {
        for (int px = track_x - 5; px < track_x + track_w + 5; px++) {
            if (py >= track_y && py < track_y + track_h) {
                PUT_PIXEL(v, px, py, (py == thumb_y || px == track_x - 5 ||
                                      px == track_x + track_w + 4) ? 0 : 7);
            }
        }
    }

    int button_x = x + 5;
    int button_y = y + 114;
    for (int py = button_y; py < button_y + 22; py++) {
        for (int px = button_x; px < button_x + 46; px++) {
            PUT_PIXEL(v, px, py,
                      audio_is_muted() ? 1 : 7);
        }
    }
    for (int px = button_x; px < button_x + 46; px++) {
        PUT_PIXEL(v, px, button_y, 0);
        PUT_PIXEL(v, px, button_y + 21, 0);
    }
    for (int py = button_y; py < button_y + 22; py++) {
        PUT_PIXEL(v, button_x, py, 0);
        PUT_PIXEL(v, button_x + 45, py, 0);
    }
    font_draw_text(v, button_x + 7, button_y + 3, "Mute",
                   audio_is_muted() ? 15 : 0);
}

static void cl_btn(unsigned char* v, int cx, int cy, int p)
{
    for(int y=cy;y<cy+11;y++)for(int x=cx;x<cx+11;x++)PUT_PIXEL(v, x, y, 15);
    for(int i=0;i<11;i++){ PUT_PIXEL(v, cx+i, cy, 0); PUT_PIXEL(v, cx+i, cy+10, 0); PUT_PIXEL(v, cx, cy+i, 0); PUT_PIXEL(v, cx+10, cy+i, 0); }
    if(p){
        for(int y=cy+1;y<cy+10;y++)for(int x=cx+1;x<cx+10;x++)PUT_PIXEL(v, x, y, 0);
    }else{
        for(int i=3;i<8;i++){ PUT_PIXEL(v, cx+i, cy+3, 0); PUT_PIXEL(v, cx+i, cy+7, 0); PUT_PIXEL(v, cx+3, cy+i, 0); PUT_PIXEL(v, cx+7, cy+i, 0); }
    }
}

static void mi_btn(unsigned char* v, int cx, int cy, int p)
{
    for(int y=cy;y<cy+11;y++)for(int x=cx;x<cx+11;x++)PUT_PIXEL(v, x, y, 15);
    for(int i=0;i<11;i++){ PUT_PIXEL(v, cx+i, cy, 0); PUT_PIXEL(v, cx+i, cy+10, 0); PUT_PIXEL(v, cx, cy+i, 0); PUT_PIXEL(v, cx+10, cy+i, 0); }
    if(p){
        for(int y=cy+1;y<cy+10;y++)for(int x=cx+1;x<cx+10;x++)PUT_PIXEL(v, x, y, 0);
    }else{
        for(int i=2;i<9;i++){ PUT_PIXEL(v, cx+i, cy+2, 0); PUT_PIXEL(v, cx+i, cy+8, 0); PUT_PIXEL(v, cx+2, cy+i, 0); PUT_PIXEL(v, cx+8, cy+i, 0); }
        for(int i=3;i<8;i++) PUT_PIXEL(v, cx+i, cy+3, 0);
    }
}

static void window_ex(unsigned char* v, int wx, int wy, const char* t, int ww, int wh, int rs, int active, int no_close)
{
    // 1px black outline + inner background
    for(int y=wy;y<wy+wh&&y<screen_h;y++)for(int x=wx;x<wx+ww&&x<screen_w;x++) {
        if (x < 0 || y < 0) continue;
        if(y==wy||y==wy+wh-1||x==wx||x==wx+ww-1) PUT_PIXEL(v, x, y, 0);
        else PUT_PIXEL(v, x, y, 7);
    }
    
    // Title bar area (white)
    for(int y=wy+1;y<wy+18&&y<screen_h;y++)for(int x=wx+1;x<wx+ww-1&&x<screen_w;x++) {
        if (x >= 0 && y >= 0) PUT_PIXEL(v, x, y, 15);
    }
    for(int x=wx;x<wx+ww&&x<screen_w;x++) {
        if (x >= 0 && wy+18 >= 0 && wy+18 < screen_h) PUT_PIXEL(v, x, wy+18, 0);
    }
    
    int tw = 0; while(t[tw]) tw++;
    int tx = wx + (ww - tw*8)/2;
    
    if (active) {
        // Draw stripes
        for (int y = wy+3; y <= wy+15; y+=2) {
            if (y < 0 || y >= screen_h) continue;
            for (int x = wx+2; x < wx+ww-2 && x < screen_w; x++) {
                if (x < 0) continue;
                if ((!no_close && ((x >= wx+4 && x <= wx+16) || (x >= wx+ww-18 && x <= wx+ww-6))) ||
                    (x >= tx-4 && x <= tx+tw*8+4)) continue;
                PUT_PIXEL(v, x, y, 0);
            }
        }
    }
    
    if (!no_close) {
        cl_btn(v,wx+4,wy+4,0);
        mi_btn(v,wx+ww-16,wy+4,0);
    }
    
    font_draw_text(v,tx,wy+2,t,0);
    
    if(rs){
        int c=10;
        for(int i=0;i<c;i++)for(int j=0;j<=i;j++){
            int px=wx+ww-c+j,py=wy+wh-c+i;
            if(px>=0&&px<screen_w&&py>=0&&py<screen_h)PUT_PIXEL(v, px, py, 8);
        }
    }
}

static void wrap_desktop_label(const char* label, char* line1,
                               char* line2) {
    line1[0] = '\0';
    line2[0] = '\0';
    if (!label) return;
    int length = (int)strlen(label);
    if (length <= 7) {
        strncpy(line1, label, 23);
        line1[23] = '\0';
        return;
    }

    int split = 7;
    for (int index = 7; index > 0; index--) {
        if (label[index] == ' ') {
            split = index;
            break;
        }
    }
    int first_length = split;
    while (first_length > 0 && label[first_length - 1] == ' ') first_length--;
    if (first_length > 7) first_length = 7;
    for (int index = 0; index < first_length; index++) line1[index] = label[index];
    line1[first_length] = '\0';

    int second_start = split;
    while (label[second_start] == ' ') second_start++;
    int remaining = length - second_start;
    if (remaining <= 7) {
        for (int index = 0; index < remaining; index++) {
            line2[index] = label[second_start + index];
        }
        line2[remaining] = '\0';
    } else {
        for (int index = 0; index < 7; index++) {
            line2[index] = label[second_start + index];
        }
        line2[7] = '.';
        line2[8] = '.';
        line2[9] = '.';
        line2[10] = '\0';
    }
}

static int label_equals_ci(const char* left, const char* right) {
    if (!left || !right) return 0;
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return *left == '\0' && *right == '\0';
}

static unsigned char desktop_fallback_icon(const desktop_item_t* item) {
    static const unsigned char alternate[] = { 1, 2, 4, 7, 8, 9, 10, 11 };
    if (!item) return 6;
    if (item->kind == DESKTOP_ITEM_FOLDER ||
        (item->kind == DESKTOP_ITEM_SHORTCUT &&
         item->target_kind == DESKTOP_TARGET_FOLDER)) {
        return 5;
    }
    if (label_equals_ci(item->display_name, "About This Machine")) return 1;
    if (label_equals_ci(item->display_name, "Clock")) return 2;
    if (label_equals_ci(item->display_name, "Calculator")) return 4;
    if (label_equals_ci(item->display_name, "Control Panel")) return 6;
    if (label_equals_ci(item->display_name, "System Information")) return 7;
    if (label_equals_ci(item->display_name, "Notepad")) return 3;
    if (label_equals_ci(item->display_name, "Media Player")) return 10;
    if (label_equals_ci(item->display_name, "Image Viewer")) return 9;
    if (label_equals_ci(item->display_name, "File Management")) return 5;
    return alternate[(unsigned char)item->display_name[0] %
                     (sizeof(alternate) / sizeof(alternate[0]))];
}

static void icon(unsigned char* v, int x, int y, int desktop_index,
                 unsigned char bg, const char* lb, int sel, bool shortcut)
{
    if (sel) {
        for(int i=y-4; i<y+70; i++) for(int j=x-4; j<x+36; j++) PUT_PIXEL(v, j, i, 11);
    }
    
    unsigned char ic[16][16];
    for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=255;

    bool custom_ok = false;
    if (desktop_index >= 0 &&
        desktop_load_icon(desktop_index, ic)) {
        custom_ok = true;
    }

    if (custom_ok) {
    } else if(bg==1){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=9;
        for(int j=0;j<16;j++) ic[0][j]=11;
        for(int i=0;i<16;i++) ic[i][0]=11;
        for(int j=0;j<16;j++) ic[15][j]=1;
        for(int i=0;i<16;i++) ic[i][15]=1;
        for(int i=3;i<7;i++)for(int j=6;j<10;j++) ic[i][j]=15;
        for(int i=8;i<9;i++)for(int j=6;j<10;j++) ic[i][j]=15;
        for(int i=10;i<12;i++)for(int j=7;j<9;j++) ic[i][j]=15;
    }else if(bg==2){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=8;
        for(int i=2;i<14;i++)for(int j=2;j<14;j++) ic[i][j]=15;
        for(int j=2;j<14;j++){ic[2][j]=0;ic[13][j]=0;}
        for(int i=2;i<14;i++){ic[i][2]=0;ic[i][13]=0;}
        ic[8][7]=0;ic[8][8]=0;ic[8][9]=0;
        ic[5][7]=0;ic[6][7]=0;ic[7][7]=0;
        ic[8][7]=4;
    }else if(bg==4){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=0;j<16;j++) ic[0][j]=15;
        for(int i=0;i<16;i++) ic[i][0]=15;
        for(int j=0;j<16;j++) ic[15][j]=0;
        for(int i=0;i<16;i++) ic[i][15]=0;
        for(int i=2;i<6;i++)for(int j=2;j<14;j++) ic[i][j]=2;
        for(int j=2;j<14;j++) ic[2][j]=0;
        for(int i=7;i<10;i++)for(int j=2;j<6;j++) ic[i][j]=15;
        for(int i=7;i<10;i++)for(int j=7;j<11;j++) ic[i][j]=15;
        for(int i=11;i<14;i++)for(int j=2;j<6;j++) ic[i][j]=15;
        for(int i=11;i<14;i++)for(int j=7;j<11;j++) ic[i][j]=15;
        for(int i=7;i<14;i++)for(int j=12;j<14;j++) ic[i][j]=4;
        ic[3][8]=0;ic[3][9]=0;ic[3][10]=0;ic[3][11]=0;
    }else if(bg==3){ // Notepad
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int i=1;i<15;i++)for(int j=2;j<13;j++) ic[i][j]=15;
        for(int j=2;j<13;j++) {ic[0][j]=0; ic[15][j]=0;}
        for(int i=0;i<16;i++) {ic[i][1]=0; ic[i][13]=0;}
        for(int i=3;i<13;i+=3)for(int j=4;j<11;j++) ic[i][j]=0;
    }else if(bg==6){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=8;
        for(int j=0;j<16;j++) ic[0][j]=15;
        for(int i=0;i<16;i++) ic[i][0]=15;
        for(int j=0;j<16;j++) ic[15][j]=0;
        for(int i=0;i<16;i++) ic[i][15]=0;
        for(int j=3;j<13;j++) ic[4][j]=15;
        for(int j=3;j<13;j++) ic[8][j]=15;
        for(int j=3;j<13;j++) ic[12][j]=15;
        for(int i=3;i<7;i++)for(int j=7;j<9;j++) ic[i][j]=9;
        ic[5][7]=11;ic[3][7]=11;
    }else if(bg==5){
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=0;j<16;j++) ic[0][j]=15;
        for(int i=0;i<16;i++) ic[i][0]=15;
        for(int j=0;j<16;j++) ic[15][j]=0;
        for(int i=0;i<16;i++) ic[i][15]=0;
        for(int i=3;i<13;i++)for(int j=3;j<13;j++) ic[i][j]=6;
        for(int j=3;j<13;j++) ic[3][j]=14;
        for(int i=3;i<5;i++)for(int j=3;j<7;j++) ic[i][j]=14;
    }else if(bg==7){ // SysInfo (Computer)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int i=2;i<10;i++)for(int j=2;j<14;j++) ic[i][j]=15;
        for(int j=2;j<14;j++){ ic[2][j]=0; ic[9][j]=0; }
        for(int i=2;i<10;i++){ ic[i][2]=0; ic[i][13]=0; }
        for(int i=3;i<9;i++)for(int j=3;j<13;j++) ic[i][j]=1; // blue screen
        for(int j=5;j<11;j++) ic[10][j]=8;
        for(int j=3;j<13;j++) ic[11][j]=0;
        for(int j=3;j<13;j++) ic[12][j]=15;
    }else if(bg==8){ // Puzzle (15-Puzzle grid)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=0;j<16;j++){ ic[0][j]=15; ic[15][j]=0; }
        for(int i=0;i<16;i++){ ic[i][0]=15; ic[i][15]=0; }
        for(int i=1;i<15;i++){ ic[i][7]=0; ic[7][i]=0; }
        ic[3][3]=4; ic[3][4]=4; // tile 1
        ic[3][11]=2; ic[3][12]=2; // tile 2
        ic[11][3]=1; ic[11][4]=1; // tile 3
        ic[11][11]=8; ic[12][11]=8; // empty slot
    }else if(bg==9){ // Image Viewer (Photo frame with sun and mountain)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=1;j<15;j++) { ic[1][j]=15; ic[14][j]=0; }
        for(int i=1;i<15;i++) { ic[i][1]=15; ic[i][14]=0; }
        for(int i=2;i<14;i++)for(int j=2;j<14;j++) ic[i][j]=9; // blue sky
        ic[3][10]=14; ic[3][11]=14; ic[4][10]=14; ic[4][11]=14; // sun
        for(int j=2;j<9;j++) ic[13][j]=2;
        for(int j=3;j<8;j++) ic[12][j]=2;
        for(int j=4;j<7;j++) ic[11][j]=2;
        ic[10][5]=10; ic[10][6]=10;
        for(int j=7;j<14;j++) ic[13][j]=10;
        for(int j=8;j<13;j++) ic[12][j]=10;
        for(int j=9;j<12;j++) ic[11][j]=10;
        ic[10][10]=15; ic[10][11]=15; // snow peak
    }else if(bg==10){ // Audio Player (Vinyl record / CD with musical note)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=7;
        for(int j=1;j<15;j++) { ic[1][j]=15; ic[14][j]=0; }
        for(int i=1;i<15;i++) { ic[i][1]=15; ic[i][14]=0; }
        for(int i=2;i<14;i++)for(int j=2;j<14;j++) ic[i][j]=0; // black vinyl
        ic[3][7]=8; ic[3][8]=8; ic[12][7]=8; ic[12][8]=8;
        ic[7][3]=8; ic[8][3]=8; ic[7][12]=8; ic[8][12]=8;
        for(int i=6;i<=9;i++)for(int j=6;j<=9;j++) ic[i][j]=11;
        ic[7][7]=0; ic[8][8]=0;
        ic[3][10]=14; ic[3][11]=14; ic[3][12]=14;
        ic[4][10]=14; ic[4][12]=14;
        ic[5][10]=14; ic[5][12]=14;
        ic[6][9]=14; ic[6][10]=14; ic[6][11]=14; ic[6][12]=14;
    }else if(bg==11){ // Installer (Hard Drive with install arrow)
        for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=255;
        for(int i=6;i<15;i++)for(int j=1;j<15;j++) ic[i][j]=7;
        for(int j=1;j<15;j++){ ic[6][j]=15; ic[14][j]=0; }
        for(int i=6;i<15;i++){ ic[i][1]=15; ic[i][14]=0; }
        for(int j=3;j<13;j++) ic[8][j]=8;
        for(int j=3;j<13;j++) ic[12][j]=0;
        ic[10][11]=10; // green LED
        ic[1][7]=2; ic[1][8]=2;
        ic[2][7]=2; ic[2][8]=2;
        ic[3][7]=2; ic[3][8]=2;
        ic[4][5]=2; ic[4][6]=2; ic[4][7]=2; ic[4][8]=2; ic[4][9]=2; ic[4][10]=2;
        ic[5][6]=2; ic[5][7]=2; ic[5][8]=2; ic[5][9]=2;
        ic[6][7]=2; ic[6][8]=2;
    }
    
    for(int i=0;i<16;i++){
        for(int j=0;j<16;j++){
            unsigned char c = ic[i][j];
            if(c != 255){
                PUT_PIXEL(v, x+j*2, y+i*2, c);
                PUT_PIXEL(v, x+j*2+1, y+i*2, c);
                PUT_PIXEL(v, x+j*2, y+i*2+1, c);
                PUT_PIXEL(v, x+j*2+1, y+i*2+1, c);
            }
        }
    }

    if (shortcut) {
        for (int i = 22; i < 32; i++) {
            for (int j = 22; j < 32; j++) PUT_PIXEL(v, x + j, y + i, 15);
        }
        for (int i = 0; i < 10; i++) {
            PUT_PIXEL(v, x + 22 + i, y + 22, 0);
            PUT_PIXEL(v, x + 22 + i, y + 31, 0);
            PUT_PIXEL(v, x + 22, y + 22 + i, 0);
            PUT_PIXEL(v, x + 31, y + 22 + i, 0);
        }
        for (int i = 0; i < 5; i++) {
            PUT_PIXEL(v, x + 25 + i, y + 26 + i, 0);
            PUT_PIXEL(v, x + 25 + i, y + 28 + i, 0);
        }
        PUT_PIXEL(v, x + 23, y + 28, 0);
        PUT_PIXEL(v, x + 24, y + 27, 0);
    }
    
    if (lb) {
        char line1[24];
        char line2[24];
        wrap_desktop_label(lb, line1, line2);
        int tw1 = (int)strlen(line1);
        int tw2 = (int)strlen(line2);
        int tx1 = x + 16 - tw1 * 4;
        int tx2 = x + 16 - tw2 * 4;
        int y1 = line2[0] ? y + 34 : y + 36;
        if (sel) {
            int label_chars = tw1 > tw2 ? tw1 : tw2;
            int label_left = x + 16 - label_chars * 4 - 2;
            int label_right = x + 16 + label_chars * 4 + 2;
            for(int yy=y1-2; yy<y+68; yy++) {
                for(int xx=label_left; xx<label_right; xx++) {
                    PUT_PIXEL(v, xx, yy, 1);
                }
            }
            font_draw_text(v, tx1, y1, line1, 15);
            if (line2[0]) font_draw_text(v, tx2, y+50, line2, 15);
        } else {
            font_draw_text(v, tx1, y1, line1, 0);
            if (line2[0]) font_draw_text(v, tx2, y+50, line2, 0);
        }
    }
}

static void draw_power_dialog(unsigned char* v)
{
    int dw = 220, dh = 80;
    int dx = (screen_w - dw) / 2;
    int dy = (screen_h - dh) / 2;

    for(int y=dy;y<dy+dh;y++)for(int x=dx;x<dx+dw;x++)PUT_PIXEL(v, x, y, 8);
    for(int y=dy+2;y<dy+dh-2;y++)for(int x=dx+2;x<dx+dw-2;x++)PUT_PIXEL(v, x, y, 7);
    for(int x=dx+2;x<dx+dw-2;x++){PUT_PIXEL(v, x, dy+2, 15);PUT_PIXEL(v, x, dy+dh-3, 0);}
    for(int y=dy+2;y<dy+dh-2;y++){PUT_PIXEL(v, dx+2, y, 15);PUT_PIXEL(v, dx+dw-3, y, 0);}
    if (power_confirm == 1) {
        font_draw_text(v,dx+60,dy+10,"Restart?",0);font_draw_text(v,dx+50,dy+28,"Are you sure?",0);
    } else {
        font_draw_text(v,dx+50,dy+10,"Shutdown?",0);font_draw_text(v,dx+50,dy+28,"Are you sure?",0);
    }
    int bx=dx+20,by=dy+48;
    for(int y=by;y<by+22;y++)for(int x=bx;x<bx+80;x++)PUT_PIXEL(v, x, y, power_cancel_pressed?0:7);
    for(int x=bx;x<bx+80;x++){PUT_PIXEL(v, x, by, power_cancel_pressed?0:15);PUT_PIXEL(v, x, by+21, power_cancel_pressed?15:0);}
    for(int y=by;y<by+22;y++){PUT_PIXEL(v, bx, y, power_cancel_pressed?0:15);PUT_PIXEL(v, bx+79, y, power_cancel_pressed?15:0);}
    font_draw_text(v,bx+16,by+3,"Cancel",power_cancel_pressed?15:0);
    
    bx=dx+110;
    for(int y=by;y<by+22;y++)for(int x=bx;x<bx+80;x++)PUT_PIXEL(v, x, y, power_ok_pressed?4:4);
    for(int x=bx;x<bx+80;x++){PUT_PIXEL(v, x, by, power_ok_pressed?0:12);PUT_PIXEL(v, x, by+21, power_ok_pressed?12:0);}
    for(int y=by;y<by+22;y++){PUT_PIXEL(v, bx, y, power_ok_pressed?0:12);PUT_PIXEL(v, bx+79, y, power_ok_pressed?12:0);}
    font_draw_text(v,bx+32,by+3,"OK",15);
}

static void draw_uninit_disk_dialog(unsigned char* v)
{
    int dw = 260, dh = 86;
    int dx = (screen_w - dw) / 2;
    int dy = (screen_h - dh) / 2;

    for(int y=dy;y<dy+dh;y++)for(int x=dx;x<dx+dw;x++)PUT_PIXEL(v, x, y, 8);
    for(int y=dy+2;y<dy+dh-2;y++)for(int x=dx+2;x<dx+dw-2;x++)PUT_PIXEL(v, x, y, 7);
    for(int x=dx+2;x<dx+dw-2;x++){PUT_PIXEL(v, x, dy+2, 15);PUT_PIXEL(v, x, dy+dh-3, 0);}
    for(int y=dy+2;y<dy+dh-2;y++){PUT_PIXEL(v, dx+2, y, 15);PUT_PIXEL(v, dx+dw-3, y, 0);}
    
    font_draw_text(v, dx+26, dy+12, "Uninitialized Disk Detected", 0);
    font_draw_text(v, dx+32, dy+28, "Initialize this drive now?", 0);

    int cbx = dx + 20, oby = dy + 52;
    for(int y=oby;y<oby+22;y++)for(int x=cbx;x<cbx+100;x++)PUT_PIXEL(v, x, y, uninit_ignore_pressed?0:7);
    for(int x=cbx;x<cbx+100;x++){PUT_PIXEL(v, x, oby, uninit_ignore_pressed?0:15);PUT_PIXEL(v, x, oby+21, uninit_ignore_pressed?15:0);}
    for(int y=oby;y<oby+22;y++){PUT_PIXEL(v, cbx, y, uninit_ignore_pressed?0:15);PUT_PIXEL(v, cbx+99, y, uninit_ignore_pressed?15:0);}
    font_draw_text(v, cbx+28, oby+3, "Ignore", uninit_ignore_pressed?15:0);

    int obx = dx + 140;
    for(int y=oby;y<oby+22;y++)for(int x=obx;x<obx+100;x++)PUT_PIXEL(v, x, y, uninit_init_pressed?0:7);
    for(int x=obx;x<obx+100;x++){PUT_PIXEL(v, x, oby, uninit_init_pressed?0:15);PUT_PIXEL(v, x, oby+21, uninit_init_pressed?15:0);}
    for(int y=oby;y<oby+22;y++){PUT_PIXEL(v, obx, y, uninit_init_pressed?0:15);PUT_PIXEL(v, obx+99, y, uninit_init_pressed?15:0);}
    font_draw_text(v, obx+18, oby+3, "Initialize", uninit_init_pressed?15:0);
}

static inline void put_rgb32_pixel(unsigned char* v, int px, int py, uint32_t rgb) {
    if (px < 0 || px >= screen_w || py < 0 || py >= screen_h) return;
    if (screen_bpp >= 32) {
        *(uint32_t*)(v + (size_t)py * (size_t)screen_pitch + (size_t)px * 4) = rgb & 0x00FFFFFFu;
    } else if (screen_bpp == 24) {
        unsigned char* p = v + (size_t)py * (size_t)screen_pitch + (size_t)px * 3;
        p[0] = (unsigned char)(rgb & 0xFFu);
        p[1] = (unsigned char)((rgb >> 8) & 0xFFu);
        p[2] = (unsigned char)((rgb >> 16) & 0xFFu);
    } else if (screen_bpp == 16) {
        uint8_t r = (uint8_t)((rgb >> 16) & 0xFFu);
        uint8_t g = (uint8_t)((rgb >> 8) & 0xFFu);
        uint8_t b = (uint8_t)(rgb & 0xFFu);
        *(uint16_t*)(v + (size_t)py * (size_t)screen_pitch + (size_t)px * 2) = RGB16(r, g, b);
    } else {
        uint32_t r = ((rgb >> 16) & 0xFFu) * 5u / 255u;
        uint32_t g = ((rgb >> 8) & 0xFFu) * 5u / 255u;
        uint32_t b = (rgb & 0xFFu) * 5u / 255u;
        v[(size_t)py * (size_t)screen_pitch + (size_t)px] = (unsigned char)(32u + r + g * 6u + b * 36u);
    }
}

static void blit_dyn_client_rect(unsigned char* v, const wm_dyn_win_t* dw,
                                 int rx0, int ry0, int rw, int rh) {
    if (!v || dw->shaded || !dw->canvas_kvirt) return;
    int ww_cur = dw->client_w + 2;
    int wh_cur = dw->client_h + 20;
    int is_rs = (dw->flags & PXE_WIN_RESIZABLE) ? 1 : 0;

    int cw = dw->client_w;
    int ch = dw->client_h;
    if (cw > dw->alloc_stride) cw = dw->alloc_stride;
    if (ch > dw->alloc_h) ch = dw->alloc_h;

    if (rx0 < 0) { rw += rx0; rx0 = 0; }
    if (ry0 < 0) { rh += ry0; ry0 = 0; }
    int rx1 = rx0 + rw;
    int ry1 = ry0 + rh;
    if (rx1 > cw) rx1 = cw;
    if (ry1 > ch) ry1 = ch;
    if (rx0 >= rx1 || ry0 >= ry1) return;

    int cx0 = dw->x + 1;
    int cy0 = dw->y + 19;

    if (dw->format == PXE_CANVAS_RGB32) {
        for (int ry = ry0; ry < ry1; ry++) {
            int sy = cy0 + ry;
            if (sy < 0 || sy >= screen_h) continue;
            const uint32_t* src = (const uint32_t*)((const uint8_t*)dw->canvas_kvirt +
                                                    (size_t)ry * (size_t)dw->pitch);
            for (int rx = rx0; rx < rx1; rx++) {
                int sx = cx0 + rx;
                if (sx >= 0 && sx < screen_w) {
                    put_rgb32_pixel(v, sx, sy, src[rx]);
                }
            }
        }
    } else {
        for (int ry = ry0; ry < ry1; ry++) {
            int sy = cy0 + ry;
            if (sy < 0 || sy >= screen_h) continue;
            const uint8_t* src = (const uint8_t*)dw->canvas_kvirt +
                                 (size_t)ry * (size_t)dw->pitch;
            for (int rx = rx0; rx < rx1; rx++) {
                int sx = cx0 + rx;
                if (sx < 0 || sx >= screen_w) continue;
                uint8_t c = src[rx];
                if (c != 255) {
                    PUT_PIXEL(v, sx, sy, c);
                }
            }
        }
    }

    if (is_rs) {
        int c = 10;
        int grip_rx0 = dw->client_w + 1 - c;
        int grip_ry0 = dw->client_h + 1 - c;
        if (rx1 > grip_rx0 && ry1 > grip_ry0) {
            for (int i = 0; i < c; i++) {
                int py = dw->y + wh_cur - c + i;
                int dry = py - cy0;
                if (dry < ry0 || dry >= ry1 || py < 0 || py >= screen_h) continue;
                for (int j = 0; j <= i; j++) {
                    int px = dw->x + ww_cur - c + j;
                    int drx = px - cx0;
                    if (drx >= rx0 && drx < rx1 && px >= 0 && px < screen_w) {
                        PUT_PIXEL(v, px, py, 8);
                    }
                }
            }
        }
    }
}

static void blit_dyn_window(unsigned char* v, const wm_dyn_win_t* dw, int active) {
    int ww_cur = dw->client_w + 2;
    int wh_cur = dw->shaded ? 20 : (dw->client_h + 20);
    int no_cl = (dw->flags & PXE_WIN_NO_CLOSE) ? 1 : 0;

    window_ex(v, dw->x, dw->y, dw->title, ww_cur, wh_cur, 0, active, no_cl);
    blit_dyn_client_rect(v, dw, 0, 0, dw->client_w, dw->client_h);
}

static void format_hex64(uint64_t val, char* out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++) {
        out[2 + i] = hex[(val >> ((15 - i) * 4)) & 0xFu];
    }
    out[18] = '\0';
}

static size_t crash_append_str(char* dst, size_t cap, size_t pos, const char* src) {
    if (!dst || cap == 0 || pos >= cap - 1 || !src) return pos;
    while (*src && pos < cap - 1) {
        dst[pos++] = *src++;
    }
    dst[pos] = '\0';
    return pos;
}

static size_t crash_append_u32(char* dst, size_t cap, size_t pos, uint32_t val) {
    char rev[16];
    int n = 0;
    if (val == 0) {
        rev[n++] = '0';
    } else {
        while (val > 0 && n < (int)sizeof(rev)) {
            rev[n++] = (char)('0' + (val % 10u));
            val /= 10u;
        }
    }
    while (n > 0 && pos < cap - 1) {
        dst[pos++] = rev[--n];
    }
    if (pos < cap) dst[pos] = '\0';
    return pos;
}

static void draw_crash_dialog(unsigned char* v) {
    int dw = 380, dh = 144;
    int dx = (screen_w - dw) / 2;
    int dy = (screen_h - dh) / 2;

    for(int y=dy;y<dy+dh;y++)for(int x=dx;x<dx+dw;x++)PUT_PIXEL(v, x, y, 0);
    for(int y=dy+2;y<dy+dh-2;y++)for(int x=dx+2;x<dx+dw-2;x++)PUT_PIXEL(v, x, y, 15);
    for(int y=dy+4;y<dy+dh-4;y++)for(int x=dx+4;x<dx+dw-4;x++) {
        if (y==dy+4||y==dy+dh-5||x==dx+4||x==dx+dw-5) PUT_PIXEL(v, x, y, 0);
        else PUT_PIXEL(v, x, y, 7);
    }

    /* Classic Mac System Bomb Icon (scaled 2x at dx+16, dy+18) */
    int bx0 = dx + 16, by0 = dy + 18;
    for (int iy = 4; iy < 15; iy++) {
        for (int ix = 2; ix < 13; ix++) {
            int ddx = ix - 7, ddy = iy - 9;
            if (ddx*ddx + ddy*ddy <= 24) {
                unsigned char col = (ix == 5 && iy == 7) ? 15 : 0;
                PUT_PIXEL(v, bx0+ix*2, by0+iy*2, col);
                PUT_PIXEL(v, bx0+ix*2+1, by0+iy*2, col);
                PUT_PIXEL(v, bx0+ix*2, by0+iy*2+1, col);
                PUT_PIXEL(v, bx0+ix*2+1, by0+iy*2+1, col);
            }
        }
    }
    for (int k = 0; k < 4; k++) {
        PUT_PIXEL(v, bx0+18+k, by0+8-k, 8);
        PUT_PIXEL(v, bx0+19+k, by0+8-k, 0);
    }
    PUT_PIXEL(v, bx0+23, by0+3, 14); PUT_PIXEL(v, bx0+24, by0+2, 12);
    PUT_PIXEL(v, bx0+22, by0+2, 12); PUT_PIXEL(v, bx0+25, by0+4, 14);

    font_draw_text(v, dx + 54, dy + 14, "Sorry, an application crashed.", 0);

    char line1[64];
    line1[0] = '\0';
    size_t p1 = crash_append_str(line1, sizeof(line1), 0, "App: ");
    p1 = crash_append_str(line1, sizeof(line1), p1, crash_app_name);
    p1 = crash_append_str(line1, sizeof(line1), p1, " (PID ");
    p1 = crash_append_u32(line1, sizeof(line1), p1, crash_pid);
    (void)crash_append_str(line1, sizeof(line1), p1, ")");
    font_draw_text(v, dx + 54, dy + 34, line1, 0);

    char line2[64];
    line2[0] = '\0';
    size_t p2 = crash_append_str(line2, sizeof(line2), 0, "Fault: ");
    (void)crash_append_str(line2, sizeof(line2), p2, crash_exc_name);
    font_draw_text(v, dx + 54, dy + 52, line2, 4);

    char rip_hex[20];
    format_hex64(crash_rip, rip_hex);
    char line3[64];
    line3[0] = '\0';
    size_t p3 = crash_append_str(line3, sizeof(line3), 0, "RIP: ");
    (void)crash_append_str(line3, sizeof(line3), p3, rip_hex);
    font_draw_text(v, dx + 54, dy + 70, line3, 8);

    char cr2_hex[20];
    format_hex64(crash_cr2, cr2_hex);
    char line4[64];
    line4[0] = '\0';
    size_t p4 = crash_append_str(line4, sizeof(line4), 0, "CR2: ");
    (void)crash_append_str(line4, sizeof(line4), p4, cr2_hex);
    font_draw_text(v, dx + 54, dy + 88, line4, 8);

    int obx = dx + dw - 96, oby = dy + dh - 32;
    for(int y=oby;y<oby+22;y++)for(int x=obx;x<obx+80;x++)PUT_PIXEL(v, x, y, crash_ok_pressed?0:7);
    for(int x=obx;x<obx+80;x++){PUT_PIXEL(v, x, oby, crash_ok_pressed?0:15);PUT_PIXEL(v, x, oby+21, crash_ok_pressed?15:0);}
    for(int y=oby;y<oby+22;y++){PUT_PIXEL(v, obx, y, crash_ok_pressed?0:15);PUT_PIXEL(v, obx+79, y, crash_ok_pressed?15:0);}
    font_draw_text(v, obx + 32, oby + 3, "OK", crash_ok_pressed ? 15 : 0);
}

static void draw_r0_confirm_dialog(unsigned char* v) {
    int dw = 380, dh = 124;
    int dx = (screen_w - dw) / 2;
    int dy = (screen_h - dh) / 2;

    for(int y=dy;y<dy+dh;y++)for(int x=dx;x<dx+dw;x++)PUT_PIXEL(v, x, y, 8);
    for(int y=dy+2;y<dy+dh-2;y++)for(int x=dx+2;x<dx+dw-2;x++)PUT_PIXEL(v, x, y, 7);
    for(int x=dx+2;x<dx+dw-2;x++){PUT_PIXEL(v, x, dy+2, 15);PUT_PIXEL(v, x, dy+dh-3, 0);}
    for(int y=dy+2;y<dy+dh-2;y++){PUT_PIXEL(v, dx+2, y, 15);PUT_PIXEL(v, dx+dw-3, y, 0);}

    /* Yellow Warning Triangle */
    int tx0 = dx + 26, ty0 = dy + 16;
    for (int r = 0; r < 24; r++) {
        int half = r / 2;
        for (int c = -half; c <= half; c++) {
            unsigned char col = (r == 23 || c == -half || c == half) ? 0 : 14;
            PUT_PIXEL(v, tx0 + c, ty0 + r, col);
        }
    }
    for (int r = 7; r <= 15; r++) { PUT_PIXEL(v, tx0, ty0 + r, 0); PUT_PIXEL(v, tx0 + 1, ty0 + r, 0); }
    PUT_PIXEL(v, tx0, ty0 + 19, 0); PUT_PIXEL(v, tx0 + 1, ty0 + 19, 0);

    font_draw_text(v, dx + 52, dy + 14, "Ring 0 Kernel Executable", 4);
    font_draw_text(v, dx + 52, dy + 34, "Grant full Ring 0 hardware access?", 0);

    char disp_path[44];
    int plen = (int)strlen(r0_confirm_path);
    if (plen <= 40) {
        strcpy(disp_path, r0_confirm_path);
    } else {
        disp_path[0] = '.'; disp_path[1] = '.'; disp_path[2] = '.';
        strcpy(disp_path + 3, r0_confirm_path + (plen - 37));
    }
    font_draw_text(v, dx + 20, dy + 56, disp_path, 1);

    int cbx = dx + 40, oby = dy + 86;
    for(int y=oby;y<oby+22;y++)for(int x=cbx;x<cbx+110;x++)PUT_PIXEL(v, x, y, r0_cancel_pressed?0:7);
    for(int x=cbx;x<cbx+110;x++){PUT_PIXEL(v, x, oby, r0_cancel_pressed?0:15);PUT_PIXEL(v, x, oby+21, r0_cancel_pressed?15:0);}
    for(int y=oby;y<oby+22;y++){PUT_PIXEL(v, cbx, y, r0_cancel_pressed?0:15);PUT_PIXEL(v, cbx+109, y, r0_cancel_pressed?15:0);}
    font_draw_text(v, cbx + 31, oby + 3, "Cancel", r0_cancel_pressed ? 15 : 0);

    int obx = dx + 210;
    for(int y=oby;y<oby+22;y++)for(int x=obx;x<obx+130;x++)PUT_PIXEL(v, x, y, 4);
    for(int x=obx;x<obx+130;x++){PUT_PIXEL(v, x, oby, r0_run_pressed?0:12);PUT_PIXEL(v, x, oby+21, r0_run_pressed?12:0);}
    for(int y=oby;y<oby+22;y++){PUT_PIXEL(v, obx, y, r0_run_pressed?0:12);PUT_PIXEL(v, obx+129, y, r0_run_pressed?12:0);}
    font_draw_text(v, obx + 17, oby + 3, "Run (Ring 0)", 15);
}

static wm_dyn_win_t* get_active_pxe_menubar_win(void) {
    int active_id = top_visible_win_id();
    if (active_id >= WM_DYN_BASE && active_id < WM_DYN_BASE + WM_DYN_MAX) {
        wm_dyn_win_t* dw = &g_dyn_wins[active_id - WM_DYN_BASE];
        if (dw->used && dw->has_menu && dw->menu_bar.menu_count > 0) {
            return dw;
        }
    }
    return NULL;
}

static void get_pxe_menu_span(const pxe_menu_bar_t* mb, int idx, int* out_x, int* out_w) {
    int cur_x = 30;
    for (int i = 0; i < mb->menu_count && i < PXE_MAX_MENUS; i++) {
        int tw = (int)strlen(mb->menus[i].title) * 8 + 16;
        if (tw < 40) tw = 40;
        if (i == idx) {
            *out_x = cur_x;
            *out_w = tw;
            return;
        }
        cur_x += tw + 4;
    }
    *out_x = cur_x;
    *out_w = 40;
}

static void draw_others(unsigned char* v, int dgw)
{
    int active_win = top_visible_win_id();
    for (int i = 0; i < WM_Z_COUNT; i++) {
        int id = z_order[i];
        if (id + 1 == dgw) continue;
        if (id >= WM_DYN_BASE && id < WM_DYN_BASE + WM_DYN_MAX) {
            const wm_dyn_win_t* dw = &g_dyn_wins[id - WM_DYN_BASE];
            if (dw->used) {
                blit_dyn_window(v, dw, (active_win == id));
            }
        }
    }
}

static void draw_all(unsigned char* v)
{
    draw_others(v, 0);
}

static void draw_menu_popup(unsigned char* v, int x, int y, int w, int h)
{
    for(int py = y; py < y + h && py < screen_h; py++) {
        for(int px = x; px < x + w && px < screen_w; px++) {
            PUT_PIXEL(v, px, py, 15);
        }
    }
    for(int px = x; px < x + w && px < screen_w; px++) {
        PUT_PIXEL(v, px, y, 0);
        PUT_PIXEL(v, px, y + h - 1, 0);
    }
    for(int py = y; py < y + h && py < screen_h; py++) {
        PUT_PIXEL(v, x, py, 0);
        PUT_PIXEL(v, x + w - 1, py, 0);
    }
    for(int py = y + 2; py < y + h + 2 && py < screen_h; py++) {
        if (x + w < screen_w) PUT_PIXEL(v, x + w, py, 0);
        if (x + w + 1 < screen_w) PUT_PIXEL(v, x + w + 1, py, 0);
    }
    for(int px = x + 2; px < x + w + 2 && px < screen_w; px++) {
        if (y + h < screen_h) PUT_PIXEL(v, px, y + h, 0);
        if (y + h + 1 < screen_h) PUT_PIXEL(v, px, y + h + 1, 0);
    }
}

static void draw_menubar(unsigned char* v)
{
    bool install_mode = ui_install_mode_requested();
    wm_dyn_win_t* pxe_mb_win = (!install_mode) ? get_active_pxe_menubar_win() : NULL;

    for(int y=0; y<20; y++) for(int x=0; x<screen_w; x++) PUT_PIXEL(v, x, y, 15);
    for(int x=0; x<screen_w; x++) PUT_PIXEL(v, x, 20, 0);
    for(int y=4; y<16; y++) for(int x=10; x<22; x++) PUT_PIXEL(v, x, y, (y/2)%6+1);

    if (menu_open && install_mode && menu_index != 4) {
        menu_open = 0;
        menu_index = -1;
    }

    if (pxe_mb_win) {
        const pxe_menu_bar_t* mb = &pxe_mb_win->menu_bar;
        for (int m = 0; m < mb->menu_count && m < PXE_MAX_MENUS; m++) {
            int mx_pos = 0, mw = 0;
            get_pxe_menu_span(mb, m, &mx_pos, &mw);
            if (menu_open && menu_index == m) {
                for (int y = 2; y < 18; y++) {
                    for (int x = mx_pos - 4; x < mx_pos + mw - 4 && x < screen_w; x++) {
                        PUT_PIXEL(v, x, y, 1);
                    }
                }
                font_draw_text(v, mx_pos + 4, 2, mb->menus[m].title, 15);
            } else {
                font_draw_text(v, mx_pos + 4, 2, mb->menus[m].title, 0);
            }
        }
        if (menu_open) {
            if (menu_index == 4) {
                draw_menu_popup(v, 4, 20, 172, 5*18+4);
                font_draw_text(v, 10, 24, "About Pixel Neko", 0);
                font_draw_text(v, 10, 24+18, "System Information", 0);
                font_draw_text(v, 10, 24+36, "Task Manager", 0);
                font_draw_text(v, 10, 24+54, "Restart Computer", 0);
                font_draw_text(v, 10, 24+72, "Shut Down Computer", 0);
            } else if (menu_index >= 0 && menu_index < mb->menu_count && menu_index < PXE_MAX_MENUS) {
                const pxe_menu_t* pm = &mb->menus[menu_index];
                int mx_pos = 0, mw = 0;
                get_pxe_menu_span(mb, menu_index, &mx_pos, &mw);
                int cnt = pm->item_count;
                if (cnt < 1) cnt = 1;
                if (cnt > PXE_MAX_MENU_ITEMS) cnt = PXE_MAX_MENU_ITEMS;
                draw_menu_popup(v, mx_pos - 4, 20, 176, cnt * 18 + 4);
                if (pm->item_count <= 0) {
                    font_draw_text(v, mx_pos + 4, 24, "(Empty)", 8);
                } else {
                    for (int it = 0; it < cnt; it++) {
                        font_draw_text(v, mx_pos + 4, 24 + it * 18,
                                       pm->items[it].label,
                                       pm->items[it].enabled ? 0 : 8);
                    }
                }
            }
        }
    } else if (menu_open) {
        if (menu_index != 4 && !install_mode) {
            int mx = 30 + menu_index * 40;
            int mw = 40;
            for(int y=2; y<18; y++) for(int x=mx-4; x<mx+mw; x++) PUT_PIXEL(v, x, y, 1);
        }
        if (!install_mode) {
            font_draw_text(v, 30, 2, "File", menu_index==0?15:0);
            font_draw_text(v, 70, 2, "Edit", menu_index==1?15:0);
            font_draw_text(v, 110, 2, "View", menu_index==2?15:0);
        }
        
        if (menu_index == 4) { // Apple Logo
            if (install_mode) {
                draw_menu_popup(v, 4, 20, 172, 2*18+4);
                font_draw_text(v, 10, 24, "Restart Computer", 0);
                font_draw_text(v, 10, 24+18, "Shut Down Computer", 0);
            } else {
                draw_menu_popup(v, 4, 20, 172, 5*18+4);
                font_draw_text(v, 10, 24, "About Pixel Neko", 0);
                font_draw_text(v, 10, 24+18, "System Information", 0);
                font_draw_text(v, 10, 24+36, "Task Manager", 0);
                font_draw_text(v, 10, 24+54, "Restart Computer", 0);
                font_draw_text(v, 10, 24+72, "Shut Down Computer", 0);
            }
        } else if (!install_mode && menu_index == 0) { // File
            draw_menu_popup(v, 30, 20, 110, 1*18+4);
            font_draw_text(v, 38, 24, "(Empty)", 8);
        } else if (!install_mode && menu_index == 1) { // Edit
            draw_menu_popup(v, 66, 20, 120, 6*18+4);
            font_draw_text(v, 74, 24, "Undo", 8);
            font_draw_text(v, 74, 24+18, "Cut", 8);
            font_draw_text(v, 74, 24+36, "Copy", 8);
            font_draw_text(v, 74, 24+54, "Paste", 8);
            font_draw_text(v, 74, 24+72, "Select All", 8);
            font_draw_text(v, 74, 24+90, "Clear", 8);
        } else if (!install_mode && menu_index == 2) { // View
            draw_menu_popup(v, 106, 20, 160, 1*18+4);
            font_draw_text(v, 114, 24, "Rearrange Desktop",
                           desktop_is_mutable() ? 0 : 8);
        }
    } else if (!install_mode) {
        font_draw_text(v, 30, 2, "File", 0);
        font_draw_text(v, 70, 2, "Edit", 0);
        font_draw_text(v, 110, 2, "View", 0);
    }
    
    char clock_text[32];
    int clock_length = format_clock_text(clock_text, sizeof(clock_text));
    int clock_x = screen_w - clock_length * 8 - 10;
    if (clock_x < 24) clock_x = 24;
    int volume_x = 0;
    notification_volume_span(&volume_x, NULL);
    draw_volume_icon(v, volume_x, 2);
    font_draw_text(v, clock_x, 2, clock_text, 0);

    if (opened_msg[0]) {
        int ox = screen_w / 2 - 40;
        if (ox < 10) ox = 10;
        font_draw_text(v, ox, 22, opened_msg, 0);
    }
    draw_volume_popup(v);
}

extern int g_desktop_pattern;
static const unsigned char g_patterns[6][8] = {
    { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 }, // 0: Stipple 50%
    { 0x88, 0x44, 0x22, 0x11, 0x88, 0x44, 0x22, 0x11 }, // 1: Diagonal
    { 0x80, 0x00, 0x08, 0x00, 0x80, 0x00, 0x08, 0x00 }, // 2: Micro Dots
    { 0xCC, 0xCC, 0x33, 0x33, 0xCC, 0xCC, 0x33, 0x33 }, // 3: Brick Tile
    { 0xAA, 0x00, 0xAA, 0x00, 0xAA, 0x00, 0xAA, 0x00 }, // 4: Slate Gray
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  // 5: Solid Gray
};

static void clamp_win_pos(int* x, int* y, int* w, int* h, int min_w, int min_h) {
    if (*w > screen_w - 8) *w = screen_w - 8;
    if (*h > screen_h - 28) *h = screen_h - 28;
    if (*w < min_w && min_w <= screen_w - 8) *w = min_w;
    if (*h < min_h && min_h <= screen_h - 28) *h = min_h;
    if (*x + *w > screen_w) *x = screen_w - *w;
    if (*y + *h > screen_h) *y = screen_h - *h;
    if (*x < 0) *x = 0;
    if (*y < 20) *y = 20;
}

static inline uint64_t wm_irq_save(void) {
    uint64_t rflags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(rflags) :: "memory");
    return rflags;
}

static inline void wm_irq_restore(uint64_t rflags) {
    if (rflags & (1ULL << 9)) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

static void wm_dyn_push_event(wm_dyn_win_t* dw, const pxe_event_t* ev) {
    if (!dw || !dw->used || !ev) return;
    uint64_t flags = wm_irq_save();
    if ((ev->type == PXE_EV_MOUSE_MOVE || ev->type == PXE_EV_REDRAW) &&
        dw->ev_count > 0) {
        int last_idx = (dw->ev_tail - 1 + WM_EV_QUEUE_CAP) % WM_EV_QUEUE_CAP;
        if (dw->ev_queue[last_idx].type == ev->type) {
            dw->ev_queue[last_idx] = *ev;
            wm_irq_restore(flags);
            scheduler_wake_win_waiters(dw->win_id);
            return;
        }
    }
    if (dw->ev_count < WM_EV_QUEUE_CAP) {
        dw->ev_queue[dw->ev_tail] = *ev;
        dw->ev_tail = (dw->ev_tail + 1) % WM_EV_QUEUE_CAP;
        dw->ev_count++;
    }
    wm_irq_restore(flags);
    scheduler_wake_win_waiters(dw->win_id);
}

static void wm_dyn_push_simple(wm_dyn_win_t* dw, uint32_t type) {
    if (!dw || !dw->used) return;
    pxe_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.win_id = dw->win_id;
    ev.w = dw->client_w;
    ev.h = dw->client_h;
    wm_dyn_push_event(dw, &ev);
}

static void wm_sync_volume_state(void) {
    static int last_volume_state = -1;
    static int last_mute_state = -1;
    int current_volume = audio_get_volume();
    int current_mute = audio_is_muted() ? 1 : 0;
    if (current_volume != last_volume_state ||
        current_mute != last_mute_state) {
        int had_prev = (last_volume_state != -1);
        last_volume_state = current_volume;
        last_mute_state = current_mute;
        redraw = 1;
        need_mr = 1;
        if (had_prev) {
            for (int s = 0; s < WM_DYN_MAX; s++) {
                if (g_dyn_wins[s].used) {
                    wm_dyn_push_simple(&g_dyn_wins[s], PXE_EV_REDRAW);
                }
            }
        }
    }
}

void wm_on_resolution_change(int w, int h) {
    (void)w;
    (void)h;
    volume_popup_open = 0;
    volume_drag_active = 0;
    desktop_on_resolution_change(w, h);
    for (int s = 0; s < WM_DYN_MAX; s++) {
        wm_dyn_win_t* dw = &g_dyn_wins[s];
        if (!dw->used) continue;
        int ow = dw->client_w + 2;
        int oh = dw->client_h + 20;
        int old_cw = dw->client_w;
        int old_ch = dw->client_h;
        if (dw->flags & PXE_WIN_RESIZABLE) {
            clamp_win_pos(&dw->x, &dw->y, &ow, &oh, dw->min_w + 2, dw->min_h + 20);
            int ncw = ow - 2;
            int nch = oh - 20;
            if (ncw > dw->alloc_stride) ncw = dw->alloc_stride;
            if (nch > dw->alloc_h) nch = dw->alloc_h;
            dw->client_w = ncw;
            dw->client_h = nch;
            if (ncw != old_cw || nch != old_ch) {
                wm_dyn_push_simple(dw, PXE_EV_RESIZE);
                wm_dyn_push_simple(dw, PXE_EV_REDRAW);
            }
        } else {
            if (dw->x + ow > screen_w) dw->x = screen_w - ow;
            if (dw->y + oh > screen_h) dw->y = screen_h - oh;
            if (dw->x < 0) dw->x = 0;
            if (dw->y < 20) dw->y = 20;
        }
    }
    redraw = 1;
    need_mr = 1;
}

void wm_init(void){
    desktop_init();
    redraw = 1;
}

void wm_installer_activate(void) {
    menu_open = 0;
    menu_index = -1;
    pxe_launch_builtin_app("install.pxe", NULL, true);
    redraw = 1;
    need_mr = 1;
}

static bool dyn_rect_occluded(int z_idx, int sx, int sy, int sw, int sh) {
    int sx2 = sx + sw;
    int sy2 = sy + sh;
    for (int j = z_idx + 1; j < WM_Z_COUNT; j++) {
        int oid = z_order[j];
        int ox = 0, oy = 0, ow = 0, oh = 0, ovis = 0, omin = 0;
        get_win_geom(oid, &ox, &oy, &ow, &oh, &ovis, &omin);
        if (!ovis || omin) continue;
        /* Include 2px window drop shadow on bottom/right */
        if (sx < ox + ow + 2 && sx2 > ox && sy < oy + oh + 2 && sy2 > oy) {
            return true;
        }
    }
    return false;
}

void wm_draw(unsigned char* v){
    if(redraw){
        extern void mouse_invalidate(void);
        mouse_invalidate();
        g_dirty_rect_count = 0;
        if (wallpaper_is_active()) {
            wallpaper_render(v, screen_w, screen_h, screen_bpp, screen_pitch);
        } else {
            if (g_desktop_pattern < 0 || g_desktop_pattern > 5) g_desktop_pattern = 0;
            if (g_desktop_pattern == 5) {
                for (int y = 0; y < screen_h; y++) {
                    for (int x = 0; x < screen_w; x++) {
                        PUT_PIXEL(v, x, y, 7);
                    }
                }
            } else {
                for (int y = 0; y < screen_h; y++) {
                    unsigned char row_pat = g_patterns[g_desktop_pattern][y & 7];
                    for (int x = 0; x < screen_w; x++) {
                        unsigned char c = (row_pat & (0x80 >> (x & 7))) ? 8 : 7;
                        PUT_PIXEL(v, x, y, c);
                    }
                }
            }
        }
        if (!ui_install_mode_requested()) {
            int num_icons = desktop_count();
            for (int i = 0; i < num_icons; i++) {
                desktop_item_t* item = desktop_get(i);
                if (!item) continue;
                unsigned char fallback_bg = desktop_fallback_icon(item);
                icon(v, item->x, item->y, i, fallback_bg,
                     item->display_name, last_click_icon == i,
                     item->kind == DESKTOP_ITEM_SHORTCUT);
            }
        }
        draw_all(v);
        draw_menubar(v);
        if (!uninit_dialog_checked && !ui_install_mode_requested()) {
            uninit_dialog_checked = 1;
            if (part_get_uninit_disk() != NULL) {
                uninit_dialog_active = 1;
            }
        }
        if(uninit_dialog_active) { draw_uninit_disk_dialog(v); }
        if(r0_confirm_active)    { draw_r0_confirm_dialog(v); }
        if(crash_dialog_active)  { draw_crash_dialog(v); }
        if(power_confirm)        { draw_power_dialog(v); }
        draw_desktop_context_menu(v);
    }
    redraw=0;
}


static int in_icon(int mx, int my, int i) {
    desktop_item_t* item = desktop_get(i);
    return item && mx >= item->x - 4 && mx < item->x + 36 &&
           my >= item->y - 4 && my < item->y + 72;
}

static void open_icon_app(int clicked_icon) {
    if (ui_install_mode_requested() || !desktop_get(clicked_icon)) return;
    desktop_open_item(clicked_icon);
    last_click_icon = -1;
    last_click_icon_prev = -1;
    last_click_time_prev = 0;
    icon_pressed = -1;
    icon_dragging = -1;
    redraw = 1;
    need_mr = 1;
}

static void draw_desktop_context_menu(unsigned char* v) {
    if (!desktop_menu_mode) return;
    int width = 150;
    int rows = desktop_menu_mode == 1 ? 2 : 3;
    int height = rows * 18 + 4;
    draw_menu_popup(v, desktop_menu_x, desktop_menu_y, width, height);
    if (desktop_menu_mode == 1) {
        desktop_item_t* item = desktop_get(desktop_menu_item);
        font_draw_text(v, desktop_menu_x + 8, desktop_menu_y + 4,
                       "Delete", item && item->locked ? 8 : 0);
        font_draw_text(v, desktop_menu_x + 8, desktop_menu_y + 22,
                       "Properties", 0);
    } else {
        font_draw_text(v, desktop_menu_x + 8, desktop_menu_y + 4,
                       desktop_is_mutable() ? "New Folder" : "Live: Read-only",
                       desktop_is_mutable() ? 0 : 8);
        font_draw_text(v, desktop_menu_x + 8, desktop_menu_y + 22,
                       "Rearrange Desktop",
                       desktop_is_mutable() ? 0 : 8);
        font_draw_text(v, desktop_menu_x + 8, desktop_menu_y + 40,
                       "Refresh", 0);
    }
}

static int in_cl(int mx, int my, int cx, int cy) { return (mx>=cx && mx<cx+12 && my>=cy && my<cy+12); }
static int in_mi(int mx, int my, int cx, int cy) { return (mx>=cx && mx<cx+12 && my>=cy && my<cy+12); }
static int rs_dir(int mx, int my, int wx, int wy, int ww, int wh) {
    if(mx>=wx+ww-10 && mx<wx+ww && my>=wy+wh-10 && my<wy+wh) return 1;
    return 0;
}
int wm_is_dragging(void){return dragging || resizing || (icon_dragging != -1);}
int wm_get_drag_box(int* x, int* y, int* w, int* h){
    if (drag_active) {
        if (x) *x = drag_bx;
        if (y) *y = drag_by;
        if (w) *w = drag_bw;
        if (h) *h = drag_bh;
        return 1;
    }
    if (rs_active) {
        if (x) *x = rs_bx;
        if (y) *y = rs_by;
        if (w) *w = rs_bw;
        if (h) *h = rs_bh;
        return 1;
    }
    if (icon_drag_active) {
        if (x) *x = icon_drag_bx;
        if (y) *y = icon_drag_by;
        if (w) *w = 36;
        if (h) *h = 70;
        return 1;
    }
    return 0;
}
int wm_need_mouse_reset(void){int r=need_mr;need_mr=0;return r;}
void wm_set_input_event_tick(int tick){input_event_tick=tick;}
int wm_get_input_event_tick(void){
    return input_event_tick >= 0 ? input_event_tick : global_tick;
}

static void get_win_geom(int id, int* rx, int* ry, int* rw, int* rh, int* vis, int* min) {
    *rx = *ry = *rw = *rh = *vis = *min = 0;
    if (id >= WM_DYN_BASE && id < WM_DYN_BASE + WM_DYN_MAX) {
        const wm_dyn_win_t* dw = &g_dyn_wins[id - WM_DYN_BASE];
        if (dw->used) {
            *rx = dw->x;
            *ry = dw->y;
            *rw = dw->client_w + 2;
            *rh = dw->shaded ? 20 : (dw->client_h + 20);
            *vis = 1;
            *min = dw->shaded;
        }
    }
}

void wm_mouse_wheel(int mx, int my, int wheel) {
    if (!wheel) return;

    int volume_x = 0, volume_width = 0;
    notification_volume_span(&volume_x, &volume_width);
    int panel_x = 0, panel_y = 0, panel_w = 0, panel_h = 0;
    notification_volume_panel(&panel_x, &panel_y, &panel_w, &panel_h);
    int over_volume = (my >= 0 && my < 20 &&
                       mx >= volume_x && mx < volume_x + volume_width) ||
                      (volume_popup_open && mx >= panel_x &&
                       mx < panel_x + panel_w && my >= panel_y &&
                       my < panel_y + panel_h);
    if (over_volume) {
        if (audio_is_muted()) {
            if (wheel > 0) audio_toggle_mute();
            else return;
        }
        int volume = audio_get_volume() + (wheel > 0 ? 5 : -5);
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        audio_set_volume(volume);
        wm_sync_volume_state();
        redraw = 1;
        need_mr = 1;
        return;
    }

    for (int z = WM_Z_COUNT - 1; z >= 0; z--) {
        int id = z_order[z];
        int x = 0, y = 0, w = 0, h = 0, visible = 0, minimized = 0;
        get_win_geom(id, &x, &y, &w, &h, &visible, &minimized);
        if (!visible) continue;
        if (mx < x || mx >= x + w || my < y || my >= y + h) continue;
        if (minimized || id < WM_DYN_BASE ||
            id >= WM_DYN_BASE + WM_DYN_MAX) {
            return;
        }
        wm_dyn_win_t* dw = &g_dyn_wins[id - WM_DYN_BASE];
        if (!dw->used) return;
        int client_x = mx - (x + 1);
        int client_y = my - (y + 19);
        if (client_x < 0 || client_x >= dw->client_w ||
            client_y < 0 || client_y >= dw->client_h) {
            return;
        }
        pxe_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = PXE_EV_MOUSE_WHEEL;
        event.win_id = dw->win_id;
        event.x = client_x;
        event.y = client_y;
        event.arg0 = wheel;
        wm_dyn_push_event(dw, &event);
        return;
    }
}

void wm_update(unsigned char* v, int mx, int my, int mb, int mb_press, int mb_release)
{
    static int last_processed_tick = 0;
    int dt = global_tick - last_processed_tick;
    if (dt < 0) dt = 0;
    if (dt > 0) {
        last_processed_tick = global_tick;
        if (opened_msg_timer > 0) {
            opened_msg_timer -= dt;
            if (opened_msg_timer <= 0) {
                opened_msg_timer = 0;
                opened_msg[0] = 0;
                redraw = 1;
                need_mr = 1;
            }
        }
    }

    if (g_pxe_layout_dirty) {
        g_pxe_layout_dirty = 0;
        redraw = 1;
        need_mr = 1;
    }

    int volume_x = 0, volume_width = 0;
    notification_volume_span(&volume_x, &volume_width);
    int panel_x = 0, panel_y = 0, panel_w = 0, panel_h = 0;
    notification_volume_panel(&panel_x, &panel_y, &panel_w, &panel_h);
    int volume_icon_hit =
        mx >= volume_x && mx < volume_x + volume_width && my >= 0 && my < 20;
    int volume_slider_hit =
        mx >= panel_x + 14 && mx < panel_x + 43 &&
        my >= panel_y + 24 && my < panel_y + 108;
    int volume_mute_hit =
        mx >= panel_x + 5 && mx < panel_x + 51 &&
        my >= panel_y + 114 && my < panel_y + 136;

    if (volume_drag_active) {
        if (mb & INPUT_MOUSE_LEFT) {
            volume_set_from_y(my);
            if (g_pxe_present_dirty) {
                g_pxe_present_dirty = 0;
                redraw = 1;
                need_mr = 1;
            }
            return;
        }
        volume_drag_active = 0;
        (void)settings_save();
        if (mb_release) {
            redraw = 1;
            need_mr = 1;
            return;
        }
    }
    if (volume_popup_open && mb_press) {
        int volume_panel_hit =
            mx >= panel_x && mx < panel_x + panel_w &&
            my >= panel_y && my < panel_y + panel_h;
        if (volume_icon_hit) {
            volume_popup_open = 0;
            redraw = 1;
            need_mr = 1;
            return;
        } else if (volume_mute_hit) {
            audio_toggle_mute();
            wm_sync_volume_state();
            (void)settings_save();
            redraw = 1;
            need_mr = 1;
            return;
        } else if (volume_slider_hit) {
            volume_drag_active = 1;
            volume_set_from_y(my);
            redraw = 1;
            need_mr = 1;
            return;
        } else if (volume_panel_hit) {
            redraw = 1;
            need_mr = 1;
            return;
        } else {
            /* Close popup and allow the click to pass through to windows below */
            volume_popup_open = 0;
            redraw = 1;
            need_mr = 1;
        }
    }

    wm_sync_volume_state();

    if (g_pxe_present_dirty && !wm_is_dragging()) {
        g_pxe_present_dirty = 0;
        if (redraw || !v || power_confirm || crash_dialog_active ||
            r0_confirm_active || uninit_dialog_active || menu_open ||
            volume_popup_open || icon_dragging != -1 || opened_msg[0] != 0) {
            redraw = 1;
            need_mr = 1;
        } else {
            bool need_full = false;
            for (int i = 0; i < WM_Z_COUNT; i++) {
                int id = z_order[i];
                if (id < WM_DYN_BASE || id >= WM_DYN_BASE + WM_DYN_MAX) continue;
                wm_dyn_win_t* dw = &g_dyn_wins[id - WM_DYN_BASE];
                if (!dw->used || !dw->present_dirty) continue;

                uint64_t fl = wm_irq_save();
                dw->present_dirty = false;
                int drx = dw->dirty_rx;
                int dry = dw->dirty_ry;
                int drw = dw->dirty_rw;
                int drh = dw->dirty_rh;
                dw->dirty_rx = 0;
                dw->dirty_ry = 0;
                dw->dirty_rw = 0;
                dw->dirty_rh = 0;
                wm_irq_restore(fl);

                if (dw->shaded || !dw->canvas_kvirt) continue;
                int cw = dw->client_w;
                int ch = dw->client_h;
                if (cw > dw->alloc_stride) cw = dw->alloc_stride;
                if (ch > dw->alloc_h) ch = dw->alloc_h;
                if (drw <= 0 || drh <= 0) {
                    drx = 0;
                    dry = 0;
                    drw = cw;
                    drh = ch;
                } else {
                    if (drx < 0) { drw += drx; drx = 0; }
                    if (dry < 0) { drh += dry; dry = 0; }
                    if (drx + drw > cw) drw = cw - drx;
                    if (dry + drh > ch) drh = ch - dry;
                }
                if (drw <= 0 || drh <= 0) continue;

                int sx = dw->x + 1 + drx;
                int sy = dw->y + 19 + dry;
                if (dyn_rect_occluded(i, sx, sy, drw, drh)) {
                    need_full = true;
                    break;
                }
                blit_dyn_client_rect(v, dw, drx, dry, drw, drh);
                wm_push_dirty_rect(sx, sy, drw, drh);
            }
            if (need_full) {
                redraw = 1;
                need_mr = 1;
            }
        }
    }

    // Poll RTC once every 100 ticks (1 Hz / 1 second) to prevent ISA bus stalls
    static int last_rtc_tick = 0;
    static char last_clock_text[32];
    if (global_tick - last_rtc_tick >= 100 || last_s == -1) {
        last_rtc_tick = global_tick;
        int h, m, s;
        get_time(&h, &m, &s);
        last_s = s;
        char next_clock[32];
        format_clock_text(next_clock, sizeof(next_clock));
        if (strcmp(next_clock, last_clock_text) != 0) {
            if (!dragging) {
                if (!redraw && v && !power_confirm && !crash_dialog_active &&
                    !r0_confirm_active && !uninit_dialog_active && !menu_open &&
                    !volume_popup_open && icon_dragging == -1) {
                    int old_width = (int)strlen(last_clock_text) * 8;
                    int new_width = (int)strlen(next_clock) * 8;
                    int width = old_width > new_width ? old_width : new_width;
                    int volume_x = 0, volume_width = 0;
                    notification_volume_span(&volume_x, &volume_width);
                    int cx0 = screen_w - width - 84;
                    if (volume_x - 4 < cx0) cx0 = volume_x - 4;
                    if (cx0 < 24) cx0 = 24;
                    for (int cy = 0; cy < 20; cy++) {
                        for (int cx = cx0; cx < screen_w; cx++) {
                            PUT_PIXEL(v, cx, cy, 15);
                        }
                    }
                    int tx = screen_w - new_width - 10;
                    if (tx < 24) tx = 24;
                    draw_volume_icon(v, volume_x, 2);
                    font_draw_text(v, tx, 2, next_clock, 0);
                    wm_push_dirty_rect(cx0, 0, screen_w - cx0, 20);
                } else {
                    redraw = 1;
                }
            }
            strncpy(last_clock_text, next_clock,
                    sizeof(last_clock_text) - 1);
            last_clock_text[sizeof(last_clock_text) - 1] = '\0';
        }
    }

    if(power_confirm){
        int dw = 220, dh = 80;
        int dx = (screen_w - dw) / 2;
        int dy = (screen_h - dh) / 2;
        int cbx = dx + 20, oby = dy + 48, obx = dx + 110;

        if(mb_press && mx>=cbx && mx<cbx+80 && my>=oby && my<oby+22){power_cancel_pressed=1;redraw=1;need_mr=1;}
        if(mb_release && power_cancel_pressed){power_cancel_pressed=0;
            if(mx>=cbx && mx<cbx+80 && my>=oby && my<oby+22){power_confirm=0;redraw=1;}else{redraw=1;}}
        if(mb_press && mx>=obx && mx<obx+80 && my>=oby && my<oby+22){power_ok_pressed=1;redraw=1;need_mr=1;}
        if(mb_release && power_ok_pressed){power_ok_pressed=0;
            if(mx>=obx && mx<obx+80 && my>=oby && my<oby+22){
                if(power_confirm==1) {
                    sys_reboot();
                } else {
                    sys_shutdown();
                }
            }else{redraw=1;}}
        if(redraw) { need_mr=1; }
        return;
    }

    if (crash_dialog_active) {
        int dw = 380, dh = 144;
        int dx = (screen_w - dw) / 2;
        int dy = (screen_h - dh) / 2;
        int obx = dx + dw - 96, oby = dy + dh - 32;

        if (mb_press && mx >= obx && mx < obx + 80 && my >= oby && my < oby + 22) {
            crash_ok_pressed = 1;
            redraw = 1;
            need_mr = 1;
        }
        if (mb_release && crash_ok_pressed) {
            crash_ok_pressed = 0;
            if (mx >= obx && mx < obx + 80 && my >= oby && my < oby + 22) {
                crash_dialog_active = 0;
            }
            redraw = 1;
            need_mr = 1;
        }
        if (keyboard_get_key() == '\n') {
            crash_dialog_active = 0;
            crash_ok_pressed = 0;
            redraw = 1;
            need_mr = 1;
        }
        return;
    }

    if (r0_confirm_active) {
        int dw = 380, dh = 124;
        int dx = (screen_w - dw) / 2;
        int dy = (screen_h - dh) / 2;
        int cbx = dx + 40, obx = dx + 210, oby = dy + 86;

        if (mb_press && mx >= cbx && mx < cbx + 110 && my >= oby && my < oby + 22) {
            r0_cancel_pressed = 1;
            redraw = 1;
            need_mr = 1;
        }
        if (mb_release && r0_cancel_pressed) {
            r0_cancel_pressed = 0;
            if (mx >= cbx && mx < cbx + 110 && my >= oby && my < oby + 22) {
                r0_confirm_active = 0;
            }
            redraw = 1;
            need_mr = 1;
        }
        if (mb_press && mx >= obx && mx < obx + 130 && my >= oby && my < oby + 22) {
            r0_run_pressed = 1;
            redraw = 1;
            need_mr = 1;
        }
        if (mb_release && r0_run_pressed) {
            r0_run_pressed = 0;
            if (mx >= obx && mx < obx + 130 && my >= oby && my < oby + 22) {
                char launch_path[128];
                char launch_arg1[128];
                int has_arg1 = r0_confirm_has_arg1;
                strcpy(launch_path, r0_confirm_path);
                strcpy(launch_arg1, r0_confirm_arg1);
                r0_confirm_active = 0;
                redraw = 1;
                need_mr = 1;
                const char* argv_local[2];
                argv_local[0] = launch_path;
                argv_local[1] = has_arg1 ? launch_arg1 : NULL;
                pxe_spawn_file(launch_path, has_arg1 ? 2 : 1, argv_local, NULL, true);
            } else {
                redraw = 1;
                need_mr = 1;
            }
        }
        return;
    }

    if(uninit_dialog_active){
        int dw = 260, dh = 86;
        int dx = (screen_w - dw) / 2;
        int dy = (screen_h - dh) / 2;
        int cbx = dx + 20, obx = dx + 140, oby = dy + 52;

        if(mb_press && mx>=cbx && mx<cbx+100 && my>=oby && my<oby+22){uninit_ignore_pressed=1;need_mr=1;}
        if(mb_release && uninit_ignore_pressed){uninit_ignore_pressed=0;
            if(mx>=cbx && mx<cbx+100 && my>=oby && my<oby+22){
                part_clear_uninit_disk();
                uninit_dialog_active=0;
                redraw=1;
            } else {redraw=1;}
        }
        if(mb_press && mx>=obx && mx<obx+100 && my>=oby && my<oby+22){uninit_init_pressed=1;need_mr=1;}
        if(mb_release && uninit_init_pressed){uninit_init_pressed=0;
            if(mx>=obx && mx<obx+100 && my>=oby && my<oby+22){
                blkdev_t* d = part_get_uninit_disk();
                if (d) {
                    part_initialize_disk(d);
                    part_clear_uninit_disk();
                    vfs_init();
                }
                uninit_dialog_active=0;
                redraw=1;
                show_opened("Disk Initialized");
            } else {redraw=1;}
        }
        if(redraw) { need_mr=1; }
        return;
    }

    int active_win = top_visible_win_id();
    wm_dyn_win_t* pxe_mb_win = (!ui_install_mode_requested()) ? get_active_pxe_menubar_win() : NULL;

    // Menu bar interaction
    if (menu_open && mb_press) {
        bool inst_mode = ui_install_mode_requested();
        if (menu_index == 4) { // Apple Logo
            int item_count = inst_mode ? 2 : 5;
            if (mx >= 4 && mx < 4 + 172 && my >= 20 && my < 20 + item_count*18 + 4) {
                int item = (my - 24) / 18;
                if (inst_mode) {
                    if (item == 0) { power_confirm = 1; }
                    if (item == 1) { power_confirm = 2; }
                } else if (item >= 0 && item < 5) {
                    if (item == 0) { pxe_launch_builtin_app("about.pxe", NULL, false); }
                    if (item == 1) { pxe_launch_builtin_app("sysinfo.pxe", NULL, true); }
                    if (item == 2) { pxe_launch_builtin_app("taskmgr.pxe", NULL, true); }
                    if (item == 3) { power_confirm = 1; }
                    if (item == 4) { power_confirm = 2; }
                }
            }
        } else if (pxe_mb_win && !inst_mode) {
            const pxe_menu_bar_t* mb_bar = &pxe_mb_win->menu_bar;
            if (menu_index >= 0 && menu_index < mb_bar->menu_count && menu_index < PXE_MAX_MENUS) {
                const pxe_menu_t* pm = &mb_bar->menus[menu_index];
                int mx_pos = 0, mw = 0;
                get_pxe_menu_span(mb_bar, menu_index, &mx_pos, &mw);
                int cnt = pm->item_count;
                if (cnt > PXE_MAX_MENU_ITEMS) cnt = PXE_MAX_MENU_ITEMS;
                if (cnt > 0 && mx >= mx_pos - 4 && mx < mx_pos - 4 + 176 &&
                    my >= 20 && my < 20 + cnt * 18 + 4) {
                    int item = (my - 24) / 18;
                    if (item >= 0 && item < cnt && pm->items[item].enabled) {
                        pxe_event_t ev;
                        memset(&ev, 0, sizeof(ev));
                        ev.type = PXE_EV_MENU;
                        ev.win_id = pxe_mb_win->win_id;
                        ev.arg0 = menu_index;
                        ev.arg1 = item;
                        wm_dyn_push_event(pxe_mb_win, &ev);
                    }
                }
            }
        } else if (menu_index == 2 && !inst_mode) { // View
            if (mx >= 106 && mx < 106 + 160 && my >= 20 && my < 20 + 1*18 + 4) {
                if (desktop_is_mutable()) {
                    desktop_rearrange();
                    show_opened("Desktop Rearranged");
                } else {
                    wm_notify_banner("Live Desktop is read-only");
                }
            }
        }
        menu_open = 0;
        redraw = 1;
        need_mr = 1;
        return;
    }

    if(mb_press && my < 20) {
        bool inst_mode = ui_install_mode_requested();
        int volume_x = 0, volume_width = 0;
        notification_volume_span(&volume_x, &volume_width);
        if (mx >= volume_x && mx < volume_x + volume_width) {
            volume_popup_open = !volume_popup_open;
            volume_drag_active = 0;
            redraw = 1;
            need_mr = 1;
        } else if (mx >= 4 && mx < 24) {
            menu_open = (menu_open && menu_index == 4) ? 0 : 1;
            menu_index = 4;
            redraw = 1;
            need_mr = 1;
        } else if (!inst_mode && pxe_mb_win) {
            const pxe_menu_bar_t* mb_bar = &pxe_mb_win->menu_bar;
            int hit = -1;
            for (int m = 0; m < mb_bar->menu_count && m < PXE_MAX_MENUS; m++) {
                int mx_pos = 0, mw = 0;
                get_pxe_menu_span(mb_bar, m, &mx_pos, &mw);
                if (mx >= mx_pos - 4 && mx < mx_pos + mw - 4) {
                    hit = m;
                    break;
                }
            }
            if (hit != -1) {
                menu_open = (menu_open && menu_index == hit) ? 0 : 1;
                menu_index = hit;
                redraw = 1;
                need_mr = 1;
            } else {
                menu_open = 0;
                redraw = 1;
            }
        } else if (!inst_mode && mx >= 30 && mx < 62) { menu_open = (menu_open && menu_index == 0) ? 0 : 1; menu_index = 0; redraw = 1; need_mr = 1; }
        else if (!inst_mode && mx >= 70 && mx < 102) { menu_open = (menu_open && menu_index == 1) ? 0 : 1; menu_index = 1; redraw = 1; need_mr = 1; }
        else if (!inst_mode && mx >= 110 && mx < 142) { menu_open = (menu_open && menu_index == 2) ? 0 : 1; menu_index = 2; redraw = 1; need_mr = 1; }
        else { menu_open = 0; redraw = 1; }
        return;
    }

    // Mouse motion delivery to dynamic PXE windows
    if ((mx != g_last_pxe_mx || my != g_last_pxe_my) && !wm_is_dragging() && !menu_open) {
        g_last_pxe_mx = mx;
        g_last_pxe_my = my;
        int target_id = (g_pxe_capture_win >= WM_DYN_BASE) ? g_pxe_capture_win : active_win;
        if (target_id >= WM_DYN_BASE && target_id < WM_DYN_BASE + WM_DYN_MAX) {
            wm_dyn_win_t* dw = &g_dyn_wins[target_id - WM_DYN_BASE];
            if (dw->used && !dw->shaded) {
                int cx = mx - (dw->x + 1);
                int cy = my - (dw->y + 19);
                if (g_pxe_capture_win == target_id ||
                    (cx >= 0 && cx < dw->client_w && cy >= 0 && cy < dw->client_h)) {
                    pxe_event_t ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.type = PXE_EV_MOUSE_MOVE;
                    ev.win_id = dw->win_id;
                    ev.x = cx;
                    ev.y = cy;
                    ev.buttons = (uint32_t)mb;
                    wm_dyn_push_event(dw, &ev);
                }
            }
        }
    }

    if (mb_release) {
        if (g_pxe_capture_win >= WM_DYN_BASE && g_pxe_capture_win < WM_DYN_BASE + WM_DYN_MAX) {
            wm_dyn_win_t* dw = &g_dyn_wins[g_pxe_capture_win - WM_DYN_BASE];
            if (dw->used) {
                pxe_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = PXE_EV_MOUSE_UP;
                ev.win_id = dw->win_id;
                ev.x = mx - (dw->x + 1);
                ev.y = my - (dw->y + 19);
                ev.buttons = (uint32_t)mb_release;
                wm_dyn_push_event(dw, &ev);
            }
            g_pxe_capture_win = -1;
        }
    }

    // Keyboard input to active window (plus global shortcuts)
    if (keyboard_consume_taskmgr_hotkey() && !ui_install_mode_requested()) {
        if (menu_open) {
            menu_open = 0;
            redraw = 1;
            need_mr = 1;
        }
        pxe_launch_builtin_app("taskmgr.pxe", NULL, true);
    }
    int key = keyboard_get_key();
    while (key != 0) {
        if (key == 27 && keyboard_get_ctrl() && keyboard_get_shift() &&
            !ui_install_mode_requested()) {
            if (menu_open) {
                menu_open = 0;
                redraw = 1;
                need_mr = 1;
            }
            pxe_launch_builtin_app("taskmgr.pxe", NULL, true);
            key = keyboard_get_key();
            continue;
        }
        if (active_win >= WM_DYN_BASE && active_win < WM_DYN_BASE + WM_DYN_MAX) {
            wm_dyn_win_t* dw = &g_dyn_wins[active_win - WM_DYN_BASE];
            if (dw->used && !dw->shaded) {
                int pxe_key = key;
                if (pxe_key == INPUT_CODE_ENTER) {
                    pxe_key = PXE_KEY_ENTER;
                } else if (pxe_key >= INPUT_CODE_UP && pxe_key <= INPUT_CODE_DELETE) {
                    pxe_key = PXE_KEY_UP + (pxe_key - INPUT_CODE_UP);
                }
                pxe_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = PXE_EV_KEY_DOWN;
                ev.win_id = dw->win_id;
                ev.key = pxe_key;
                ev.modifiers = keyboard_get_ctrl() ? PXE_MOD_CTRL : 0;
                wm_dyn_push_event(dw, &ev);
            }
        }
        key = keyboard_get_key();
    }

    if (desktop_poll()) {
        redraw = 1;
        need_mr = 1;
    }

    if (desktop_menu_mode && mb_press) {
        int rows = desktop_menu_mode == 1 ? 2 : 3;
        if (mx >= desktop_menu_x && mx < desktop_menu_x + 150 &&
            my >= desktop_menu_y &&
            my < desktop_menu_y + rows * 18 + 4) {
            int item = (my - desktop_menu_y - 4) / 18;
            if (desktop_menu_mode == 1) {
                if (item == 0) {
                    char error[96] = { 0 };
                    int result = desktop_delete_item(desktop_menu_item,
                                                     error, sizeof(error));
                    if (result != VFS_OK) {
                        wm_notify_banner(error[0] ? error :
                                         "Unable to delete desktop item");
                    } else {
                        show_opened("Moved to Trash");
                    }
                } else if (item == 1) {
                    desktop_item_t* selected = desktop_get(desktop_menu_item);
                    if (selected) wm_notify_banner(selected->path);
                }
            } else if (item == 0) {
                char error[96] = { 0 };
                if (desktop_create_folder(error, sizeof(error)) != VFS_OK) {
                    wm_notify_banner(error[0] ? error :
                                     "Unable to create desktop folder");
                } else {
                    show_opened("Folder Created");
                }
            } else if (item == 1) {
                if (desktop_is_mutable()) {
                    desktop_rearrange();
                    show_opened("Desktop Rearranged");
                } else {
                    wm_notify_banner("Live Desktop is read-only");
                }
            } else if (item == 2) {
                if (vfs_poll_media()) wm_notify_media_changed();
                desktop_refresh();
                show_opened("Desktop Refreshed");
            }
        }
        desktop_menu_mode = 0;
        desktop_menu_item = -1;
        redraw = 1;
        need_mr = 1;
        return;
    }

    // Handle active desktop icon drag (fast outline box)
    if (icon_dragging != -1) {
        if (mb & 1) {
            int nix = mx - icon_drag_ox;
            int niy = my - icon_drag_oy;
            if (nix < 0) nix = 0;
            if (nix + 36 > screen_w) nix = screen_w - 36;
            if (niy < 20) niy = 20;
            if (niy + 70 > screen_h) niy = screen_h - 70;
            icon_drag_bx = nix;
            icon_drag_by = niy;
            icon_drag_active = 1;
        } else {
            if (icon_drag_active) {
                (void)desktop_set_position(icon_dragging, icon_drag_bx,
                                           icon_drag_by);
            }
            icon_dragging = -1;
            icon_drag_active = 0;
            redraw = 1;
            need_mr = 1;
        }
        return;
    }

    // Handle pending desktop icon press (transition to drag or release to select)
    if (icon_pressed != -1) {
        desktop_item_t* pressed_item = desktop_get(icon_pressed);
        if (!pressed_item) {
            icon_pressed = -1;
            need_mr = 1;
            return;
        }
        if (mb & 1) {
            int dx = mx - (pressed_item->x + icon_press_ox);
            int dy = my - (pressed_item->y + icon_press_oy);
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > 6 || dy > 6) {
                if (!desktop_is_mutable()) {
                    icon_pressed = -1;
                    icon_press_moved = 1;
                    redraw = 1;
                    need_mr = 1;
                    return;
                }
                icon_dragging = icon_pressed;
                icon_drag_ox = icon_press_ox;
                icon_drag_oy = icon_press_oy;
                icon_drag_bx = pressed_item->x;
                icon_drag_by = pressed_item->y;
                icon_drag_active = 1;
                icon_pressed = -1;
                icon_press_moved = 1;
                return;
            }
        } else {
            if (!icon_press_moved && in_icon(mx, my, icon_pressed)) {
                if (last_click_icon_prev == icon_pressed &&
                    (wm_get_input_event_tick() - last_click_time_prev) <
                    WM_DOUBLE_CLICK_TICKS) {
                    open_icon_app(icon_pressed);
                    return;
                } else {
                    last_click_icon_prev = icon_pressed;
                    last_click_time_prev = wm_get_input_event_tick();
                    redraw = 1;
                }
            }
            icon_pressed = -1;
            need_mr = 1;
        }
    }

    // Handle active window resize (fast outline box)
    if (resizing) {
        if (mb & 1) {
            int nw = 0, nh = 0, rx = 0, ry = 0;
            if (rs_win_id >= WM_DYN_BASE && rs_win_id < WM_DYN_BASE + WM_DYN_MAX) {
                wm_dyn_win_t* dw = &g_dyn_wins[rs_win_id - WM_DYN_BASE];
                rx = dw->x;
                ry = dw->y;
                nw = mx - rx;
                nh = my - ry;
                if (nw < dw->min_w + 2) nw = dw->min_w + 2;
                if (nh < dw->min_h + 20) nh = dw->min_h + 20;
                if (nw > dw->alloc_stride + 2) nw = dw->alloc_stride + 2;
                if (nh > dw->alloc_h + 20) nh = dw->alloc_h + 20;
            }

            if (rx + nw > screen_w) nw = screen_w - rx;
            if (ry + nh > screen_h) nh = screen_h - ry;

            rs_bx = rx; rs_by = ry; rs_bw = nw; rs_bh = nh;
            rs_active = 1;
        } else {
            if (rs_active) {
                if (rs_win_id >= WM_DYN_BASE && rs_win_id < WM_DYN_BASE + WM_DYN_MAX) {
                    wm_dyn_win_t* dw = &g_dyn_wins[rs_win_id - WM_DYN_BASE];
                    if (dw->used) {
                        int ncw = rs_bw - 2;
                        int nch = rs_bh - 20;
                        if (ncw < dw->min_w) ncw = dw->min_w;
                        if (nch < dw->min_h) nch = dw->min_h;
                        if (ncw > dw->alloc_stride) ncw = dw->alloc_stride;
                        if (nch > dw->alloc_h) nch = dw->alloc_h;
                        if (ncw != dw->client_w || nch != dw->client_h) {
                            dw->client_w = ncw;
                            dw->client_h = nch;
                            wm_dyn_push_simple(dw, PXE_EV_RESIZE);
                            wm_dyn_push_simple(dw, PXE_EV_REDRAW);
                        }
                    }
                }
            }
            resizing = 0;
            rs_active = 0;
            rs_win_id = -1;
            dgw = 0;
            redraw = 1;
            need_mr = 1;
        }
        return;
    }

    // Handle active window title-bar drag (fast outline box)
    if (dragging) {
        if (mb & 1) {
            int nwx = mx - dgo;
            int nwy = my - dgoy;
            int nww = 100, nwh = 20;
            int win_id = dgw - 1;
            if (win_id >= WM_DYN_BASE && win_id < WM_DYN_BASE + WM_DYN_MAX) {
                wm_dyn_win_t* dw = &g_dyn_wins[win_id - WM_DYN_BASE];
                nww = dw->client_w + 2;
                nwh = dw->shaded ? 20 : (dw->client_h + 20);
            }
            if (nwx < 0) nwx = 0;
            if (nwy < 20) nwy = 20;
            if (nwx + nww > screen_w) nwx = screen_w - nww;
            if (nwy + nwh > screen_h) nwy = screen_h - nwh;

            drag_bx = nwx;
            drag_by = nwy;
            drag_bw = nww;
            drag_bh = nwh;
            drag_active = 1;
        } else {
            if (drag_active) {
                int win_id = dgw - 1;
                if (win_id >= WM_DYN_BASE && win_id < WM_DYN_BASE + WM_DYN_MAX) {
                    wm_dyn_win_t* dw = &g_dyn_wins[win_id - WM_DYN_BASE];
                    if (dw->used) {
                        dw->x = drag_bx;
                        dw->y = drag_by;
                    }
                }
            }
            dragging = 0;
            drag_active = 0;
            dgw = 0;
            redraw = 1;
            need_mr = 1;
        }
        return;
    }

    // Handle pending Close / Minimize button releases
    if (mb_release && clp) {
        if (in_cl(mx, my, clcx, clcy)) {
            int win_id = clw - 1;
            if (win_id >= WM_DYN_BASE && win_id < WM_DYN_BASE + WM_DYN_MAX) {
                wm_dyn_win_t* dw = &g_dyn_wins[win_id - WM_DYN_BASE];
                if (dw->used) {
                    wm_dyn_push_simple(dw, PXE_EV_CLOSE);
                }
            }
        }
        clp = 0;
        clw = 0;
        redraw = 1;
        need_mr = 1;
        return;
    }
    if (mb_release && mip) {
        int win_id = miw - 1;
        if (win_id >= WM_DYN_BASE && win_id < WM_DYN_BASE + WM_DYN_MAX) {
            wm_dyn_win_t* dw = &g_dyn_wins[win_id - WM_DYN_BASE];
            int mcx = dw->x + (dw->client_w + 2) - 16;
            int mcy = dw->y + 4;
            if (dw->used && in_mi(mx, my, mcx, mcy)) {
                dw->shaded = !dw->shaded;
            }
        }
        mip = 0;
        miw = 0;
        redraw = 1;
        need_mr = 1;
        return;
    }

    // Handle new mouse button press: check topmost window first, then desktop icons
    if (mb_press && !clp && !mip) {
        int top_id = -1;
        int win_x = 0, win_y = 0, win_w = 0, win_h = 0, win_min = 0;
        for (int i = WM_Z_COUNT - 1; i >= 0; i--) {
            int id = z_order[i];
            int rx, ry, rw, rh, vis, min;
            get_win_geom(id, &rx, &ry, &rw, &rh, &vis, &min);
            if (vis && mx >= rx && mx < rx + rw && my >= ry && my < ry + rh) {
                top_id = id;
                win_x = rx; win_y = ry; win_w = rw; win_h = rh; win_min = min;
                break;
            }
        }

        if (top_id != -1) {
            bring_to_top(top_id);
            if (last_click_icon != -1 || last_click_icon_prev != -1) {
                last_click_icon = -1;
                last_click_icon_prev = -1;
            }
            redraw = 1;
            need_mr = 1;

            bool no_close = false;
            bool no_drag = false;
            if (top_id >= WM_DYN_BASE && top_id < WM_DYN_BASE + WM_DYN_MAX) {
                uint32_t wflags = g_dyn_wins[top_id - WM_DYN_BASE].flags;
                no_close = (wflags & PXE_WIN_NO_CLOSE) != 0;
                if (ui_install_mode_requested() && no_close) {
                    no_drag = true;
                }
            }

            // 1. Check Close & Minimize buttons on title bar
            if (!no_close) {
                int cx = win_x + 4, cy = win_y + 3;
                if (in_cl(mx, my, cx, cy)) {
                    clp = 1; clw = top_id + 1; clcx = cx; clcy = cy;
                    cl_btn(v, cx, cy, 1);
                    need_mr = 1;
                    return;
                }
                int mcx = win_x + win_w - 16, mcy = win_y + 4;
                if (in_mi(mx, my, mcx, mcy)) {
                    mip = 1; miw = top_id + 1;
                    mi_btn(v, mcx, mcy, 1);
                    need_mr = 1;
                    return;
                }
            }

            // 2. Check Title Bar for Window Dragging (full 19px height)
            if (!no_drag && my >= win_y && my < win_y + 19) {
                dragging = 1;
                drag_active = 0;
                dgw = top_id + 1;
                dgo = mx - win_x;
                dgoy = my - win_y;
                drag_bx = win_x;
                drag_by = win_y;
                drag_bw = win_w;
                drag_bh = win_h;
                return;
            }

            // 3. If window is not minimized, check Resize Corner or Content Click
            if (!win_min) {
                int real_h = win_h;
                bool can_resize = false;
                if (top_id >= WM_DYN_BASE && top_id < WM_DYN_BASE + WM_DYN_MAX) {
                    can_resize = (g_dyn_wins[top_id - WM_DYN_BASE].flags & PXE_WIN_RESIZABLE) != 0;
                }
                if (can_resize && rs_dir(mx, my, win_x, win_y, win_w, real_h)) {
                    resizing = 1;
                    rs_active = 0;
                    rs_win_id = top_id;
                    rs_bx = win_x;
                    rs_by = win_y;
                    rs_bw = win_w;
                    rs_bh = real_h;
                    dgw = 0;
                    return;
                }

                if (top_id >= WM_DYN_BASE && top_id < WM_DYN_BASE + WM_DYN_MAX) {
                    wm_dyn_win_t* dw = &g_dyn_wins[top_id - WM_DYN_BASE];
                    if (dw->used) {
                        g_pxe_capture_win = top_id;
                        pxe_event_t ev;
                        memset(&ev, 0, sizeof(ev));
                        ev.type = PXE_EV_MOUSE_DOWN;
                        ev.win_id = dw->win_id;
                        ev.x = mx - (dw->x + 1);
                        ev.y = my - (dw->y + 19);
                        ev.buttons = (uint32_t)(mb ? mb : mb_press);
                        wm_dyn_push_event(dw, &ev);
                    }
                }
            }
            return;
        }

        // Clicked on desktop background (not inside any window)
        if (!ui_install_mode_requested()) {
            int total_icons = desktop_count();
            int clicked_icon = -1;
            for (int i = total_icons - 1; i >= 0; i--) {
                if (in_icon(mx, my, i)) {
                    clicked_icon = i;
                    break;
                }
            }
            if (mb_press & INPUT_MOUSE_RIGHT) {
                desktop_menu_mode = clicked_icon != -1 ? 1 : 2;
                desktop_menu_item = clicked_icon;
                desktop_menu_x = mx;
                desktop_menu_y = my;
                int menu_rows = desktop_menu_mode == 1 ? 2 : 3;
                if (desktop_menu_x + 150 > screen_w) {
                    desktop_menu_x = screen_w - 150;
                }
                if (desktop_menu_y + menu_rows * 18 + 4 > screen_h) {
                    desktop_menu_y = screen_h - (menu_rows * 18 + 4);
                }
                if (desktop_menu_x < 4) desktop_menu_x = 4;
                if (desktop_menu_y < 22) desktop_menu_y = 22;
                redraw = 1;
                need_mr = 1;
                return;
            }
            if (clicked_icon != -1) {
                desktop_item_t* clicked_item = desktop_get(clicked_icon);
                if (!clicked_item) return;
                if (last_click_icon_prev == clicked_icon &&
                    (wm_get_input_event_tick() - last_click_time_prev) <
                    WM_DOUBLE_CLICK_TICKS) {
                    open_icon_app(clicked_icon);
                    return;
                }
                icon_pressed = clicked_icon;
                icon_press_ox = mx - clicked_item->x;
                icon_press_oy = my - clicked_item->y;
                icon_press_moved = 0;
                last_click_icon = clicked_icon;
                redraw = 1;
                need_mr = 1;
                return;
            }
        }

        if (last_click_icon != -1 || last_click_icon_prev != -1) {
            last_click_icon = -1;
            last_click_icon_prev = -1;
            redraw = 1;
            need_mr = 1;
        }
    }

    if(redraw) need_mr=1;
}

/* =========================================================================
 * Public PXE Dynamic Compositor & Notification API
 * ========================================================================= */

static void wm_pxe_destroy_slot(int slot) {
    if (slot < 0 || slot >= WM_DYN_MAX) return;
    uint64_t flags = wm_irq_save();
    wm_dyn_win_t* dw = &g_dyn_wins[slot];
    if (!dw->used) {
        wm_irq_restore(flags);
        return;
    }

    int win_id = dw->win_id;
    uint32_t owner_pid = dw->owner_pid;
    phys_addr_t cphys = dw->canvas_phys;
    size_t cpages = dw->canvas_pages;
    uint64_t uvirt = dw->canvas_uvirt;

    memset(dw, 0, sizeof(*dw));
    if (g_pxe_capture_win == win_id) g_pxe_capture_win = -1;
    g_pxe_layout_dirty = 1;
    g_pxe_present_dirty = 1;
    wm_irq_restore(flags);

    process_t* owner = process_find_by_pid(owner_pid);
    if (owner) {
        if (uvirt && owner->pml4 && owner->pml4 != vmm_get_kernel_pml4()) {
            for (size_t pg = 0; pg < cpages; pg++) {
                vmm_unmap_page(owner->pml4, uvirt + pg * PAGE_SIZE);
            }
        }
        if (owner->mem_pages_used >= cpages) {
            owner->mem_pages_used -= cpages;
        } else {
            owner->mem_pages_used = 0;
        }
    }

    if (cphys && cpages > 0) {
        pmm_free_pages(cphys, cpages);
    }

    scheduler_wake_win_waiters(win_id);
    g_pxe_present_dirty = 1;
}

int wm_pxe_win_create(struct process* proc, const pxe_win_desc_t* desc, pxe_win_surface_t* out_surf) {
    if (!proc || !desc || !out_surf) return -1;

    uint32_t win_flags = desc->flags;
    if (proc->pxe_flags & PXE_FLAG_NO_CLOSE) {
        win_flags |= PXE_WIN_NO_CLOSE;
    }
    bool install_locked = ui_install_mode_requested() &&
                          ((win_flags & PXE_WIN_NO_CLOSE) != 0);
    if (install_locked) {
        win_flags |= PXE_WIN_NO_CLOSE;
        win_flags &= ~PXE_WIN_RESIZABLE;
    }

    uint32_t format = (desc->format == PXE_CANVAS_RGB32) ? PXE_CANVAS_RGB32 : PXE_CANVAS_PAL8;
    int max_cw = (screen_w > 64) ? (screen_w - 2) : 638;
    int max_ch = (screen_h > 64) ? (screen_h - 40) : 440;
    int cw = install_locked ? max_cw : desc->w;
    int ch = install_locked ? max_ch : desc->h;
    if (cw < 64) cw = 64;
    if (cw > max_cw) cw = max_cw;
    if (ch < 32) ch = 32;
    if (ch > max_ch) ch = max_ch;

    int min_w = (desc->min_w > 64) ? desc->min_w : 64;
    int min_h = (desc->min_h > 32) ? desc->min_h : 32;
    if (min_w > cw) min_w = cw;
    if (min_h > ch) min_h = ch;

    int res_w = (screen_w > cw) ? screen_w : cw;
    int res_h = (screen_h > ch) ? screen_h : ch;
    if (res_w < 1280) res_w = 1280;
    if (res_h < 1024) res_h = 1024;

    int alloc_stride = (win_flags & PXE_WIN_RESIZABLE) ? res_w : cw;
    int alloc_h      = (win_flags & PXE_WIN_RESIZABLE) ? res_h : ch;
    int bpp_bytes    = (format == PXE_CANVAS_RGB32) ? 4 : 1;
    uint64_t need64  = (uint64_t)(unsigned int)alloc_stride *
                       (uint64_t)(unsigned int)alloc_h *
                       (uint64_t)(unsigned int)bpp_bytes;
    if (need64 > NEKO_SHM_STRIDE) {
        /* Fallback clamp within SHM stride */
        alloc_stride = cw;
        alloc_h = ch;
        need64 = (uint64_t)(unsigned int)alloc_stride *
                 (uint64_t)(unsigned int)alloc_h *
                 (uint64_t)(unsigned int)bpp_bytes;
        if (need64 > NEKO_SHM_STRIDE) return -1;
    }
    int pitch        = alloc_stride * bpp_bytes;
    size_t total_sz  = (size_t)pitch * (size_t)alloc_h;
    size_t pages     = (total_sz + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t irq_flags = wm_irq_save();
    int slot = -1;
    for (int i = 0; i < WM_DYN_MAX; i++) {
        if (!g_dyn_wins[i].used) {
            slot = i;
            memset(&g_dyn_wins[i], 0, sizeof(g_dyn_wins[i]));
            g_dyn_wins[i].used = true;
            g_dyn_wins[i].win_id = WM_DYN_BASE + i;
            g_dyn_wins[i].owner_pid = proc->pid;
            break;
        }
    }
    wm_irq_restore(irq_flags);
    if (slot < 0) return -1;

    phys_addr_t cphys = pmm_alloc_pages(pages);
    if (!cphys) {
        g_dyn_wins[slot].used = false;
        return -1;
    }

    void* kvirt = phys_to_virt(cphys);
    if (format == PXE_CANVAS_RGB32) {
        uint32_t* p32 = (uint32_t*)kvirt;
        size_t count = (pages * PAGE_SIZE) / 4;
        for (size_t k = 0; k < count; k++) p32[k] = 0x00C0C0C0u;
    } else {
        memset(kvirt, PXE_COL_LIGHT_GRAY, pages * PAGE_SIZE);
    }

    uint64_t uvirt = 0;
    if (proc->exec_ring == 3) {
        uvirt = proc->next_shm_virt;
        proc->next_shm_virt += NEKO_SHM_STRIDE;
        if (!process_map_shared(proc, cphys, uvirt, pages * PAGE_SIZE)) {
            pmm_free_pages(cphys, pages);
            g_dyn_wins[slot].used = false;
            return -1;
        }
    }
    proc->mem_pages_used += pages;

    static int s_cascade = 0;
    int ow = cw + 2;
    int oh = ch + 20;
    int wx_pos = desc->x;
    int wy_pos = desc->y;
    if (install_locked) {
        wx_pos = 0;
        wy_pos = 20;
    } else if (win_flags & PXE_WIN_CENTERED) {
        wx_pos = (screen_w - ow) / 2;
        wy_pos = 20 + (screen_h - 20 - oh) / 2;
    } else if (wx_pos < 0 || wy_pos < 0) {
        int step = s_cascade % 6;
        s_cascade++;
        wx_pos = 72 + step * 24;
        wy_pos = 42 + step * 22;
    }
    if (wx_pos + ow > screen_w) wx_pos = screen_w - ow;
    if (wy_pos + oh > screen_h) wy_pos = screen_h - oh;
    if (wx_pos < 0) wx_pos = 0;
    if (wy_pos < 20) wy_pos = 20;

    wm_dyn_win_t* dw = &g_dyn_wins[slot];
    dw->x = wx_pos;
    dw->y = wy_pos;
    dw->client_w = cw;
    dw->client_h = ch;
    dw->min_w = min_w;
    dw->min_h = min_h;
    dw->alloc_stride = alloc_stride;
    dw->alloc_h = alloc_h;
    dw->pitch = pitch;
    dw->format = format;
    dw->flags = win_flags;
    dw->shaded = 0;
    dw->canvas_phys = cphys;
    dw->canvas_pages = pages;
    dw->canvas_kvirt = kvirt;
    dw->canvas_uvirt = uvirt;
    strncpy(dw->title, desc->title, sizeof(dw->title) - 1);
    dw->title[sizeof(dw->title) - 1] = '\0';

    out_surf->pixels = (proc->exec_ring == 3) ? (void*)(uintptr_t)uvirt : kvirt;
    out_surf->width  = cw;
    out_surf->height = ch;
    out_surf->stride = alloc_stride;
    out_surf->pitch  = pitch;
    out_surf->format = format;

    irq_flags = wm_irq_save();
    bring_to_top(dw->win_id);
    wm_irq_restore(irq_flags);

    wm_dyn_push_simple(dw, PXE_EV_REDRAW);
    wm_notify_banner(dw->title);
    g_pxe_layout_dirty = 1;
    g_pxe_present_dirty = 1;
    return dw->win_id;
}

int wm_pxe_win_destroy(uint32_t owner_pid, int win_id) {
    int slot = win_id - WM_DYN_BASE;
    if (slot < 0 || slot >= WM_DYN_MAX) return -1;
    if (!g_dyn_wins[slot].used || g_dyn_wins[slot].owner_pid != owner_pid) return -1;
    wm_pxe_destroy_slot(slot);
    return 0;
}

void wm_pxe_destroy_all_for_pid(uint32_t pid) {
    if (pid == 0) return;
    for (int i = 0; i < WM_DYN_MAX; i++) {
        if (g_dyn_wins[i].used && g_dyn_wins[i].owner_pid == pid) {
            wm_pxe_destroy_slot(i);
        }
    }
}

int wm_pxe_win_set_title(uint32_t owner_pid, int win_id, const char* title) {
    int slot = win_id - WM_DYN_BASE;
    if (slot < 0 || slot >= WM_DYN_MAX || !title) return -1;
    wm_dyn_win_t* dw = &g_dyn_wins[slot];
    if (!dw->used || dw->owner_pid != owner_pid) return -1;
    strncpy(dw->title, title, sizeof(dw->title) - 1);
    dw->title[sizeof(dw->title) - 1] = '\0';
    g_pxe_layout_dirty = 1;
    g_pxe_present_dirty = 1;
    return 0;
}

int wm_pxe_win_set_menu(uint32_t owner_pid, int win_id, const pxe_menu_bar_t* menu) {
    int slot = win_id - WM_DYN_BASE;
    if (slot < 0 || slot >= WM_DYN_MAX) return -1;
    wm_dyn_win_t* dw = &g_dyn_wins[slot];
    if (!dw->used || dw->owner_pid != owner_pid) return -1;
    if (!menu || menu->menu_count <= 0) {
        dw->has_menu = false;
        memset(&dw->menu_bar, 0, sizeof(dw->menu_bar));
    } else {
        dw->menu_bar = *menu;
        if (dw->menu_bar.menu_count > PXE_MAX_MENUS) {
            dw->menu_bar.menu_count = PXE_MAX_MENUS;
        }
        dw->has_menu = true;
    }
    g_pxe_layout_dirty = 1;
    g_pxe_present_dirty = 1;
    return 0;
}

int wm_pxe_win_get_surface(struct process* proc, int win_id, pxe_win_surface_t* out_surf) {
    int slot = win_id - WM_DYN_BASE;
    if (!proc || !out_surf || slot < 0 || slot >= WM_DYN_MAX) return -1;
    wm_dyn_win_t* dw = &g_dyn_wins[slot];
    if (!dw->used || dw->owner_pid != proc->pid) return -1;
    out_surf->pixels = (proc->exec_ring == 3) ? (void*)(uintptr_t)dw->canvas_uvirt : dw->canvas_kvirt;
    out_surf->width  = dw->client_w;
    out_surf->height = dw->client_h;
    out_surf->stride = dw->alloc_stride;
    out_surf->pitch  = dw->pitch;
    out_surf->format = dw->format;
    return 0;
}

int wm_pxe_win_present(uint32_t owner_pid, int win_id, int rx, int ry, int rw, int rh) {
    int slot = win_id - WM_DYN_BASE;
    if (slot < 0 || slot >= WM_DYN_MAX) return -1;
    uint64_t flags = wm_irq_save();
    wm_dyn_win_t* dw = &g_dyn_wins[slot];
    if (!dw->used || dw->owner_pid != owner_pid) {
        wm_irq_restore(flags);
        return -1;
    }

    int cw = dw->client_w;
    int ch = dw->client_h;
    if (cw > dw->alloc_stride) cw = dw->alloc_stride;
    if (ch > dw->alloc_h) ch = dw->alloc_h;

    if (rw <= 0 || rh <= 0) {
        rx = 0;
        ry = 0;
        rw = cw;
        rh = ch;
    } else {
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > cw) rw = cw - rx;
        if (ry + rh > ch) rh = ch - ry;
    }

    if (rw > 0 && rh > 0) {
        if (!dw->present_dirty || dw->dirty_rw <= 0 || dw->dirty_rh <= 0) {
            dw->dirty_rx = rx;
            dw->dirty_ry = ry;
            dw->dirty_rw = rw;
            dw->dirty_rh = rh;
        } else {
            int x0 = (dw->dirty_rx < rx) ? dw->dirty_rx : rx;
            int y0 = (dw->dirty_ry < ry) ? dw->dirty_ry : ry;
            int ex1 = dw->dirty_rx + dw->dirty_rw;
            int ex2 = rx + rw;
            int ey1 = dw->dirty_ry + dw->dirty_rh;
            int ey2 = ry + rh;
            int x1 = (ex1 > ex2) ? ex1 : ex2;
            int y1 = (ey1 > ey2) ? ey1 : ey2;
            dw->dirty_rx = x0;
            dw->dirty_ry = y0;
            dw->dirty_rw = x1 - x0;
            dw->dirty_rh = y1 - y0;
        }
        dw->present_dirty = true;
        g_pxe_present_dirty = 1;
    }
    wm_irq_restore(flags);
    return 0;
}

int wm_pxe_win_poll_event(uint32_t owner_pid, int win_id, pxe_event_t* out_ev) {
    int slot = win_id - WM_DYN_BASE;
    if (!out_ev || slot < 0 || slot >= WM_DYN_MAX) return -1;
    uint64_t flags = wm_irq_save();
    wm_dyn_win_t* dw = &g_dyn_wins[slot];
    if (!dw->used || dw->owner_pid != owner_pid) {
        wm_irq_restore(flags);
        return -1;
    }
    if (dw->ev_count <= 0) {
        wm_irq_restore(flags);
        return 0;
    }
    *out_ev = dw->ev_queue[dw->ev_head];
    dw->ev_head = (dw->ev_head + 1) % WM_EV_QUEUE_CAP;
    dw->ev_count--;
    wm_irq_restore(flags);
    return 1;
}

int wm_pxe_count_windows_for_pid(uint32_t pid) {
    int cnt = 0;
    for (int i = 0; i < WM_DYN_MAX; i++) {
        if (g_dyn_wins[i].used && g_dyn_wins[i].owner_pid == pid) cnt++;
    }
    return cnt;
}

int wm_pxe_focus_pid(uint32_t pid, const char* activate_arg) {
    int focused = 0;
    for (int i = 0; i < WM_DYN_MAX; i++) {
        wm_dyn_win_t* dw = &g_dyn_wins[i];
        if (dw->used && dw->owner_pid == pid) {
            dw->shaded = 0;
            uint64_t flags = wm_irq_save();
            bring_to_top(dw->win_id);
            wm_irq_restore(flags);

            pxe_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = PXE_EV_ACTIVATE;
            ev.win_id = dw->win_id;
            if (activate_arg && activate_arg[0]) {
                strncpy(ev.text, activate_arg, sizeof(ev.text) - 1);
            }
            wm_dyn_push_event(dw, &ev);
            focused = 1;
        }
    }
    if (focused) {
        g_pxe_layout_dirty = 1;
        g_pxe_present_dirty = 1;
    }
    return focused ? 0 : -1;
}

void wm_notify_banner(const char* msg) {
    if (!msg) return;
    uint64_t flags = wm_irq_save();
    int i = 0;
    while (msg[i] && i < (int)sizeof(opened_msg) - 1) {
        opened_msg[i] = msg[i];
        i++;
    }
    opened_msg[i] = '\0';
    opened_msg_timer = 120;
    g_pxe_layout_dirty = 1;
    g_pxe_present_dirty = 1;
    wm_irq_restore(flags);
}

void wm_notify_app_crash(uint32_t pid, const char* app_name, const char* exc_name, uint64_t rip, uint64_t cr2) {
    uint64_t flags = wm_irq_save();
    crash_pid = pid;
    memset(crash_app_name, 0, sizeof(crash_app_name));
    memset(crash_exc_name, 0, sizeof(crash_exc_name));
    if (app_name) strncpy(crash_app_name, app_name, sizeof(crash_app_name) - 1);
    if (exc_name) strncpy(crash_exc_name, exc_name, sizeof(crash_exc_name) - 1);
    crash_rip = rip;
    crash_cr2 = cr2;
    crash_ok_pressed = 0;
    crash_dialog_active = 1;
    g_pxe_layout_dirty = 1;
    g_pxe_present_dirty = 1;
    wm_irq_restore(flags);
    speaker_beep();
}

void wm_request_ring0_launch(const char* path, const char* arg1) {
    if (!path || !path[0]) return;
    uint64_t flags = wm_irq_save();
    memset(r0_confirm_path, 0, sizeof(r0_confirm_path));
    memset(r0_confirm_arg1, 0, sizeof(r0_confirm_arg1));
    strncpy(r0_confirm_path, path, sizeof(r0_confirm_path) - 1);
    if (arg1 && arg1[0]) {
        strncpy(r0_confirm_arg1, arg1, sizeof(r0_confirm_arg1) - 1);
        r0_confirm_has_arg1 = 1;
    } else {
        r0_confirm_has_arg1 = 0;
    }
    r0_cancel_pressed = 0;
    r0_run_pressed = 0;
    r0_confirm_active = 1;
    g_pxe_layout_dirty = 1;
    g_pxe_present_dirty = 1;
    wm_irq_restore(flags);
    speaker_beep();
}

void wm_request_desktop_redraw(void) {
    uint64_t flags = wm_irq_save();
    g_pxe_layout_dirty = 1;
    g_pxe_present_dirty = 1;
    redraw = 1;
    wm_irq_restore(flags);
}

void wm_notify_media_changed(void) {
    uint64_t flags = wm_irq_save();
    g_pxe_layout_dirty = 1;
    g_pxe_present_dirty = 1;
    redraw = 1;
    wm_irq_restore(flags);
    desktop_refresh();

    for (int s = 0; s < WM_DYN_MAX; s++) {
        if (g_dyn_wins[s].used) {
            pxe_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = PXE_EV_REDRAW;
            ev.win_id = g_dyn_wins[s].win_id;
            ev.w = g_dyn_wins[s].client_w;
            ev.h = g_dyn_wins[s].client_h;
            ev.arg0 = 1;
            wm_dyn_push_event(&g_dyn_wins[s], &ev);
        }
    }
}
