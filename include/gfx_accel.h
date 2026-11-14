#ifndef GFX_ACCEL_H
#define GFX_ACCEL_H

// ----------------------------------------------------------------------------
// Unified 2D graphics backend HAL. Drivers remain in ring0.
// ----------------------------------------------------------------------------

#define GFX_MIN_WIDTH           640
#define GFX_MIN_HEIGHT          480
#define GFX_FALLBACK_MAX_WIDTH  1024
#define GFX_FALLBACK_MAX_HEIGHT 768

#define GFX_BPP_MASK_8   (1U << 0)
#define GFX_BPP_MASK_16  (1U << 1)
#define GFX_BPP_MASK_24  (1U << 2)
#define GFX_BPP_MASK_32  (1U << 3)
#define GFX_BPP_MASK_ALL (GFX_BPP_MASK_8 | GFX_BPP_MASK_16 | GFX_BPP_MASK_24 | GFX_BPP_MASK_32)

void         gfx_accel_init(void);
int          gfx_accel_is_active(void);
int          gfx_accel_has_valid_driver(void);
const char*  gfx_accel_get_name(void);
const char*  gfx_accel_get_status(void);
const char*  gfx_accel_get_fallback_reason(void);
int          gfx_accel_is_hardware_2d_active(void);
int          gfx_accel_supports_1280x1024(void);
unsigned int gfx_accel_get_vram_size(void);
int          gfx_accel_get_max_width(void);
int          gfx_accel_get_max_height(void);
int          gfx_accel_get_supported_bpp_mask(void);
int          gfx_accel_get_mode_count(void);
int          gfx_accel_get_mode(int index, int* out_w, int* out_h,
                                unsigned char* out_bpp_mask,
                                unsigned char* out_preferred);
int          gfx_accel_can_change_mode(int w, int h, int bpp);
int          gfx_accel_supports_bpp(int w, int h, int bpp);
int          gfx_accel_try_set_mode(int w, int h, int bpp);
void         gfx_accel_set_mode(int w, int h, int bpp);
void         gfx_accel_fill_rect(int x, int y, int w, int h, unsigned int color);
void         gfx_accel_screen_blt(int sx, int sy, int dx, int dy, int w, int h);
void         gfx_accel_present_rect(int x, int y, int w, int h);
void         gfx_accel_sync(void);
int          gfx_accel_has_hardware_cursor(void);
int          gfx_accel_set_cursor(int x, int y, int visible);

#endif
