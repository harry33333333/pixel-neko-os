#ifndef WM_H
#define WM_H

#include <stdint.h>
#include <stdbool.h>
#include "../sdk/include/pxe/abi.h"

struct process;

#define WM_DOUBLE_CLICK_TICKS 80

void wm_init(void);
void wm_installer_activate(void);
void wm_on_resolution_change(int w, int h);
void wm_update(unsigned char* v, int mx, int my, int mb, int mb_press, int mb_release);
void wm_draw(unsigned char* v);
int wm_is_dragging(void);
int wm_get_drag_box(int* x, int* y, int* w, int* h);
int wm_need_mouse_reset(void);
void wm_set_input_event_tick(int tick);
int wm_get_input_event_tick(void);

/* PXE Dynamic Compositor & Modal Notifications */
int  wm_pxe_win_create(struct process* proc, const pxe_win_desc_t* desc, pxe_win_surface_t* out_surf);
int  wm_pxe_win_destroy(uint32_t owner_pid, int win_id);
void wm_pxe_destroy_all_for_pid(uint32_t pid);
int  wm_pxe_win_set_title(uint32_t owner_pid, int win_id, const char* title);
int  wm_pxe_win_set_menu(uint32_t owner_pid, int win_id, const pxe_menu_bar_t* menu);
int  wm_pxe_win_get_surface(struct process* proc, int win_id, pxe_win_surface_t* out_surf);
int  wm_pxe_win_present(uint32_t owner_pid, int win_id, int rx, int ry, int rw, int rh);
int  wm_pxe_win_poll_event(uint32_t owner_pid, int win_id, pxe_event_t* out_ev);
int  wm_pxe_count_windows_for_pid(uint32_t pid);
int  wm_pxe_focus_pid(uint32_t pid, const char* activate_arg);
void wm_notify_banner(const char* msg);
void wm_request_desktop_redraw(void);
void wm_notify_media_changed(void);
void wm_notify_app_crash(uint32_t pid, const char* app_name, const char* exc_name, uint64_t rip, uint64_t cr2);
void wm_request_ring0_launch(const char* path, const char* arg1);

void copy_fullscreen_to_lfb(const unsigned char* s_buf);
void copy_rect_to_lfb(const unsigned char* src, int rx, int ry, int rw, int rh);

extern int screen_w;
extern int screen_h;
extern int screen_bpp;
extern int screen_pitch;
extern unsigned char backbuffer[];

extern unsigned short palette16[256];
extern unsigned int palette32[256];

#define RGB16(r, g, b) (unsigned short)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))

#define PUT_PIXEL(v, px, py, c) do { \
    if ((px) >= 0 && (px) < screen_w && (py) >= 0 && (py) < screen_h) { \
        if (screen_bpp <= 8) (v)[(py)*screen_pitch + (px)] = (unsigned char)(c); \
        else if (screen_bpp <= 16) *(unsigned short*)&((v)[(py)*screen_pitch + (px)*2]) = palette16[(unsigned char)(c)]; \
        else if (screen_bpp == 24) { \
            unsigned int _c32 = palette32[(unsigned char)(c)]; \
            unsigned char* _p = &((v)[(py)*screen_pitch + (px)*3]); \
            _p[0] = (unsigned char)(_c32 & 0xFF); \
            _p[1] = (unsigned char)((_c32 >> 8) & 0xFF); \
            _p[2] = (unsigned char)((_c32 >> 16) & 0xFF); \
        } else { \
            *(unsigned int*)&((v)[(py)*screen_pitch + (px)*4]) = palette32[(unsigned char)(c)]; \
        } \
    } \
} while(0)

#endif
