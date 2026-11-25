#ifndef _VPLAYER_MP4_H
#define _VPLAYER_MP4_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VP_OPEN_OK               0
#define VP_OPEN_ERR_IO          -1
#define VP_OPEN_ERR_FORMAT      -2
#define VP_OPEN_UNSUPPORTED_SPEC -3
#define VP_OPEN_ERR_NOMEM       -4

typedef struct {
    uint64_t file_offset;
    uint32_t size;
    uint64_t pts_us;
    uint64_t dts_us;
    uint32_t duration_us;
    uint8_t  is_audio;  /* 0 = video, 1 = audio */
    uint8_t  is_sync;   /* 1 = keyframe / IDR */
    uint32_t track_idx; /* sample index within its track */
} vp_sample_entry_t;

typedef struct {
    int      is_raw_h264;
    uint64_t file_size;
    int      width;
    int      height;
    int      fps_x100;       /* e.g. 6000 for 60.00 fps */
    int      bit_depth;      /* 8 */
    int      profile_idc;    /* 66=Baseline, 77=Main, 100=High */
    int      bt709;          /* 1 = BT.709, 0 = BT.601 */
    uint64_t duration_us;

    /* Video metadata */
    int      has_video;
    int      avc_nal_len_size; /* usually 4 */
    uint8_t* extradata_annexb; /* SPS + PPS with 00 00 00 01 start codes */
    size_t   extradata_len;

    /* Audio metadata */
    int      has_audio;
    int      audio_codec;    /* VP_ACODEC_* */
    int      audio_rate;
    int      audio_channels;
    int      audio_bits;
    uint8_t* audio_dsi;
    size_t   audio_dsi_len;

    /* Unified chronologically/offset-sorted sample table for sequential I/O */
    vp_sample_entry_t* samples;
    uint32_t           sample_count;
    uint32_t           video_sample_count;
    uint32_t           audio_sample_count;

    /* Rejection detail if VP_OPEN_UNSUPPORTED_SPEC */
    char     reject_reason[128];
} vp_media_index_t;

/*
 * Open MP4/MOV or raw .h264/.264 file, parse moov/SPS once into memory,
 * and enforce <=1080p @ 60fps 8-bit 4:2:0 H.264 spec limits.
 * On success returns VP_OPEN_OK and populates *out_idx.
 * Caller owns out_idx and must call vp_media_index_free().
 */
int  vp_media_index_open(const char* path, vp_media_index_t* out_idx);
void vp_media_index_free(vp_media_index_t* idx);

/*
 * Find the nearest video keyframe sample index <= target_us in idx->samples.
 */
uint32_t vp_media_find_seek_sample(const vp_media_index_t* idx, uint64_t target_us);

/*
 * Convert an MP4 AVCC length-prefixed video sample into Annex-B (00 00 00 01)
 * in dst_buf (returns bytes written, or 0 on overflow).
 * If prepend_sps_pps != 0, prepends idx->extradata_annexb first.
 */
size_t vp_avcc_to_annexb(const vp_media_index_t* idx,
                         const uint8_t* src, size_t src_len,
                         uint8_t* dst, size_t dst_cap,
                         int prepend_sps_pps);

#ifdef __cplusplus
}
#endif

#endif /* _VPLAYER_MP4_H */

