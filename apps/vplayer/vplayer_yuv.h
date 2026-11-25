#ifndef _VPLAYER_YUV_H
#define _VPLAYER_YUV_H

#include <stdint.h>
#include <pxe/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert YUV420P (8-bit, limited TV range) to BGRA32 (0x00RRGGBB) or RGB565,
 * with aspect-preserving scaling into (dst_x, dst_y, dst_w, dst_h).
 * Uses SSE2 vectorized fast paths for 1:1 and scaled rows.
 * bt709: 1 for BT.709 (HD >= 720p), 0 for BT.601 (SD).
 */
void vp_yuv420_to_surface(const pxe_win_surface_t* surf,
                          int dst_x, int dst_y, int dst_w, int dst_h,
                          const uint8_t* y_plane, int y_stride,
                          const uint8_t* u_plane, const uint8_t* v_plane, int uv_stride,
                          int src_w, int src_h,
                          int bt709);

#ifdef __cplusplus
}
#endif

#endif /* _VPLAYER_YUV_H */

