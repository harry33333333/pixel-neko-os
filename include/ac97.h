#ifndef AC97_H
#define AC97_H

// Intel ICH / VIA / SiS / AMD AC'97 PCI Audio Controller Driver

#define AC97_DMA_BUFFER_BYTES  65536U
#define AC97_DMA_HALF_BYTES    32768U
#define AC97_DMA_TOTAL_FRAMES  (AC97_DMA_BUFFER_BYTES / 4U)

int ac97_init(void);
int ac97_is_present(void);
const char* ac97_get_name(void);
unsigned short ac97_get_io_base(void);

void ac97_set_format(int rate, int bits, int stereo);
void ac97_start_dma(void);
void ac97_stop_dma(void);
void ac97_pause_dma(int pause);
void ac97_set_volume(int master_vol, int pcm_vol);

unsigned int ac97_get_dma_frame_pos(void);
const void* ac97_get_dma_buffer(void);
void ac97_write_half(int half_idx, const void* data, unsigned int bytes);
void ac97_silence_half(int half_idx);
int ac97_poll_half_needed(void);
void ac97_mark_half_filled(int half_idx);

#endif // AC97_H

