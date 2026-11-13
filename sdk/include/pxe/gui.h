#ifndef _PXE_GUI_H
#define _PXE_GUI_H

#include "abi.h"

int  pxe_win_create(const pxe_win_desc_t* desc, pxe_win_surface_t* out_surface);
int  pxe_win_destroy(int win_id);
int  pxe_win_set_title(int win_id, const char* title);
int  pxe_win_set_menu(int win_id, const pxe_menu_bar_t* menu_bar);
int  pxe_win_get_surface(int win_id, pxe_win_surface_t* out_surface);
int  pxe_win_present(int win_id);
int  pxe_win_present_rect(int win_id, int x, int y, int w, int h);

int  pxe_win_poll_event(int win_id, pxe_event_t* out_ev);
int  pxe_win_wait_event(int win_id, pxe_event_t* out_ev);

int  pxe_clipboard_get(char* buf, size_t max_len);
int  pxe_clipboard_set(const char* text);
void pxe_notify_banner(const char* message);
void pxe_beep(void);

#endif /* _PXE_GUI_H */

