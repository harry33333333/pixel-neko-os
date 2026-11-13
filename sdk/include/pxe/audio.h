#ifndef _PXE_AUDIO_H
#define _PXE_AUDIO_H

#include "abi.h"

int  pxe_audio_play_file(const char* path);
void pxe_audio_pause(void);
void pxe_audio_resume(void);
void pxe_audio_stop(void);
void pxe_audio_seek(uint32_t seconds);
void pxe_audio_set_volume(int volume);
void pxe_audio_toggle_mute(void);
int  pxe_audio_get_status(pxe_audio_status_t* out_status);
int  pxe_audio_feed_pcm(const void* samples, uint32_t sample_count,
                        int src_rate, int src_channels, int src_bits, int src_float);

#endif /* _PXE_AUDIO_H */

