#ifndef _PXE_GFX_H
#define _PXE_GFX_H

#include "abi.h"

/* Classic Pixel Neko 256-color palette -> 0x00RRGGBB lookup */
uint32_t pxe_palette_to_rgb32(uint8_t color_idx);

/* Pixel & primitive drawing on pxe_win_surface_t (supports both PAL8 and RGB32) */
void pxe_put_pixel(const pxe_win_surface_t* surf, int x, int y, uint32_t color);
void pxe_put_rgb32(const pxe_win_surface_t* surf, int x, int y, uint32_t rgb32);
void pxe_fill_rect(const pxe_win_surface_t* surf, int x, int y, int w, int h, uint32_t color);
void pxe_draw_rect(const pxe_win_surface_t* surf, int x, int y, int w, int h, uint32_t color);
void pxe_draw_hline(const pxe_win_surface_t* surf, int x, int y, int w, uint32_t color);
void pxe_draw_vline(const pxe_win_surface_t* surf, int x, int y, int h, uint32_t color);

/* Classic 8x16 and 5x7 Pixel Neko bitmap font rendering */
void pxe_draw_char(const pxe_win_surface_t* surf, int x, int y, char c, uint32_t fg);
void pxe_draw_text(const pxe_win_surface_t* surf, int x, int y, const char* text, uint32_t fg);
void pxe_draw_text_small(const pxe_win_surface_t* surf, int x, int y, const char* text, uint32_t fg);

/* Classic Mac Platinum / Win9x 3D UI Widgets */
void pxe_draw_bevel_box(const pxe_win_surface_t* surf, int x, int y, int w, int h,
                        int sunken, uint32_t fill_col);
void pxe_draw_button(const pxe_win_surface_t* surf, int x, int y, int w, int h,
                     const char* label, int pressed, int enabled);
void pxe_draw_scrollbar(const pxe_win_surface_t* surf, int x, int y, int w, int h,
                        int offset, int visible_rows, int total_rows);

static inline int pxe_point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

#endif /* _PXE_GFX_H */

