#include "vplayer_mp4.h"
#include "vplayer_audio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pxe/fs.h>

#define MINIMP4_IMPLEMENTATION
#include "third_party/minimp4/minimp4.h"

/* =========================================================================
 * Lightweight H.264 SPS RBSP parser (for strict spec validation & BT.709)
 * ========================================================================= */

typedef struct {
    const uint8_t* data;
    size_t         size_bits;
    size_t         pos;
} bs_t;

static inline uint32_t bs_read_bit(bs_t* b) {
    if (b->pos >= b->size_bits) return 0;
    uint32_t v = (b->data[b->pos >> 3] >> (7u - (b->pos & 7u))) & 1u;
    b->pos++;
    return v;
}

static inline uint32_t bs_read_bits(bs_t* b, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | bs_read_bit(b);
    return v;
}

static uint32_t bs_read_ue(bs_t* b) {
    int zeros = 0;
    while (b->pos < b->size_bits && bs_read_bit(b) == 0 && zeros < 31) {
        zeros++;
    }
    if (zeros == 0) return 0;
    return ((1u << zeros) - 1u) + bs_read_bits(b, zeros);
}

static int32_t bs_read_se(bs_t* b) {
    uint32_t v = bs_read_ue(b);
    if (v & 1u) return (int32_t)((v + 1u) >> 1);
    return -(int32_t)(v >> 1);
}

static void skip_scaling_list(bs_t* b, int size) {
    int last_scale = 8, next_scale = 8;
    for (int j = 0; j < size; j++) {
        if (next_scale != 0) {
            int delta = bs_read_se(b);
            next_scale = (last_scale + delta + 256) & 255;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

typedef struct {
    int valid;
    int profile_idc;
    int chroma_format_idc;
    int bit_depth_luma;
    int bit_depth_chroma;
    int width;
    int height;
    int bt709;
    int fps_x100;
} sps_info_t;

static void parse_h264_sps(const uint8_t* nalu, size_t nalu_len, sps_info_t* out) {
    memset(out, 0, sizeof(*out));
    out->chroma_format_idc = 1;
    out->bit_depth_luma = 8;
    out->bit_depth_chroma = 8;
    if (!nalu || nalu_len < 4) return;

    /* Skip NAL header byte (0x67 / type 7) and strip emulation prevention 0x03 */
    uint8_t rbsp[512];
    size_t rlen = 0;
    size_t start = ((nalu[0] & 0x1Fu) == 7u) ? 1u : 0u;
    int zeros = 0;
    for (size_t i = start; i < nalu_len && rlen < sizeof(rbsp); i++) {
        uint8_t c = nalu[i];
        if (zeros == 2 && c == 0x03u) {
            zeros = 0;
            continue;
        }
        rbsp[rlen++] = c;
        if (c == 0) zeros++;
        else zeros = 0;
    }

    bs_t bs;
    bs.data = rbsp;
    bs.size_bits = rlen * 8u;
    bs.pos = 0;

    int profile_idc = (int)bs_read_bits(&bs, 8);
    (void)bs_read_bits(&bs, 8); /* constraint flags */
    (void)bs_read_bits(&bs, 8); /* level_idc */
    (void)bs_read_ue(&bs);      /* seq_parameter_set_id */

    out->profile_idc = profile_idc;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
        profile_idc == 86  || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {
        int chroma = (int)bs_read_ue(&bs);
        out->chroma_format_idc = chroma;
        if (chroma == 3) (void)bs_read_bit(&bs); /* separate_colour_plane_flag */
        out->bit_depth_luma = 8 + (int)bs_read_ue(&bs);
        out->bit_depth_chroma = 8 + (int)bs_read_ue(&bs);
        (void)bs_read_bit(&bs); /* qpprime_y_zero_transform_bypass_flag */
        if (bs_read_bit(&bs)) { /* seq_scaling_matrix_present_flag */
            int cnt = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < cnt; i++) {
                if (bs_read_bit(&bs)) {
                    skip_scaling_list(&bs, i < 6 ? 16 : 64);
                }
            }
        }
    }

    (void)bs_read_ue(&bs); /* log2_max_frame_num_minus4 */
    uint32_t poc_type = bs_read_ue(&bs);
    if (poc_type == 0) {
        (void)bs_read_ue(&bs);
    } else if (poc_type == 1) {
        (void)bs_read_bit(&bs);
        (void)bs_read_se(&bs);
        (void)bs_read_se(&bs);
        uint32_t n = bs_read_ue(&bs);
        for (uint32_t i = 0; i < n && i < 256; i++) (void)bs_read_se(&bs);
    }
    (void)bs_read_ue(&bs); /* max_num_ref_frames */
    (void)bs_read_bit(&bs); /* gaps_in_frame_num_value_allowed_flag */

    uint32_t w_mbs = bs_read_ue(&bs) + 1u;
    uint32_t h_map = bs_read_ue(&bs) + 1u;
    uint32_t frame_mbs_only = bs_read_bit(&bs);
    if (!frame_mbs_only) (void)bs_read_bit(&bs);
    (void)bs_read_bit(&bs); /* direct_8x8_inference_flag */

    uint32_t crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
    if (bs_read_bit(&bs)) {
        crop_l = bs_read_ue(&bs);
        crop_r = bs_read_ue(&bs);
        crop_t = bs_read_ue(&bs);
        crop_b = bs_read_ue(&bs);
    }

    uint32_t cw = w_mbs * 16u;
    uint32_t ch = (2u - frame_mbs_only) * h_map * 16u;
    uint32_t sub_x = (out->chroma_format_idc == 1 || out->chroma_format_idc == 2) ? 2u : 1u;
    uint32_t sub_y = (out->chroma_format_idc == 1) ? 2u : 1u;
    uint32_t crop_unit_x = sub_x;
    uint32_t crop_unit_y = sub_y * (2u - frame_mbs_only);
    if (cw > (crop_l + crop_r) * crop_unit_x) cw -= (crop_l + crop_r) * crop_unit_x;
    if (ch > (crop_t + crop_b) * crop_unit_y) ch -= (crop_t + crop_b) * crop_unit_y;
    out->width = (int)cw;
    out->height = (int)ch;
    out->bt709 = (out->height >= 720) ? 1 : 0;

    /* VUI parameters for matrix_coefficients & timing_info */
    if (bs_read_bit(&bs)) {
        if (bs_read_bit(&bs)) { /* aspect_ratio_info_present_flag */
            uint32_t ar = bs_read_bits(&bs, 8);
            if (ar == 255u) { (void)bs_read_bits(&bs, 32); }
        }
        if (bs_read_bit(&bs)) (void)bs_read_bit(&bs); /* overscan */
        if (bs_read_bit(&bs)) { /* video_signal_type_present_flag */
            (void)bs_read_bits(&bs, 4); /* video_format + full_range */
            if (bs_read_bit(&bs)) { /* colour_description_present_flag */
                (void)bs_read_bits(&bs, 8); /* primaries */
                (void)bs_read_bits(&bs, 8); /* transfer */
                uint32_t matrix = bs_read_bits(&bs, 8);
                if (matrix == 1u) out->bt709 = 1;
                else if (matrix == 5u || matrix == 6u) out->bt709 = 0;
            }
        }
        if (bs_read_bit(&bs)) { /* chroma_loc */
            (void)bs_read_ue(&bs);
            (void)bs_read_ue(&bs);
        }
        if (bs_read_bit(&bs)) { /* timing_info_present_flag */
            uint32_t num_units = bs_read_bits(&bs, 32);
            uint32_t time_scale = bs_read_bits(&bs, 32);
            if (num_units > 0 && time_scale > 0) {
                /* H.264 timing: time_scale / (2 * num_units) */
                uint64_t fps100 = ((uint64_t)time_scale * 100ULL) / (2ULL * (uint64_t)num_units);
                if (fps100 > 0 && fps100 < 100000ULL) {
                    out->fps_x100 = (int)fps100;
                }
            }
        }
    }
    out->valid = (out->width > 0 && out->height > 0);
}

/* =========================================================================
 * MP4 read callback for minimp4 (single-open moov parsing)
 * ========================================================================= */

typedef struct {
    int      fd;
    uint64_t file_size;
    uint64_t cur_pos;
} mp4_fd_ctx_t;

static int mp4_read_cb(int64_t offset, void* buffer, size_t size, void* token) {
    mp4_fd_ctx_t* ctx = (mp4_fd_ctx_t*)token;
    if (!ctx || offset < 0 || (uint64_t)offset > ctx->file_size) return 1;
    if ((uint64_t)offset != ctx->cur_pos) {
        if (pxe_seek(ctx->fd, (uint64_t)offset) < 0) return 1;
        ctx->cur_pos = (uint64_t)offset;
    }
    uint8_t* dst = (uint8_t*)buffer;
    size_t rem = size;
    while (rem > 0) {
        int r = pxe_read(ctx->fd, dst, rem);
        if (r <= 0) return 1;
        dst += r;
        rem -= (size_t)r;
        ctx->cur_pos += (uint64_t)r;
    }
    return 0;
}

static int cmp_sample_by_offset(const void* a, const void* b) {
    const vp_sample_entry_t* sa = (const vp_sample_entry_t*)a;
    const vp_sample_entry_t* sb = (const vp_sample_entry_t*)b;
    if (sa->file_offset < sb->file_offset) return -1;
    if (sa->file_offset > sb->file_offset) return 1;
    if (sa->dts_us < sb->dts_us) return -1;
    if (sa->dts_us > sb->dts_us) return 1;
    return 0;
}

static int has_raw_h264_ext(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".h264") == 0 || strcasecmp(dot, ".264") == 0;
}

static int validate_video_spec(vp_media_index_t* idx) {
    if (idx->width <= 0 || idx->height <= 0) {
        snprintf(idx->reject_reason, sizeof(idx->reject_reason), "Invalid resolution");
        return VP_OPEN_UNSUPPORTED_SPEC;
    }
    if (idx->width > 1920 || idx->height > 1080) {
        snprintf(idx->reject_reason, sizeof(idx->reject_reason),
                 "%dx%d > 1920x1080", idx->width, idx->height);
        return VP_OPEN_UNSUPPORTED_SPEC;
    }
    if (idx->fps_x100 > 6050) {
        snprintf(idx->reject_reason, sizeof(idx->reject_reason),
                 "%d.%02d fps > 60 fps", idx->fps_x100 / 100, idx->fps_x100 % 100);
        return VP_OPEN_UNSUPPORTED_SPEC;
    }
    if (idx->bit_depth > 8 || idx->profile_idc == 110 ||
        idx->profile_idc == 122 || idx->profile_idc == 244) {
        snprintf(idx->reject_reason, sizeof(idx->reject_reason),
                 "%d-bit / profile %d (8-bit 4:2:0 only)", idx->bit_depth, idx->profile_idc);
        return VP_OPEN_UNSUPPORTED_SPEC;
    }
    return VP_OPEN_OK;
}

static int open_raw_h264(const char* path, uint64_t file_size, vp_media_index_t* idx) {
    int fd = pxe_open(path);
    if (fd < 0) return VP_OPEN_ERR_IO;

    /* Scan Annex-B start codes in 256 KiB chunks to build frame/AU index & parse SPS */
    const size_t chunk_sz = 256u * 1024u;
    uint8_t* buf = (uint8_t*)malloc(chunk_sz + 8u);
    if (!buf) { pxe_close(fd); return VP_OPEN_ERR_NOMEM; }

    uint32_t cap = 1024u;
    vp_sample_entry_t* arr = (vp_sample_entry_t*)malloc(sizeof(vp_sample_entry_t) * cap);
    if (!arr) { free(buf); pxe_close(fd); return VP_OPEN_ERR_NOMEM; }

    uint32_t count = 0;
    sps_info_t sps;
    memset(&sps, 0, sizeof(sps));

    uint64_t file_off = 0;
    int64_t cur_au_start = -1;
    int cur_au_idr = 0;
    int have_vcl = 0;

    while (file_off < file_size) {
        size_t to_read = chunk_sz;
        if (file_off + to_read > file_size) to_read = (size_t)(file_size - file_off);
        int r = pxe_read(fd, buf, to_read);
        if (r <= 0) break;

        for (int i = 0; i + 4 < r; i++) {
            int sc_len = 0;
            if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) sc_len = 3;
            else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) sc_len = 4;
            if (!sc_len) continue;

            uint64_t nal_pos = file_off + (uint64_t)i;
            uint8_t nal_hdr = buf[i + sc_len];
            uint8_t nal_type = nal_hdr & 0x1Fu;

            if (nal_type == 7 && !sps.valid) {
                size_t avail = (size_t)(r - (i + sc_len));
                if (avail > 256u) avail = 256u;
                parse_h264_sps(buf + i + sc_len, avail, &sps);
            }

            if (nal_type == 1 || nal_type == 5 || nal_type == 7 || nal_type == 9) {
                if (cur_au_start >= 0 && have_vcl && (nal_type == 7 || nal_type == 9 || nal_type == 5 || nal_type == 1)) {
                    if (count >= cap) {
                        cap *= 2u;
                        vp_sample_entry_t* n = (vp_sample_entry_t*)realloc(arr, sizeof(vp_sample_entry_t) * cap);
                        if (!n) break;
                        arr = n;
                    }
                    arr[count].file_offset = (uint64_t)cur_au_start;
                    arr[count].size = (uint32_t)(nal_pos - (uint64_t)cur_au_start);
                    arr[count].is_audio = 0;
                    arr[count].is_sync = (uint8_t)cur_au_idr;
                    arr[count].track_idx = count;
                    count++;
                    cur_au_start = (int64_t)nal_pos;
                    cur_au_idr = (nal_type == 5 || nal_type == 7);
                    have_vcl = (nal_type == 1 || nal_type == 5);
                } else {
                    if (cur_au_start < 0) cur_au_start = (int64_t)nal_pos;
                    if (nal_type == 5 || nal_type == 7) cur_au_idr = 1;
                    if (nal_type == 1 || nal_type == 5) have_vcl = 1;
                }
            }
            i += sc_len;
        }
        file_off += (uint64_t)r;
    }

    if (cur_au_start >= 0 && have_vcl && (uint64_t)cur_au_start < file_size) {
        if (count >= cap) {
            cap += 16u;
            vp_sample_entry_t* n = (vp_sample_entry_t*)realloc(arr, sizeof(vp_sample_entry_t) * cap);
            if (n) arr = n;
        }
        if (count < cap) {
            arr[count].file_offset = (uint64_t)cur_au_start;
            arr[count].size = (uint32_t)(file_size - (uint64_t)cur_au_start);
            arr[count].is_audio = 0;
            arr[count].is_sync = (uint8_t)cur_au_idr;
            arr[count].track_idx = count;
            count++;
        }
    }

    free(buf);
    pxe_close(fd);

    if (!sps.valid || count == 0) {
        free(arr);
        return VP_OPEN_ERR_FORMAT;
    }

    idx->is_raw_h264 = 1;
    idx->file_size = file_size;
    idx->has_video = 1;
    idx->width = sps.width;
    idx->height = sps.height;
    idx->fps_x100 = sps.fps_x100 > 0 ? sps.fps_x100 : 3000;
    idx->bit_depth = sps.bit_depth_luma;
    idx->profile_idc = sps.profile_idc;
    idx->bt709 = sps.bt709;

    int vrc = validate_video_spec(idx);
    if (vrc != VP_OPEN_OK) {
        free(arr);
        return vrc;
    }

    uint32_t frame_us = (uint32_t)(100000000ULL / (uint64_t)(idx->fps_x100 > 0 ? idx->fps_x100 : 3000));
    for (uint32_t i = 0; i < count; i++) {
        arr[i].pts_us = (uint64_t)i * (uint64_t)frame_us;
        arr[i].dts_us = arr[i].pts_us;
        arr[i].duration_us = frame_us;
    }
    idx->duration_us = (uint64_t)count * (uint64_t)frame_us;
    idx->samples = arr;
    idx->sample_count = count;
    idx->video_sample_count = count;
    return VP_OPEN_OK;
}

int vp_media_index_open(const char* path, vp_media_index_t* out_idx) {
    if (!path || !out_idx) return VP_OPEN_ERR_IO;
    memset(out_idx, 0, sizeof(*out_idx));

    pxe_dirent_t st;
    memset(&st, 0, sizeof(st));
    if (pxe_stat(path, &st) < 0 || st.is_dir || st.size == 0) {
        return VP_OPEN_ERR_IO;
    }

    if (has_raw_h264_ext(path)) {
        return open_raw_h264(path, st.size, out_idx);
    }

    int fd = pxe_open(path);
    if (fd < 0) return VP_OPEN_ERR_IO;

    mp4_fd_ctx_t ctx;
    ctx.fd = fd;
    ctx.file_size = st.size;
    ctx.cur_pos = 0;

    MP4D_demux_t mp4;
    memset(&mp4, 0, sizeof(mp4));
    if (!MP4D_open(&mp4, mp4_read_cb, &ctx, (int64_t)st.size)) {
        pxe_close(fd);
        return VP_OPEN_ERR_FORMAT;
    }
    pxe_close(fd);

    int video_tr = -1;
    int audio_tr = -1;
    for (unsigned t = 0; t < mp4.track_count; t++) {
        MP4D_track_t* tr = &mp4.track[t];
        if (tr->handler_type == MP4D_HANDLER_TYPE_VIDE && video_tr < 0) {
            video_tr = (int)t;
        } else if (tr->handler_type == MP4D_HANDLER_TYPE_SOUN && audio_tr < 0) {
            audio_tr = (int)t;
        }
    }

    if (video_tr < 0) {
        MP4D_close(&mp4);
        return VP_OPEN_ERR_FORMAT;
    }

    MP4D_track_t* vtr = &mp4.track[video_tr];
    /* Strict codec check: only H.264 / AVC ('avc1' = 0x61766331, 'avc3' = 0x61766333) */
    uint32_t se = vtr->sample_entry;
    int is_avc = (se == 0x61766331u || se == 0x61766333u ||
                  vtr->object_type_indication == MP4_OBJECT_TYPE_AVC);
    if (!is_avc) {
        char fourcc[5] = {
            (char)((se >> 24) & 0xFF), (char)((se >> 16) & 0xFF),
            (char)((se >> 8) & 0xFF),  (char)(se & 0xFF), '\0'
        };
        snprintf(out_idx->reject_reason, sizeof(out_idx->reject_reason),
                 "Codec '%s' is not H.264", fourcc[0] ? fourcc : "unknown");
        MP4D_close(&mp4);
        return VP_OPEN_UNSUPPORTED_SPEC;
    }

    out_idx->file_size = st.size;
    out_idx->has_video = 1;
    out_idx->avc_nal_len_size = (int)(vtr->avc1.lengthSizeMinusOne & 3u) + 1;
    if (out_idx->avc_nal_len_size < 1 || out_idx->avc_nal_len_size > 4) {
        out_idx->avc_nal_len_size = 4;
    }

    /* Extract SPS & PPS from avcC and build Annex-B extradata */
    sps_info_t sps;
    memset(&sps, 0, sizeof(sps));
    size_t extra_cap = 2048u;
    uint8_t* extra = (uint8_t*)malloc(extra_cap);
    size_t extra_len = 0;

    int sps_bytes = 0;
    for (int i = 0; ; i++) {
        const void* sps_ptr = MP4D_read_sps(&mp4, (unsigned)video_tr, i, &sps_bytes);
        if (!sps_ptr || sps_bytes <= 0) break;
        if (!sps.valid) {
            parse_h264_sps((const uint8_t*)sps_ptr, (size_t)sps_bytes, &sps);
        }
        if (extra && extra_len + 4u + (size_t)sps_bytes <= extra_cap) {
            extra[extra_len++] = 0; extra[extra_len++] = 0;
            extra[extra_len++] = 0; extra[extra_len++] = 1;
            memcpy(extra + extra_len, sps_ptr, (size_t)sps_bytes);
            extra_len += (size_t)sps_bytes;
        }
    }
    int pps_bytes = 0;
    for (int i = 0; ; i++) {
        const void* pps_ptr = MP4D_read_pps(&mp4, (unsigned)video_tr, i, &pps_bytes);
        if (!pps_ptr || pps_bytes <= 0) break;
        if (extra && extra_len + 4u + (size_t)pps_bytes <= extra_cap) {
            extra[extra_len++] = 0; extra[extra_len++] = 0;
            extra[extra_len++] = 0; extra[extra_len++] = 1;
            memcpy(extra + extra_len, pps_ptr, (size_t)pps_bytes);
            extra_len += (size_t)pps_bytes;
        }
    }
    out_idx->extradata_annexb = extra;
    out_idx->extradata_len = extra_len;

    out_idx->width = sps.valid ? sps.width : (int)vtr->SampleDescription.video.width;
    out_idx->height = sps.valid ? sps.height : (int)vtr->SampleDescription.video.height;
    out_idx->bit_depth = sps.valid ? sps.bit_depth_luma : 8;
    out_idx->profile_idc = sps.valid ? sps.profile_idc : (int)vtr->avc1.AVCProfileIndication;
    out_idx->bt709 = sps.valid ? sps.bt709 : (out_idx->height >= 720 ? 1 : 0);

    /* Compute exact duration and FPS from track timescale & sample_count */
    uint64_t v_dur_ticks = ((uint64_t)vtr->duration_hi << 32) | (uint64_t)vtr->duration_lo;
    uint32_t v_ts = vtr->timescale ? vtr->timescale : 90000u;
    if (v_dur_ticks > 0) {
        out_idx->duration_us = (v_dur_ticks * 1000000ULL) / (uint64_t)v_ts;
    }
    if (vtr->sample_count > 0 && out_idx->duration_us > 0) {
        out_idx->fps_x100 = (int)(((uint64_t)vtr->sample_count * 100000000ULL) / out_idx->duration_us);
    } else if (sps.fps_x100 > 0) {
        out_idx->fps_x100 = sps.fps_x100;
    } else {
        out_idx->fps_x100 = 3000;
    }

    if (sps.valid && sps.chroma_format_idc != 1) {
        snprintf(out_idx->reject_reason, sizeof(out_idx->reject_reason),
                 "Chroma format != 4:2:0");
        vp_media_index_free(out_idx);
        MP4D_close(&mp4);
        return VP_OPEN_UNSUPPORTED_SPEC;
    }

    int vrc = validate_video_spec(out_idx);
    if (vrc != VP_OPEN_OK) {
        char saved_reason[128];
        strncpy(saved_reason, out_idx->reject_reason, sizeof(saved_reason) - 1);
        saved_reason[sizeof(saved_reason) - 1] = '\0';
        vp_media_index_free(out_idx);
        strncpy(out_idx->reject_reason, saved_reason, sizeof(out_idx->reject_reason) - 1);
        MP4D_close(&mp4);
        return vrc;
    }

    /* Inspect audio track if present */
    if (audio_tr >= 0) {
        MP4D_track_t* atr = &mp4.track[audio_tr];
        if (atr->object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3 ||
            atr->object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_LC_PROFILE ||
            atr->object_type_indication == MP4_OBJECT_TYPE_AUDIO_ISO_IEC_13818_7_MAIN_PROFILE ||
            atr->sample_entry == 0x6D703461u) { /* 'mp4a' */
            out_idx->audio_codec = VP_ACODEC_AAC;
            if (atr->object_type_indication == 0x69u || atr->object_type_indication == 0x6Bu) {
                out_idx->audio_codec = VP_ACODEC_MP3;
            }
        } else if (atr->sample_entry == 0x736F7774u) { /* 'sowt' PCM */
            out_idx->audio_codec = VP_ACODEC_PCM;
        }
        if (out_idx->audio_codec != VP_ACODEC_NONE) {
            out_idx->has_audio = 1;
            out_idx->audio_rate = atr->SampleDescription.audio.samplerate_hz ? (int)atr->SampleDescription.audio.samplerate_hz : 44100;
            out_idx->audio_channels = atr->SampleDescription.audio.channelcount ? (int)atr->SampleDescription.audio.channelcount : 2;
            out_idx->audio_bits = 16;
            if (atr->dsi && atr->dsi_bytes > 0) {
                out_idx->audio_dsi = (uint8_t*)malloc(atr->dsi_bytes);
                if (out_idx->audio_dsi) {
                    memcpy(out_idx->audio_dsi, atr->dsi, atr->dsi_bytes);
                    out_idx->audio_dsi_len = atr->dsi_bytes;
                }
            }
        }
    }

    /* Build unified offset-sorted sample table (eliminates random seeks on CD/HDD!) */
    uint32_t v_cnt = vtr->sample_count;
    uint32_t a_cnt = (out_idx->has_audio && audio_tr >= 0) ? mp4.track[audio_tr].sample_count : 0u;
    uint32_t total = v_cnt + a_cnt;
    if (total == 0) {
        vp_media_index_free(out_idx);
        MP4D_close(&mp4);
        return VP_OPEN_ERR_FORMAT;
    }

    vp_sample_entry_t* entries = (vp_sample_entry_t*)malloc(sizeof(vp_sample_entry_t) * total);
    if (!entries) {
        vp_media_index_free(out_idx);
        MP4D_close(&mp4);
        return VP_OPEN_ERR_NOMEM;
    }

    uint32_t idx_pos = 0;
    uint64_t max_end_us = out_idx->duration_us;

    for (uint32_t i = 0; i < v_cnt; i++) {
        unsigned fbytes = 0, dts = 0, pts = 0, dur = 0;
        int is_sync = 0;
        MP4D_file_offset_t off = MP4D_frame_offset(&mp4, (unsigned)video_tr, i,
                                                   &fbytes, &dts, &pts, &dur, &is_sync);
        vp_sample_entry_t* e = &entries[idx_pos++];
        e->file_offset = (uint64_t)off;
        e->size        = (uint32_t)fbytes;
        e->dts_us      = ((uint64_t)dts * 1000000ULL) / (uint64_t)v_ts;
        e->pts_us      = ((uint64_t)pts * 1000000ULL) / (uint64_t)v_ts;
        e->duration_us = (uint32_t)(((uint64_t)dur * 1000000ULL) / (uint64_t)v_ts);
        e->is_audio    = 0;
        e->is_sync     = (uint8_t)(is_sync ? 1 : 0);
        e->track_idx   = i;
        if (e->pts_us + e->duration_us > max_end_us) max_end_us = e->pts_us + e->duration_us;
    }

    if (a_cnt > 0 && audio_tr >= 0) {
        uint32_t a_ts = mp4.track[audio_tr].timescale ? mp4.track[audio_tr].timescale : (uint32_t)out_idx->audio_rate;
        for (uint32_t i = 0; i < a_cnt; i++) {
            unsigned fbytes = 0, dts = 0, pts = 0, dur = 0;
            int is_sync = 0;
            MP4D_file_offset_t off = MP4D_frame_offset(&mp4, (unsigned)audio_tr, i,
                                                       &fbytes, &dts, &pts, &dur, &is_sync);
            vp_sample_entry_t* e = &entries[idx_pos++];
            e->file_offset = (uint64_t)off;
            e->size        = (uint32_t)fbytes;
            e->dts_us      = ((uint64_t)dts * 1000000ULL) / (uint64_t)a_ts;
            e->pts_us      = ((uint64_t)pts * 1000000ULL) / (uint64_t)a_ts;
            e->duration_us = (uint32_t)(((uint64_t)dur * 1000000ULL) / (uint64_t)a_ts);
            e->is_audio    = 1;
            e->is_sync     = 1;
            e->track_idx   = i;
        }
    }

    /* Sort by file_offset so Thread 1 reads strictly forward through mdat! */
    qsort(entries, total, sizeof(vp_sample_entry_t), cmp_sample_by_offset);

    out_idx->samples = entries;
    out_idx->sample_count = total;
    out_idx->video_sample_count = v_cnt;
    out_idx->audio_sample_count = a_cnt;
    out_idx->duration_us = max_end_us;

    MP4D_close(&mp4);
    return VP_OPEN_OK;
}

void vp_media_index_free(vp_media_index_t* idx) {
    if (!idx) return;
    if (idx->extradata_annexb) free(idx->extradata_annexb);
    if (idx->audio_dsi) free(idx->audio_dsi);
    if (idx->samples) free(idx->samples);
    memset(idx, 0, sizeof(*idx));
}

uint32_t vp_media_find_seek_sample(const vp_media_index_t* idx, uint64_t target_us) {
    if (!idx || !idx->samples || idx->sample_count == 0) return 0;
    uint32_t best_sync = 0;
    for (uint32_t i = 0; i < idx->sample_count; i++) {
        const vp_sample_entry_t* e = &idx->samples[i];
        if (!e->is_audio && e->is_sync) {
            if (e->pts_us <= target_us) {
                best_sync = i;
            } else {
                break;
            }
        }
    }
    return best_sync;
}

size_t vp_avcc_to_annexb(const vp_media_index_t* idx,
                         const uint8_t* src, size_t src_len,
                         uint8_t* dst, size_t dst_cap,
                         int prepend_sps_pps) {
    if (!idx || !src || !dst || dst_cap < 8u) return 0;
    size_t out_pos = 0;

    if (idx->is_raw_h264) {
        if (src_len > dst_cap) return 0;
        memcpy(dst, src, src_len);
        return src_len;
    }

    if (prepend_sps_pps && idx->extradata_annexb && idx->extradata_len > 0) {
        if (idx->extradata_len > dst_cap) return 0;
        memcpy(dst, idx->extradata_annexb, idx->extradata_len);
        out_pos += idx->extradata_len;
    }

    int nl = idx->avc_nal_len_size;
    if (nl < 1 || nl > 4) nl = 4;
    size_t pos = 0;
    while (pos + (size_t)nl <= src_len) {
        uint32_t nalu_len = 0;
        for (int k = 0; k < nl; k++) {
            nalu_len = (nalu_len << 8) | src[pos + (size_t)k];
        }
        pos += (size_t)nl;
        if (nalu_len == 0 || pos + nalu_len > src_len) break;
        if (out_pos + 4u + nalu_len > dst_cap) break;

        dst[out_pos++] = 0x00;
        dst[out_pos++] = 0x00;
        dst[out_pos++] = 0x00;
        dst[out_pos++] = 0x01;
        memcpy(dst + out_pos, src + pos, nalu_len);
        out_pos += nalu_len;
        pos += nalu_len;
    }
    return out_pos;
}

