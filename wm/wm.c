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

static const struct {
    const char* filename;
    const char* label;
    unsigned char fallback_bg;
    bool bypass_r0_prompt;
} g_desktop_apps[10] = {
    { "about.pxe",   "About",   1,  false },
    { "clock.pxe",   "Clock",   2,  false },
    { "calc.pxe",    "Calc",    4,  false },
    { "cpanel.pxe",  "CPanel",  6,  false },
    { "files.pxe",   "Files",   5,  false },
    { "notepad.pxe", "Notepad", 3,  false },
    { "sysinfo.pxe", "SysInfo", 7,  true  },
    { "puzzle.pxe",  "Puzzle",  8,  false },
    { "viewer.pxe",  "Viewer",  9,  false },
    { "player.pxe",  "Player",  10, false }
};

static uint8_t g_desk_icon_cache[10][16][16];
static uint8_t g_desk_icon_loaded[10] = {0};

static int desk_icon_x[10] = {0}, desk_icon_y[10] = {0};
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
    pxe_event_t     ev_queue[WM_EV_QUEUE_CAP];
    int             ev_head;
    int             ev_tail;
    int             ev_count;
} wm_dyn_win_t;

static wm_dyn_win_t g_dyn_wins[WM_DYN_MAX];
static volatile int g_pxe_present_dirty = 0;
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
static int uninit_dialog_active=0;
static int uninit_dialog_checked=0;
static int uninit_ignore_pressed=0, uninit_init_pressed=0;

static void icon(unsigned char* v, int x, int y, int app_idx, unsigned char bg, const char* lb, int sel);

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

static void icon(unsigned char* v, int x, int y, int app_idx, unsigned char bg, const char* lb, int sel)
{
    if (sel) {
        for(int i=y-4; i<y+54; i++) for(int j=x-4; j<x+36; j++) PUT_PIXEL(v, j, i, 11);
    }
    
    unsigned char ic[16][16];
    for(int i=0;i<16;i++)for(int j=0;j<16;j++) ic[i][j]=255;

    bool custom_ok = false;
    if (app_idx >= 0 && app_idx < 10) {
        if (g_desk_icon_loaded[app_idx] == 0) {
            if (pxe_load_builtin_icon16(g_desktop_apps[app_idx].filename,
                                        g_desk_icon_cache[app_idx])) {
                g_desk_icon_loaded[app_idx] = 1;
            } else {
                g_desk_icon_loaded[app_idx] = 2;
            }
        }
        if (g_desk_icon_loaded[app_idx] == 1) {
            for (int i = 0; i < 16; i++) {
                for (int j = 0; j < 16; j++) {
                    ic[i][j] = g_desk_icon_cache[app_idx][i][j];
                }
            }
            custom_ok = true;
        }
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
    
    if (lb) {
        int tw = 0; while(lb[tw]) tw++;
        int tx = x + 16 - tw*4; // center 32px
        if (sel) {
            for(int yy=y+36; yy<y+48; yy++) for(int xx=tx-2; xx<tx+tw*8+2; xx++) PUT_PIXEL(v, xx, yy, 1);
            font_draw_text(v, tx, y+38, lb, 15);
        } else {
            font_draw_text(v, tx, y+38, lb, 0);
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

static void blit_dyn_window(unsigned char* v, const wm_dyn_win_t* dw, int active) {
    int ww_cur = dw->client_w + 2;
    int wh_cur = dw->shaded ? 20 : (dw->client_h + 20);
    int is_rs = (!dw->shaded && (dw->flags & PXE_WIN_RESIZABLE)) ? 1 : 0;
    int no_cl = (dw->flags & PXE_WIN_NO_CLOSE) ? 1 : 0;

    window_ex(v, dw->x, dw->y, dw->title, ww_cur, wh_cur, 0, active, no_cl);
    if (dw->shaded || !dw->canvas_kvirt) return;

    int cx0 = dw->x + 1;
    int cy0 = dw->y + 19;
    int cw = dw->client_w;
    int ch = dw->client_h;
    if (cw > dw->alloc_stride) cw = dw->alloc_stride;
    if (ch > dw->alloc_h) ch = dw->alloc_h;

    if (dw->format == PXE_CANVAS_RGB32) {
        for (int ry = 0; ry < ch; ry++) {
            int sy = cy0 + ry;
            if (sy < 0 || sy >= screen_h) continue;
            const uint32_t* src = (const uint32_t*)((const uint8_t*)dw->canvas_kvirt +
                                                    (size_t)ry * (size_t)dw->pitch);
            for (int rx = 0; rx < cw; rx++) {
                int sx = cx0 + rx;
                if (sx >= 0 && sx < screen_w) {
                    put_rgb32_pixel(v, sx, sy, src[rx]);
                }
            }
        }
    } else {
        for (int ry = 0; ry < ch; ry++) {
            int sy = cy0 + ry;
            if (sy < 0 || sy >= screen_h) continue;
            const uint8_t* src = (const uint8_t*)dw->canvas_kvirt +
                                 (size_t)ry * (size_t)dw->pitch;
            for (int rx = 0; rx < cw; rx++) {
                uint8_t c = src[rx];
                if (c != 255) {
                    PUT_PIXEL(v, cx0 + rx, sy, c);
                }
            }
        }
    }

    if (is_rs) {
        int c = 10;
        for (int i = 0; i < c; i++) {
            for (int j = 0; j <= i; j++) {
                int px = dw->x + ww_cur - c + j;
                int py = dw->y + wh_cur - c + i;
                if (px >= 0 && px < screen_w && py >= 0 && py < screen_h) {
                    PUT_PIXEL(v, px, py, 8);
                }
            }
        }
    }
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
                draw_menu_popup(v, 4, 20, 172, 4*18+4);
                font_draw_text(v, 10, 24, "About Pixel Neko", 0);
                font_draw_text(v, 10, 24+18, "System Information", 0);
                font_draw_text(v, 10, 24+36, "Restart Computer", 0);
                font_draw_text(v, 10, 24+54, "Shut Down Computer", 0);
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
            int mw = (menu_index == 3) ? 64 : 40;
            for(int y=2; y<18; y++) for(int x=mx-4; x<mx+mw; x++) PUT_PIXEL(v, x, y, 1);
        }
        if (!install_mode) {
            font_draw_text(v, 30, 2, "File", menu_index==0?15:0);
            font_draw_text(v, 70, 2, "Edit", menu_index==1?15:0);
            font_draw_text(v, 110, 2, "View", menu_index==2?15:0);
            font_draw_text(v, 150, 2, "Special", menu_index==3?15:0);
        }
        
        if (menu_index == 4) { // Apple Logo
            if (install_mode) {
                draw_menu_popup(v, 4, 20, 172, 2*18+4);
                font_draw_text(v, 10, 24, "Restart Computer", 0);
                font_draw_text(v, 10, 24+18, "Shut Down Computer", 0);
            } else {
                draw_menu_popup(v, 4, 20, 172, 4*18+4);
                font_draw_text(v, 10, 24, "About Pixel Neko", 0);
                font_draw_text(v, 10, 24+18, "System Information", 0);
                font_draw_text(v, 10, 24+36, "Restart Computer", 0);
                font_draw_text(v, 10, 24+54, "Shut Down Computer", 0);
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
            font_draw_text(v, 114, 24, "Clean Up Desktop", 0);
        } else if (!install_mode && menu_index == 3) { // Special
            draw_menu_popup(v, 146, 20, 172, 6*18+4);
            font_draw_text(v, 154, 24, "About Pixel Neko", 0);
            font_draw_text(v, 154, 24+18, "System Information", 0);
            font_draw_text(v, 154, 24+36, "15-Puzzle", 0);
            font_draw_text(v, 154, 24+54, "Empty Trash", 0);
            font_draw_text(v, 154, 24+72, "Restart Computer", 0);
            font_draw_text(v, 154, 24+90, "Shut Down Computer", 0);
        }
    } else if (!install_mode) {
        font_draw_text(v, 30, 2, "File", 0);
        font_draw_text(v, 70, 2, "Edit", 0);
        font_draw_text(v, 110, 2, "View", 0);
        font_draw_text(v, 150, 2, "Special", 0);
    }
    
    int h,m,s; get_time(&h,&m,&s); char b[9];
    b[0]='0'+h/10;b[1]='0'+h%10;b[2]=':';b[3]='0'+m/10;b[4]='0'+m%10;b[5]=':';b[6]='0'+s/10;b[7]='0'+s%10;b[8]=0;
    font_draw_text(v, screen_w - 75, 2, b, 0);

    if (opened_msg[0]) {
        int ox = screen_w / 2 - 40;
        if (ox < 10) ox = 10;
        font_draw_text(v, ox, 22, opened_msg, 0);
    }
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

static void layout_desktop_icons(void) {
    int rows_per_col = (screen_h - 40) / 52;
    if (rows_per_col < 2) rows_per_col = 2;
    if (rows_per_col > 6) rows_per_col = 6;
    for (int i = 0; i < 10; i++) {
        int col = i / rows_per_col;
        int row = i % rows_per_col;
        desk_icon_x[i] = screen_w - 60 - col * 64;
        desk_icon_y[i] = 30 + row * 52;
        if (desk_icon_x[i] < 4) desk_icon_x[i] = 4;
        if (desk_icon_y[i] > screen_h - 48) desk_icon_y[i] = screen_h - 48;
    }
}

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
    if (ev->type == PXE_EV_MOUSE_MOVE && dw->ev_count > 0) {
        int last_idx = (dw->ev_tail - 1 + WM_EV_QUEUE_CAP) % WM_EV_QUEUE_CAP;
        if (dw->ev_queue[last_idx].type == PXE_EV_MOUSE_MOVE) {
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

void wm_on_resolution_change(int w, int h) {
    (void)w;
    (void)h;
    layout_desktop_icons();
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
    layout_desktop_icons();
    redraw = 1;
}

void wm_installer_activate(void) {
    menu_open = 0;
    menu_index = -1;
    pxe_launch_builtin_app("install.pxe", NULL, true);
    redraw = 1;
    need_mr = 1;
}

void wm_draw(unsigned char* v){
    if(redraw){
        extern void mouse_invalidate(void);
        mouse_invalidate();
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
            int num_icons = 10;
            for (int i = 0; i < num_icons; i++) {
                if (desk_icon_x[i] == 0 || desk_icon_x[i] > screen_w - 32) desk_icon_x[i] = screen_w - 60 - (i / 6) * 64;
                if (desk_icon_y[i] == 0 || desk_icon_y[i] > screen_h - 32) desk_icon_y[i] = 30 + (i % 6) * 52;
                icon(v, desk_icon_x[i], desk_icon_y[i], i,
                     g_desktop_apps[i].fallback_bg, g_desktop_apps[i].label,
                     last_click_icon == i);
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
    }
    redraw=0;
}


static int in_icon(int mx, int my, int i) {
    return (mx >= desk_icon_x[i] - 4 && mx < desk_icon_x[i] + 36 &&
            my >= desk_icon_y[i] - 4 && my < desk_icon_y[i] + 52);
}

static void open_icon_app(int clicked_icon) {
    if (ui_install_mode_requested() || clicked_icon < 0 || clicked_icon >= 10) return;
    pxe_launch_builtin_app(g_desktop_apps[clicked_icon].filename, NULL,
                           g_desktop_apps[clicked_icon].bypass_r0_prompt);
    last_click_icon = -1;
    last_click_icon_prev = -1;
    last_click_time_prev = 0;
    icon_pressed = -1;
    icon_dragging = -1;
    redraw = 1;
    need_mr = 1;
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
        if (h) *h = 48;
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

    if (g_pxe_present_dirty && !wm_is_dragging()) {
        g_pxe_present_dirty = 0;
        redraw = 1;
        need_mr = 1;
    }

    // Poll RTC once every 100 ticks (1 Hz / 1 second) to prevent ISA bus stalls
    static int last_rtc_tick = 0;
    if (global_tick - last_rtc_tick >= 100 || last_s == -1) {
        last_rtc_tick = global_tick;
        int h, m, s;
        get_time(&h, &m, &s);
        if (s != last_s) {
            last_s = s;
        
            if (!dragging) redraw = 1;
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
            int item_count = inst_mode ? 2 : 4;
            if (mx >= 4 && mx < 4 + 172 && my >= 20 && my < 20 + item_count*18 + 4) {
                int item = (my - 24) / 18;
                if (inst_mode) {
                    if (item == 0) { power_confirm = 1; }
                    if (item == 1) { power_confirm = 2; }
                } else if (item >= 0 && item < 4) {
                    if (item == 0) { pxe_launch_builtin_app("about.pxe", NULL, false); }
                    if (item == 1) { pxe_launch_builtin_app("sysinfo.pxe", NULL, true); }
                    if (item == 2) { power_confirm = 1; }
                    if (item == 3) { power_confirm = 2; }
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
                layout_desktop_icons();
                show_opened("Desktop Cleaned");
            }
        } else if (menu_index == 3 && !inst_mode) { // Special
            if (mx >= 146 && mx < 146 + 172 && my >= 20 && my < 20 + 6*18 + 4) {
                int item = (my - 24) / 18;
                if (item >= 0 && item < 6) {
                    if (item == 0 && !inst_mode) { pxe_launch_builtin_app("about.pxe", NULL, false); }
                    if (item == 1 && !inst_mode) { pxe_launch_builtin_app("sysinfo.pxe", NULL, true); }
                    if (item == 2 && !inst_mode) { pxe_launch_builtin_app("puzzle.pxe", NULL, false); }
                    if (item == 3 && !inst_mode) { speaker_beep(); show_opened("Trash Emptied"); }
                    if (item == 4) { power_confirm = 1; }
                    if (item == 5) { power_confirm = 2; }
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
        if (mx >= 4 && mx < 24) {
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
        else if (!inst_mode && mx >= 150 && mx < 206) { menu_open = (menu_open && menu_index == 3) ? 0 : 1; menu_index = 3; redraw = 1; need_mr = 1; }
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

    // Keyboard input to active window
    int key = keyboard_get_key();
    while (key != 0) {
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

    // Handle active desktop icon drag (fast outline box)
    if (icon_dragging != -1) {
        if (mb & 1) {
            int nix = mx - icon_drag_ox;
            int niy = my - icon_drag_oy;
            if (nix < 0) nix = 0;
            if (nix + 36 > screen_w) nix = screen_w - 36;
            if (niy < 20) niy = 20;
            if (niy + 48 > screen_h) niy = screen_h - 48;
            icon_drag_bx = nix;
            icon_drag_by = niy;
            icon_drag_active = 1;
        } else {
            if (icon_drag_active) {
                desk_icon_x[icon_dragging] = icon_drag_bx;
                desk_icon_y[icon_dragging] = icon_drag_by;
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
        if (mb & 1) {
            int dx = mx - (desk_icon_x[icon_pressed] + icon_press_ox);
            int dy = my - (desk_icon_y[icon_pressed] + icon_press_oy);
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > 6 || dy > 6) {
                icon_dragging = icon_pressed;
                icon_drag_ox = icon_press_ox;
                icon_drag_oy = icon_press_oy;
                icon_drag_bx = desk_icon_x[icon_pressed];
                icon_drag_by = desk_icon_y[icon_pressed];
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
            int total_icons = 10;
            int clicked_icon = -1;
            for (int i = total_icons - 1; i >= 0; i--) {
                if (in_icon(mx, my, i)) {
                    clicked_icon = i;
                    break;
                }
            }
            if (clicked_icon != -1) {
                if (last_click_icon_prev == clicked_icon &&
                    (wm_get_input_event_tick() - last_click_time_prev) <
                    WM_DOUBLE_CLICK_TICKS) {
                    open_icon_app(clicked_icon);
                    return;
                }
                icon_pressed = clicked_icon;
                icon_press_ox = mx - desk_icon_x[clicked_icon];
                icon_press_oy = my - desk_icon_y[clicked_icon];
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
    (void)rx; (void)ry; (void)rw; (void)rh;
    int slot = win_id - WM_DYN_BASE;
    if (slot < 0 || slot >= WM_DYN_MAX) return -1;
    wm_dyn_win_t* dw = &g_dyn_wins[slot];
    if (!dw->used || dw->owner_pid != owner_pid) return -1;
    g_pxe_present_dirty = 1;
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
    if (focused) g_pxe_present_dirty = 1;
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
    g_pxe_present_dirty = 1;
    wm_irq_restore(flags);
    speaker_beep();
}

void wm_request_desktop_redraw(void) {
    uint64_t flags = wm_irq_save();
    g_pxe_present_dirty = 1;
    redraw = 1;
    wm_irq_restore(flags);
}

void wm_notify_media_changed(void) {
    uint64_t flags = wm_irq_save();
    for (int i = 0; i < 10; i++) {
        g_desk_icon_loaded[i] = 0;
    }
    g_pxe_present_dirty = 1;
    redraw = 1;
    wm_irq_restore(flags);

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
