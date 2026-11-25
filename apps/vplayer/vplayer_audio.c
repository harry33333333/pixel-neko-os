#include "vplayer_audio.h"
#include <stdlib.h>
#include <string.h>
#include "third_party/faad2/include/neaacdec.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "../../drivers/audio/minimp3.h"

struct vp_audio_dec {
    int            codec;
    int            sample_rate;
    int            channels;
    int            bits;
    NeAACDecHandle aac;
    mp3dec_t       mp3;
    int16_t        mp3_scratch[MINIMP3_MAX_SAMPLES_PER_FRAME];
};

vp_audio_dec_t* vp_audio_dec_create(int codec,
                                    const uint8_t* dsi, size_t dsi_len,
                                    int sample_rate, int channels, int bits) {
    vp_audio_dec_t* dec = (vp_audio_dec_t*)calloc(1, sizeof(vp_audio_dec_t));
    if (!dec) return NULL;
    dec->codec = codec;
    dec->sample_rate = sample_rate > 0 ? sample_rate : 44100;
    dec->channels = channels > 0 ? channels : 2;
    dec->bits = bits > 0 ? bits : 16;

    if (codec == VP_ACODEC_AAC) {
        dec->aac = NeAACDecOpen();
        if (!dec->aac) {
            free(dec);
            return NULL;
        }
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(dec->aac);
        if (cfg) {
            cfg->outputFormat = FAAD_FMT_16BIT;
            cfg->downMatrix = 1;
            cfg->defSampleRate = (unsigned long)dec->sample_rate;
            cfg->defObjectType = LC;
            NeAACDecSetConfiguration(dec->aac, cfg);
        }
        unsigned long sr = (unsigned long)dec->sample_rate;
        unsigned char ch = (unsigned char)dec->channels;
        if (dsi && dsi_len > 0) {
            if (NeAACDecInit2(dec->aac, (unsigned char*)dsi, (unsigned long)dsi_len, &sr, &ch) < 0) {
                NeAACDecClose(dec->aac);
                free(dec);
                return NULL;
            }
            if (sr > 0) dec->sample_rate = (int)sr;
            if (ch > 0) dec->channels = (int)ch;
        }
    } else if (codec == VP_ACODEC_MP3) {
        mp3dec_init(&dec->mp3);
    }
    return dec;
}

void vp_audio_dec_destroy(vp_audio_dec_t* dec) {
    if (!dec) return;
    if (dec->aac) {
        NeAACDecClose(dec->aac);
        dec->aac = NULL;
    }
    free(dec);
}

void vp_audio_dec_reset(vp_audio_dec_t* dec) {
    if (!dec) return;
    if (dec->codec == VP_ACODEC_AAC && dec->aac) {
        NeAACDecPostSeekReset(dec->aac, 0);
    } else if (dec->codec == VP_ACODEC_MP3) {
        mp3dec_init(&dec->mp3);
    }
}

int vp_audio_dec_decode(vp_audio_dec_t* dec,
                        const uint8_t* pkt, size_t pkt_len,
                        int16_t* out_pcm, int max_frames,
                        int* out_sample_rate) {
    if (!dec || !pkt || pkt_len == 0 || !out_pcm || max_frames <= 0) return 0;

    if (dec->codec == VP_ACODEC_AAC && dec->aac) {
        NeAACDecFrameInfo info;
        memset(&info, 0, sizeof(info));
        int16_t* pcm = (int16_t*)NeAACDecDecode(dec->aac, &info,
                                                (unsigned char*)pkt,
                                                (unsigned long)pkt_len);
        if (!pcm || info.error != 0 || info.samples == 0 || info.channels == 0) {
            return 0;
        }
        int sr = info.samplerate > 0 ? (int)info.samplerate : dec->sample_rate;
        int ch = (int)info.channels;
        int frames = (int)(info.samples / (unsigned long)ch);
        if (frames > max_frames) frames = max_frames;

        if (ch == 2) {
            memcpy(out_pcm, pcm, (size_t)frames * 2u * sizeof(int16_t));
        } else if (ch == 1) {
            for (int i = 0; i < frames; i++) {
                out_pcm[i * 2]     = pcm[i];
                out_pcm[i * 2 + 1] = pcm[i];
            }
        } else {
            for (int i = 0; i < frames; i++) {
                out_pcm[i * 2]     = pcm[i * ch];
                out_pcm[i * 2 + 1] = pcm[i * ch + 1];
            }
        }
        if (out_sample_rate) *out_sample_rate = sr;
        return frames;
    }

    if (dec->codec == VP_ACODEC_MP3) {
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int samples = mp3dec_decode_frame(&dec->mp3, pkt, (int)pkt_len, dec->mp3_scratch, &info);
        if (samples <= 0 || info.channels <= 0) return 0;
        int frames = samples;
        if (frames > max_frames) frames = max_frames;
        if (info.channels == 2) {
            memcpy(out_pcm, dec->mp3_scratch, (size_t)frames * 2u * sizeof(int16_t));
        } else {
            for (int i = 0; i < frames; i++) {
                out_pcm[i * 2]     = dec->mp3_scratch[i];
                out_pcm[i * 2 + 1] = dec->mp3_scratch[i];
            }
        }
        if (out_sample_rate) *out_sample_rate = info.hz > 0 ? info.hz : dec->sample_rate;
        return frames;
    }

    if (dec->codec == VP_ACODEC_PCM) {
        int bytes_per_frame = dec->channels * (dec->bits / 8);
        if (bytes_per_frame <= 0) return 0;
        int frames = (int)(pkt_len / (size_t)bytes_per_frame);
        if (frames > max_frames) frames = max_frames;
        if (dec->bits == 16 && dec->channels == 2) {
            memcpy(out_pcm, pkt, (size_t)frames * 4u);
        } else if (dec->bits == 16 && dec->channels == 1) {
            const int16_t* s = (const int16_t*)pkt;
            for (int i = 0; i < frames; i++) {
                out_pcm[i * 2]     = s[i];
                out_pcm[i * 2 + 1] = s[i];
            }
        } else {
            return 0;
        }
        if (out_sample_rate) *out_sample_rate = dec->sample_rate;
        return frames;
    }

    return 0;
}

