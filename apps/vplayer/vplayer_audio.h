#ifndef _VPLAYER_AUDIO_H
#define _VPLAYER_AUDIO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VP_ACODEC_NONE 0
#define VP_ACODEC_AAC  1
#define VP_ACODEC_MP3  2
#define VP_ACODEC_PCM  3

typedef struct vp_audio_dec vp_audio_dec_t;

vp_audio_dec_t* vp_audio_dec_create(int codec,
                                    const uint8_t* dsi, size_t dsi_len,
                                    int sample_rate, int channels, int bits);
void vp_audio_dec_destroy(vp_audio_dec_t* dec);
void vp_audio_dec_reset(vp_audio_dec_t* dec);

/*
 * Decode one audio packet into interleaved signed 16-bit stereo PCM frames.
 * Returns number of stereo frames written to out_pcm (each frame = 2 int16_t = 4 bytes),
 * or <= 0 if no output produced / error.
 */
int vp_audio_dec_decode(vp_audio_dec_t* dec,
                        const uint8_t* pkt, size_t pkt_len,
                        int16_t* out_pcm, int max_frames,
                        int* out_sample_rate);

#ifdef __cplusplus
}
#endif

#endif /* _VPLAYER_AUDIO_H */

