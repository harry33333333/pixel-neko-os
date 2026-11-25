#ifndef _VPLAYER_H264_H
#define _VPLAYER_H264_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vp_h264_dec vp_h264_dec_t;

typedef struct {
    int            got_frame;
    int            width;
    int            height;
    int            y_stride;
    int            uv_stride;
    const uint8_t* y_plane;
    const uint8_t* u_plane;
    const uint8_t* v_plane;
    uint64_t       pts_us;
} vp_h264_frame_t;

vp_h264_dec_t* vp_h264_create(int num_threads);
void           vp_h264_destroy(vp_h264_dec_t* dec);
void           vp_h264_reset(vp_h264_dec_t* dec);

/*
 * Feed one Annex-B access unit (with 00 00 00 01 start codes).
 * Returns 0 on success (check out->got_frame), < 0 on decode error.
 */
int vp_h264_decode(vp_h264_dec_t* dec,
                   const uint8_t* annexb, size_t len,
                   uint64_t pts_us,
                   vp_h264_frame_t* out);

/*
 * Flush any remaining reordered frames at EOF.
 */
int vp_h264_flush(vp_h264_dec_t* dec, vp_h264_frame_t* out);

#ifdef __cplusplus
}
#endif

#endif /* _VPLAYER_H264_H */

