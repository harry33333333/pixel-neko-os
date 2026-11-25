#include "vplayer_h264.h"
#include <stdlib.h>
#include <string.h>
#include "third_party/openh264/codec/api/wels/codec_api.h"

struct vp_h264_dec {
    ISVCDecoder* dec;
    int          num_threads;
};

static void silent_trace_cb(void* ctx, int level, const char* msg) {
    (void)ctx; (void)level; (void)msg;
}

static int init_openh264_instance(vp_h264_dec_t* h) {
    if (WelsCreateDecoder(&h->dec) != 0 || !h->dec) {
        h->dec = NULL;
        return -1;
    }

    int trace_level = WELS_LOG_QUIET;
    WelsTraceCallback cb = silent_trace_cb;
    h->dec->SetOption(DECODER_OPTION_TRACE_LEVEL, &trace_level);
    h->dec->SetOption(DECODER_OPTION_TRACE_CALLBACK, &cb);

    int threads = h->num_threads;
    if (threads < 1) threads = 1;
    if (threads > 16) threads = 16;
    h->dec->SetOption(DECODER_OPTION_NUM_OF_THREADS, &threads);

    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.uiTargetDqLayer = 255;
    param.eEcActiveIdc = ERROR_CON_SLICE_COPY;

    if (h->dec->Initialize(&param) != 0) {
        WelsDestroyDecoder(h->dec);
        h->dec = NULL;
        return -1;
    }
    return 0;
}

vp_h264_dec_t* vp_h264_create(int num_threads) {
    vp_h264_dec_t* h = (vp_h264_dec_t*)calloc(1, sizeof(vp_h264_dec_t));
    if (!h) return NULL;
    h->num_threads = num_threads;
    if (init_openh264_instance(h) != 0) {
        free(h);
        return NULL;
    }
    return h;
}

void vp_h264_destroy(vp_h264_dec_t* h) {
    if (!h) return;
    if (h->dec) {
        h->dec->Uninitialize();
        WelsDestroyDecoder(h->dec);
        h->dec = NULL;
    }
    free(h);
}

void vp_h264_reset(vp_h264_dec_t* h) {
    if (!h) return;
    if (h->dec) {
        h->dec->Uninitialize();
        WelsDestroyDecoder(h->dec);
        h->dec = NULL;
    }
    (void)init_openh264_instance(h);
}

int vp_h264_decode(vp_h264_dec_t* h,
                   const uint8_t* annexb, size_t len,
                   uint64_t pts_us,
                   vp_h264_frame_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!h || !h->dec || !annexb || len == 0) return -1;

    unsigned char* dst[3] = { NULL, NULL, NULL };
    SBufferInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.uiInBsTimeStamp = pts_us;

    DECODING_STATE st = h->dec->DecodeFrameNoDelay(annexb, (int)len, dst, &bi);
    if (st != dsErrorFree && bi.iBufferStatus != 1) {
        return -1;
    }
    if (bi.iBufferStatus == 1 && dst[0] && dst[1] && dst[2]) {
        out->got_frame = 1;
        out->width     = bi.UsrData.sSystemBuffer.iWidth;
        out->height    = bi.UsrData.sSystemBuffer.iHeight;
        out->y_stride  = bi.UsrData.sSystemBuffer.iStride[0];
        out->uv_stride = bi.UsrData.sSystemBuffer.iStride[1];
        out->y_plane   = dst[0];
        out->u_plane   = dst[1];
        out->v_plane   = dst[2];
        out->pts_us    = bi.uiOutYuvTimeStamp ? bi.uiOutYuvTimeStamp : pts_us;
    }
    return 0;
}

int vp_h264_flush(vp_h264_dec_t* h, vp_h264_frame_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!h || !h->dec) return -1;

    unsigned char* dst[3] = { NULL, NULL, NULL };
    SBufferInfo bi;
    memset(&bi, 0, sizeof(bi));

    h->dec->FlushFrame(dst, &bi);
    if (bi.iBufferStatus == 1 && dst[0] && dst[1] && dst[2]) {
        out->got_frame = 1;
        out->width     = bi.UsrData.sSystemBuffer.iWidth;
        out->height    = bi.UsrData.sSystemBuffer.iHeight;
        out->y_stride  = bi.UsrData.sSystemBuffer.iStride[0];
        out->uv_stride = bi.UsrData.sSystemBuffer.iStride[1];
        out->y_plane   = dst[0];
        out->u_plane   = dst[1];
        out->v_plane   = dst[2];
        out->pts_us    = bi.uiOutYuvTimeStamp;
        return 1;
    }
    return 0;
}

