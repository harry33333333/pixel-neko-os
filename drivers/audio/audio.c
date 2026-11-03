// drivers/audio/audio.c - Unified 64-Bit Audio Engine, DSP & Stream Scheduler
#include "audio.h"
#include "ac97.h"
#include "cmi8738.h"
#include "ct4810.h"
#include "driver.h"
#include "flac_dec.h"
#include "hdaudio.h"
#include "log.h"
#include "mp3_dec.h"
#include "wav_dec.h"

#define DEC_NONE 0
#define DEC_WAV  1
#define DEC_FLAC 2
#define DEC_MP3  3

#define PCM_QUEUE_SIZE          65536
#define AUDIO_DMA_HALF_BYTES    32768
#define AUDIO_DMA_BUFFER_BYTES  65536

typedef struct {
    const char* (*get_name)(void);
    int (*is_present)(void);
    void (*set_format)(int rate, int bits, int stereo);
    void (*start_dma)(void);
    void (*stop_dma)(void);
    void (*pause_dma)(int pause);
    void (*set_volume)(int master, int pcm);
    int (*poll_half_needed)(void);
    void (*write_half)(int half, const void* data, unsigned int bytes);
    void (*silence_half)(int half);
    unsigned int (*get_dma_frame_pos)(void);
    const void* (*get_dma_buffer)(void);
} audio_driver_ops_t;

static const audio_driver_ops_t g_cmi8738_ops = {
    .get_name = cmi8738_get_name,
    .is_present = cmi8738_is_present,
    .set_format = cmi8738_set_format,
    .start_dma = cmi8738_start_dma,
    .stop_dma = cmi8738_stop_dma,
    .pause_dma = cmi8738_pause_dma,
    .set_volume = cmi8738_set_volume,
    .poll_half_needed = cmi8738_poll_half_needed,
    .write_half = cmi8738_write_half,
    .silence_half = cmi8738_silence_half,
    .get_dma_frame_pos = cmi8738_get_dma_frame_pos,
    .get_dma_buffer = cmi8738_get_dma_buffer,
};

static const audio_driver_ops_t g_hdaudio_ops = {
    .get_name = hdaudio_get_name,
    .is_present = hdaudio_is_present,
    .set_format = hdaudio_set_format,
    .start_dma = hdaudio_start_dma,
    .stop_dma = hdaudio_stop_dma,
    .pause_dma = hdaudio_pause_dma,
    .set_volume = hdaudio_set_volume,
    .poll_half_needed = hdaudio_poll_half_needed,
    .write_half = hdaudio_write_half,
    .silence_half = hdaudio_silence_half,
    .get_dma_frame_pos = hdaudio_get_dma_frame_pos,
    .get_dma_buffer = hdaudio_get_dma_buffer,
};

static const audio_driver_ops_t g_ac97_ops = {
    .get_name = ac97_get_name,
    .is_present = ac97_is_present,
    .set_format = ac97_set_format,
    .start_dma = ac97_start_dma,
    .stop_dma = ac97_stop_dma,
    .pause_dma = ac97_pause_dma,
    .set_volume = ac97_set_volume,
    .poll_half_needed = ac97_poll_half_needed,
    .write_half = ac97_write_half,
    .silence_half = ac97_silence_half,
    .get_dma_frame_pos = ac97_get_dma_frame_pos,
    .get_dma_buffer = ac97_get_dma_buffer,
};

static const audio_driver_ops_t g_ct4810_ops = {
    .get_name = ct4810_get_name,
    .is_present = ct4810_is_present,
    .set_format = ct4810_set_format,
    .start_dma = ct4810_start_dma,
    .stop_dma = ct4810_stop_dma,
    .pause_dma = ct4810_pause_dma,
    .set_volume = ct4810_set_volume,
    .poll_half_needed = ct4810_poll_half_needed,
    .write_half = ct4810_write_half,
    .silence_half = ct4810_silence_half,
    .get_dma_frame_pos = ct4810_get_dma_frame_pos,
    .get_dma_buffer = ct4810_get_dma_buffer,
};

static int probe_ct4810_driver(void) {
    return ct4810_init() == 0 && ct4810_is_present();
}

static int probe_cmi8738_driver(void) {
    return cmi8738_init() == 0 && cmi8738_is_present();
}

static int probe_ac97_driver(void) {
    return ac97_init() == 0 && ac97_is_present();
}

static int probe_hdaudio_driver(void) {
    return hdaudio_init() == 0 && hdaudio_is_present();
}

static const driver_descriptor_t g_hdaudio_driver = {
    "Intel/VIA High Definition Audio",
    DRIVER_CATEGORY_AUDIO,
    120,
    probe_hdaudio_driver,
    0,
    0
};

static const driver_descriptor_t g_ac97_driver = {
    "Intel/AMD AC'97 Audio",
    DRIVER_CATEGORY_AUDIO,
    110,
    probe_ac97_driver,
    0,
    0
};

static const driver_descriptor_t g_ct4810_driver = {
    "Ensoniq AudioPCI / CT4810",
    DRIVER_CATEGORY_AUDIO,
    100,
    probe_ct4810_driver,
    0,
    0
};

static const driver_descriptor_t g_cmi8738_driver = {
    "C-Media CMI8738",
    DRIVER_CATEGORY_AUDIO,
    90,
    probe_cmi8738_driver,
    0,
    0
};

static const audio_driver_ops_t* g_active_driver = 0;
static int g_audio_initialized = 0;

static audio_state_t g_audio_state = AUDIO_STATE_STOPPED;
static audio_info_t g_audio_info;
static int g_active_dec = DEC_NONE;

static int g_master_volume = 85;
static int g_is_muted = 0;

static int audio_requested_volume(void) {
    return g_is_muted ? 0 : g_master_volume;
}

static short g_pcm_queue[PCM_QUEUE_SIZE / sizeof(short)];
static unsigned int g_queue_read_idx = 0;
static unsigned int g_queue_write_idx = 0;
static unsigned int g_queue_bytes = 0;

static unsigned int g_total_frames_played = 0;
static int g_eof_reached = 0;

static unsigned char g_spectrum_bands[10] = {0};
static unsigned char g_vu_l = 0, g_vu_r = 0;

static inline unsigned int xorshift32(void) {
    static unsigned int seed = 0x12345678U;
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static inline short dither_s24_to_s16(int s24) {
    int r1 = (int)(xorshift32() & 0xFFU);
    int r2 = (int)(xorshift32() & 0xFFU);
    int dither = r1 - r2;
    int out = (s24 + dither) >> 8;
    if (out > 32767) return 32767;
    if (out < -32768) return -32768;
    return (short)out;
}

static inline int ieee754_to_s24(unsigned int bits) {
    int sign = (bits & 0x80000000U) ? -1 : 1;
    int exp = (int)((bits >> 23) & 0xFFU) - 127;
    unsigned int mant = (bits & 0x7FFFFFU) | 0x800000U;
    if (exp < -23) return 0;
    if (exp >= 0) return sign * 0x7FFFFF;
    return sign * (int)(mant >> (-exp));
}

static void my_strcpy(char* dest, const char* src) {
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

void enable_fpu(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |=  (1ULL << 1);
    cr0 &= ~(1ULL << 3);
    cr0 |=  (1ULL << 5);
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
}

void audio_init(void) {
    if (g_audio_initialized) return;
    g_audio_initialized = 1;

    enable_fpu();
    g_audio_state = AUDIO_STATE_STOPPED;
    g_active_dec = DEC_NONE;
    g_queue_read_idx = 0;
    g_queue_write_idx = 0;
    g_queue_bytes = 0;
    g_total_frames_played = 0;
    g_eof_reached = 0;
    g_is_muted = 0;
    g_master_volume = 85;

    for (int i = 0; i < 10; i++) g_spectrum_bands[i] = 0;
    g_vu_l = 0;
    g_vu_r = 0;

    g_active_driver = 0;
    driver_register(&g_hdaudio_driver);
    driver_register(&g_ac97_driver);
    driver_register(&g_ct4810_driver);
    driver_register(&g_cmi8738_driver);
    driver_probe_category(DRIVER_CATEGORY_AUDIO);

    if (driver_is_active(DRIVER_CATEGORY_AUDIO,
                         "Intel/VIA High Definition Audio")) {
        g_active_driver = &g_hdaudio_ops;
    } else if (driver_is_active(DRIVER_CATEGORY_AUDIO,
                                "Intel/AMD AC'97 Audio")) {
        g_active_driver = &g_ac97_ops;
    } else if (driver_is_active(DRIVER_CATEGORY_AUDIO,
                                "Ensoniq AudioPCI / CT4810")) {
        g_active_driver = &g_ct4810_ops;
    } else if (driver_is_active(DRIVER_CATEGORY_AUDIO, "C-Media CMI8738")) {
        g_active_driver = &g_cmi8738_ops;
    }

    if (g_active_driver) {
        g_active_driver->set_volume(g_master_volume, g_master_volume);
    }
}

int audio_has_hardware(void) {
    if (!g_audio_initialized) audio_init();
    return (g_active_driver != 0 && g_active_driver->is_present());
}

const char* audio_get_hardware_name(void) {
    if (!g_audio_initialized) audio_init();
    if (g_active_driver) {
        return g_active_driver->get_name();
    }
    return "No Audio Device";
}

const char* audio_get_probe_status(void) {
    if (!g_audio_initialized) audio_init();
    if (g_active_driver) return g_active_driver->get_name();
    return hdaudio_get_status();
}

void audio_flush_queue(void) {
    g_queue_read_idx = 0;
    g_queue_write_idx = 0;
    g_queue_bytes = 0;
}

int audio_feed_raw_pcm(const void* samples, unsigned int sample_count,
                       int src_rate, int src_channels, int src_bits,
                       int src_float) {
    if (!samples || sample_count == 0) return 0;

    int target_rate = g_audio_info.target_rate;
    if (target_rate <= 0) target_rate = 44100;

    int decimation_ratio = 1;
    if (src_rate >= target_rate * 4) {
        decimation_ratio = 4;
    } else if (src_rate >= target_rate * 2) {
        decimation_ratio = 2;
    }

    if (decimation_ratio == 1 && src_bits == 16 && src_channels == 2 &&
        !src_float) {
        const short* src_s16 = (const short*)samples;
        unsigned int space_frames = (PCM_QUEUE_SIZE - g_queue_bytes) / 4;
        unsigned int to_copy =
            (sample_count < space_frames) ? sample_count : space_frames;
        for (unsigned int i = 0; i < to_copy; i++) {
            unsigned int w_idx = g_queue_write_idx;
            g_pcm_queue[w_idx] = src_s16[i * 2];
            g_pcm_queue[w_idx + 1] = src_s16[i * 2 + 1];
            g_queue_write_idx =
                (w_idx + 2) % (PCM_QUEUE_SIZE / sizeof(short));
            g_queue_bytes += 4;
        }
        return (int)to_copy;
    }

    if (decimation_ratio == 2 && src_bits == 16 && src_channels == 2 &&
        !src_float) {
        const short* src_s16 = (const short*)samples;
        unsigned int space_frames = (PCM_QUEUE_SIZE - g_queue_bytes) / 4;
        unsigned int out_frames = sample_count / 2;
        unsigned int to_copy =
            (out_frames < space_frames) ? out_frames : space_frames;
        for (unsigned int i = 0; i < to_copy; i++) {
            unsigned int idx = i * 4;
            short l = (short)(((int)src_s16[idx] + (int)src_s16[idx + 2]) >> 1);
            short r = (short)(((int)src_s16[idx + 1] + (int)src_s16[idx + 3]) >> 1);
            unsigned int w_idx = g_queue_write_idx;
            g_pcm_queue[w_idx] = l;
            g_pcm_queue[w_idx + 1] = r;
            g_queue_write_idx =
                (w_idx + 2) % (PCM_QUEUE_SIZE / sizeof(short));
            g_queue_bytes += 4;
        }
        return (int)(to_copy * 2);
    }

    if (decimation_ratio == 4 && src_bits == 16 && src_channels == 2 &&
        !src_float) {
        const short* src_s16 = (const short*)samples;
        unsigned int space_frames = (PCM_QUEUE_SIZE - g_queue_bytes) / 4;
        unsigned int out_frames = sample_count / 4;
        unsigned int to_copy =
            (out_frames < space_frames) ? out_frames : space_frames;
        for (unsigned int i = 0; i < to_copy; i++) {
            unsigned int idx = i * 8;
            int sum_l = (int)src_s16[idx] + (int)src_s16[idx + 2] +
                        (int)src_s16[idx + 4] + (int)src_s16[idx + 6];
            int sum_r = (int)src_s16[idx + 1] + (int)src_s16[idx + 3] +
                        (int)src_s16[idx + 5] + (int)src_s16[idx + 7];
            unsigned int w_idx = g_queue_write_idx;
            g_pcm_queue[w_idx] = (short)(sum_l >> 2);
            g_pcm_queue[w_idx + 1] = (short)(sum_r >> 2);
            g_queue_write_idx =
                (w_idx + 2) % (PCM_QUEUE_SIZE / sizeof(short));
            g_queue_bytes += 4;
        }
        return (int)(to_copy * 4);
    }

    const unsigned char* src_bytes = (const unsigned char*)samples;
    int bytes_per_sample = (src_bits + 7) / 8;
    int frame_size = bytes_per_sample * src_channels;
    unsigned int produced_frames = 0;

    for (unsigned int i = 0; i < sample_count;
         i += (unsigned int)decimation_ratio) {
        if (g_queue_bytes + 4 > PCM_QUEUE_SIZE) break;

        const unsigned char* p = src_bytes + i * (unsigned int)frame_size;
        int s24_l = 0, s24_r = 0;

        if (src_float && src_bits == 32) {
            unsigned int u_l = *(const unsigned int*)p;
            s24_l = ieee754_to_s24(u_l);
            s24_r = (src_channels > 1)
                        ? ieee754_to_s24(*(const unsigned int*)(p + 4))
                        : s24_l;
        } else if (src_bits == 16) {
            s24_l = *(const short*)p << 8;
            s24_r = (src_channels > 1) ? (*(const short*)(p + 2) << 8) : s24_l;
        } else if (src_bits == 24) {
            s24_l = (int)(((unsigned int)p[0]) |
                          ((unsigned int)p[1] << 8) |
                          ((unsigned int)(signed char)p[2] << 16));
            s24_r = (src_channels > 1)
                        ? (int)(((unsigned int)p[3]) |
                                ((unsigned int)p[4] << 8) |
                                ((unsigned int)(signed char)p[5] << 16))
                        : s24_l;
        } else if (src_bits == 32) {
            s24_l = *(const int*)p >> 8;
            s24_r = (src_channels > 1) ? (*(const int*)(p + 4) >> 8) : s24_l;
        } else if (src_bits == 8) {
            s24_l = ((int)p[0] - 128) << 16;
            s24_r = (src_channels > 1) ? (((int)p[1] - 128) << 16) : s24_l;
        }

        short s16_l = dither_s24_to_s16(s24_l);
        short s16_r = dither_s24_to_s16(s24_r);

        unsigned int w_idx = g_queue_write_idx;
        g_pcm_queue[w_idx] = s16_l;
        g_pcm_queue[w_idx + 1] = s16_r;
        g_queue_write_idx = (w_idx + 2) % (PCM_QUEUE_SIZE / sizeof(short));
        g_queue_bytes += 4;
        produced_frames++;
    }

    return (int)produced_frames;
}

int audio_play_file(const char* filepath) {
    if (!filepath || !filepath[0]) return -1;

    enable_fpu();
    audio_stop();

    if (!audio_has_hardware()) {
        return -2;
    }

    int len = my_strlen(filepath);
    char ext[5] = {0};
    int dot_pos = -1;
    for (int i = len - 1; i >= 0 && i >= len - 5; i--) {
        if (filepath[i] == '.') {
            dot_pos = i;
            break;
        }
    }
    if (dot_pos >= 0) {
        int k = 0;
        for (int i = dot_pos + 1; i < len && k < 4; i++) {
            char c = filepath[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            ext[k++] = c;
        }
        ext[k] = '\0';
    }

    int open_ok = -1;
    g_audio_info.title[0] = '\0';
    g_audio_info.current_time_s = 0;
    g_total_frames_played = 0;
    g_eof_reached = 0;

    if (ext[0] == 'w' && ext[1] == 'a' && ext[2] == 'v') {
        wav_info_t wi;
        open_ok = wav_dec_open(filepath, &wi);
        if (open_ok == 0) {
            g_active_dec = DEC_WAV;
            g_audio_info.sample_rate = wi.sample_rate;
            g_audio_info.channels = wi.channels;
            g_audio_info.bit_depth = wi.bit_depth;
            g_audio_info.is_float = wi.is_float;
            g_audio_info.total_duration_s = (int)wi.duration_s;
            g_audio_info.bitrate_kbps =
                (wi.sample_rate * wi.channels * wi.bit_depth) / 1000;
            g_audio_info.is_vbr = 0;
            my_strcpy(g_audio_info.title, filepath);
        }
    } else if (ext[0] == 'f' && ext[1] == 'l' &&
               ext[2] == 'a' && ext[3] == 'c') {
        flac_info_t fi;
        open_ok = flac_dec_open(filepath, &fi);
        if (open_ok == 0) {
            g_active_dec = DEC_FLAC;
            g_audio_info.sample_rate = fi.sample_rate;
            g_audio_info.channels = 2;
            g_audio_info.bit_depth = 16;
            g_audio_info.is_float = 0;
            g_audio_info.total_duration_s = (int)fi.duration_s;
            g_audio_info.bitrate_kbps = 0;
            g_audio_info.is_vbr = 1;
            if (fi.title[0]) my_strcpy(g_audio_info.title, fi.title);
            else my_strcpy(g_audio_info.title, filepath);
        }
    } else if (ext[0] == 'm' && ext[1] == 'p' && ext[2] == '3') {
        mp3_info_t mi;
        open_ok = mp3_dec_open(filepath, &mi);
        if (open_ok == 0) {
            g_active_dec = DEC_MP3;
            g_audio_info.sample_rate = mi.sample_rate;
            g_audio_info.channels = 2;
            g_audio_info.bit_depth = 16;
            g_audio_info.is_float = 0;
            g_audio_info.total_duration_s = mi.duration_s;
            g_audio_info.bitrate_kbps = mi.bitrate_kbps;
            g_audio_info.is_vbr = mi.is_vbr;
            if (mi.title[0]) my_strcpy(g_audio_info.title, mi.title);
            else my_strcpy(g_audio_info.title, filepath);
        }
    }

    if (open_ok != 0) {
        g_active_dec = DEC_NONE;
        return -3;
    }

    int srate = g_audio_info.sample_rate;
    if (srate % 44100 == 0 || srate == 22050 || srate == 11025) {
        g_audio_info.target_rate = 44100;
        if (g_active_driver) g_active_driver->set_format(44100, 16, 1);
    } else {
        g_audio_info.target_rate = 48000;
        if (g_active_driver) g_active_driver->set_format(48000, 16, 1);
    }

    audio_flush_queue();
    g_total_frames_played = 0;
    g_eof_reached = 0;

    if (g_active_driver) {
        g_active_driver->silence_half(0);
        g_active_driver->silence_half(1);
    }

    static unsigned char dec_temp_buf[16384];
    for (int pre = 0;
         pre < 32 && g_queue_bytes < AUDIO_DMA_HALF_BYTES && !g_eof_reached;
         pre++) {
        int samples_read = 0;
        if (g_active_dec == DEC_WAV) {
            samples_read = wav_dec_read_samples(dec_temp_buf, 2048);
        } else if (g_active_dec == DEC_FLAC) {
            samples_read = flac_dec_read_samples(dec_temp_buf, 4096);
        } else if (g_active_dec == DEC_MP3) {
            samples_read = mp3_dec_read_samples(dec_temp_buf, 1152);
        }

        if (samples_read > 0) {
            audio_feed_raw_pcm(dec_temp_buf, (unsigned int)samples_read,
                               g_audio_info.sample_rate,
                               g_audio_info.channels,
                               g_audio_info.bit_depth,
                               g_audio_info.is_float);
        } else if (samples_read < 0) {
            g_eof_reached = 1;
            break;
        }
    }

    if (g_queue_bytes >= AUDIO_DMA_HALF_BYTES) {
        static short dma_copy_buf[AUDIO_DMA_HALF_BYTES / sizeof(short)];
        unsigned int samples_needed = AUDIO_DMA_HALF_BYTES / sizeof(short);
        for (unsigned int i = 0; i < samples_needed; i++) {
            dma_copy_buf[i] = g_pcm_queue[g_queue_read_idx];
            g_queue_read_idx =
                (g_queue_read_idx + 1) % (PCM_QUEUE_SIZE / sizeof(short));
        }
        g_queue_bytes -= AUDIO_DMA_HALF_BYTES;
        if (g_active_driver) {
            g_active_driver->write_half(0, dma_copy_buf, AUDIO_DMA_HALF_BYTES);
        }
    }

    for (int pre = 0;
         pre < 32 && g_queue_bytes < AUDIO_DMA_HALF_BYTES && !g_eof_reached;
         pre++) {
        int samples_read = 0;
        if (g_active_dec == DEC_WAV) {
            samples_read = wav_dec_read_samples(dec_temp_buf, 2048);
        } else if (g_active_dec == DEC_FLAC) {
            samples_read = flac_dec_read_samples(dec_temp_buf, 4096);
        } else if (g_active_dec == DEC_MP3) {
            samples_read = mp3_dec_read_samples(dec_temp_buf, 1152);
        }

        if (samples_read > 0) {
            audio_feed_raw_pcm(dec_temp_buf, (unsigned int)samples_read,
                               g_audio_info.sample_rate,
                               g_audio_info.channels,
                               g_audio_info.bit_depth,
                               g_audio_info.is_float);
        } else if (samples_read < 0) {
            g_eof_reached = 1;
            break;
        }
    }

    if (g_queue_bytes >= AUDIO_DMA_HALF_BYTES) {
        static short dma_copy_buf[AUDIO_DMA_HALF_BYTES / sizeof(short)];
        unsigned int samples_needed = AUDIO_DMA_HALF_BYTES / sizeof(short);
        for (unsigned int i = 0; i < samples_needed; i++) {
            dma_copy_buf[i] = g_pcm_queue[g_queue_read_idx];
            g_queue_read_idx =
                (g_queue_read_idx + 1) % (PCM_QUEUE_SIZE / sizeof(short));
        }
        g_queue_bytes -= AUDIO_DMA_HALF_BYTES;
        if (g_active_driver) {
            g_active_driver->write_half(1, dma_copy_buf, AUDIO_DMA_HALF_BYTES);
        }
    }

    if (g_active_driver) {
        g_active_driver->start_dma();
        int volume = audio_requested_volume();
        g_active_driver->set_volume(volume, volume);
    }
    g_audio_state = AUDIO_STATE_PLAYING;

    return 0;
}

void audio_pause(void) {
    if (g_audio_state == AUDIO_STATE_PLAYING) {
        if (g_active_driver) {
            g_active_driver->set_volume(0, 0);
            g_active_driver->pause_dma(1);
        }
        g_audio_state = AUDIO_STATE_PAUSED;
        for (int i = 0; i < 10; i++) g_spectrum_bands[i] = 0;
        g_vu_l = 0;
        g_vu_r = 0;
    }
}

void audio_resume(void) {
    if (g_audio_state == AUDIO_STATE_PAUSED) {
        if (g_active_driver) {
            int volume = audio_requested_volume();
            g_active_driver->set_volume(volume, volume);
            g_active_driver->pause_dma(0);
        }
        g_audio_state = AUDIO_STATE_PLAYING;
    }
}

void audio_stop(void) {
    if (g_audio_state != AUDIO_STATE_STOPPED) {
        if (g_active_driver) g_active_driver->stop_dma();
        g_audio_state = AUDIO_STATE_STOPPED;
        if (g_active_driver) {
            int volume = audio_requested_volume();
            g_active_driver->set_volume(volume, volume);
        }
    }
    if (g_active_dec == DEC_WAV) wav_dec_close();
    else if (g_active_dec == DEC_FLAC) flac_dec_close();
    else if (g_active_dec == DEC_MP3) mp3_dec_close();

    g_active_dec = DEC_NONE;
    audio_flush_queue();
    g_total_frames_played = 0;
    g_eof_reached = 0;
    for (int i = 0; i < 10; i++) g_spectrum_bands[i] = 0;
    g_vu_l = 0;
    g_vu_r = 0;
}

void audio_seek_sec(unsigned int seconds) {
    if (g_active_dec == DEC_NONE) return;

    enable_fpu();

    if (g_active_dec == DEC_WAV) wav_dec_seek(seconds);
    else if (g_active_dec == DEC_FLAC) flac_dec_seek(seconds);
    else if (g_active_dec == DEC_MP3) mp3_dec_seek(seconds);

    audio_flush_queue();
    if (g_active_driver) {
        g_active_driver->silence_half(0);
        g_active_driver->silence_half(1);
    }
    g_audio_info.current_time_s = (int)seconds;
    g_total_frames_played =
        seconds * (unsigned int)g_audio_info.target_rate;
    g_eof_reached = 0;

    static unsigned char dec_temp_buf[16384];
    for (int pre = 0;
         pre < 16 && g_queue_bytes < AUDIO_DMA_HALF_BYTES && !g_eof_reached;
         pre++) {
        int samples_read = 0;
        if (g_active_dec == DEC_WAV) {
            samples_read = wav_dec_read_samples(dec_temp_buf, 2048);
        } else if (g_active_dec == DEC_FLAC) {
            samples_read = flac_dec_read_samples(dec_temp_buf, 4096);
        } else if (g_active_dec == DEC_MP3) {
            samples_read = mp3_dec_read_samples(dec_temp_buf, 1152);
        }

        if (samples_read > 0) {
            audio_feed_raw_pcm(dec_temp_buf, (unsigned int)samples_read,
                               g_audio_info.sample_rate,
                               g_audio_info.channels,
                               g_audio_info.bit_depth,
                               g_audio_info.is_float);
        } else if (samples_read < 0) {
            g_eof_reached = 1;
            break;
        }
    }
}

audio_state_t audio_get_state(void) {
    return g_audio_state;
}

const char* audio_get_title(void) {
    return g_audio_info.title;
}

unsigned int audio_get_position_sec(void) {
    return (unsigned int)g_audio_info.current_time_s;
}

unsigned int audio_get_duration_sec(void) {
    return (unsigned int)g_audio_info.total_duration_s;
}

void audio_set_time(int current_s) {
    if (current_s >= 0) {
        g_audio_info.current_time_s = current_s;
    }
}

void audio_set_bitrate(int kbps, int is_vbr) {
    g_audio_info.bitrate_kbps = kbps;
    g_audio_info.is_vbr = is_vbr;
}

void audio_set_volume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_master_volume = vol;
    if (g_active_driver) {
        int v = (g_audio_state == AUDIO_STATE_PAUSED)
                    ? 0
                    : audio_requested_volume();
        g_active_driver->set_volume(v, v);
    }
}

int audio_get_volume(void) {
    return g_is_muted ? 0 : g_master_volume;
}

void audio_toggle_mute(void) {
    g_is_muted = !g_is_muted;
    if (g_active_driver) {
        int v = (g_audio_state == AUDIO_STATE_PAUSED)
                    ? 0
                    : audio_requested_volume();
        g_active_driver->set_volume(v, v);
    }
}

int audio_is_muted(void) {
    return g_is_muted;
}

const audio_info_t* audio_get_info(void) {
    return &g_audio_info;
}

void audio_poll(void) {
    if (g_audio_state != AUDIO_STATE_PLAYING) return;
    if (!audio_has_hardware() || !g_active_driver) return;

    int dec_count = 0;
    static unsigned char dec_temp_buf[16384];
    while (!g_eof_reached && g_queue_bytes < (PCM_QUEUE_SIZE - 16384) &&
           dec_count < 6) {
        dec_count++;
        int samples_read = 0;

        if (g_active_dec == DEC_WAV) {
            samples_read = wav_dec_read_samples(dec_temp_buf, 2048);
        } else if (g_active_dec == DEC_FLAC) {
            samples_read = flac_dec_read_samples(dec_temp_buf, 4096);
        } else if (g_active_dec == DEC_MP3) {
            samples_read = mp3_dec_read_samples(dec_temp_buf, 1152);
        }

        if (samples_read > 0) {
            audio_feed_raw_pcm(dec_temp_buf, (unsigned int)samples_read,
                               g_audio_info.sample_rate,
                               g_audio_info.channels,
                               g_audio_info.bit_depth,
                               g_audio_info.is_float);
        } else if (samples_read < 0) {
            g_eof_reached = 1;
            break;
        } else {
            break;
        }
    }

    int half_needed = g_active_driver->poll_half_needed();
    if (half_needed >= 0) {
        static short dma_copy_buf[AUDIO_DMA_HALF_BYTES / sizeof(short)];
        if (g_queue_bytes >= AUDIO_DMA_HALF_BYTES) {
            unsigned int samples_needed = AUDIO_DMA_HALF_BYTES / sizeof(short);
            for (unsigned int i = 0; i < samples_needed; i++) {
                dma_copy_buf[i] = g_pcm_queue[g_queue_read_idx];
                g_queue_read_idx =
                    (g_queue_read_idx + 1) % (PCM_QUEUE_SIZE / sizeof(short));
            }
            g_queue_bytes -= AUDIO_DMA_HALF_BYTES;
            g_active_driver->write_half(
                half_needed, dma_copy_buf, AUDIO_DMA_HALF_BYTES);
            g_total_frames_played += (AUDIO_DMA_HALF_BYTES / 4);
        } else if (g_queue_bytes > 0) {
            unsigned int avail_samples = g_queue_bytes / sizeof(short);
            for (unsigned int i = 0; i < avail_samples; i++) {
                dma_copy_buf[i] = g_pcm_queue[g_queue_read_idx];
                g_queue_read_idx =
                    (g_queue_read_idx + 1) % (PCM_QUEUE_SIZE / sizeof(short));
            }
            for (unsigned int i = avail_samples;
                 i < AUDIO_DMA_HALF_BYTES / sizeof(short);
                 i++) {
                dma_copy_buf[i] = 0;
            }
            g_queue_bytes = 0;
            g_active_driver->write_half(
                half_needed, dma_copy_buf, AUDIO_DMA_HALF_BYTES);
            g_total_frames_played += (avail_samples / 2);
        } else {
            g_active_driver->silence_half(half_needed);
            if (g_eof_reached) {
                audio_stop();
                return;
            }
        }

        int tr = g_audio_info.target_rate;
        if (tr > 0) {
            g_audio_info.current_time_s =
                (int)(g_total_frames_played / (unsigned int)tr);
        }
    }

    unsigned int frame = g_active_driver->get_dma_frame_pos();
    unsigned int sample_offset =
        (frame * 2) % (AUDIO_DMA_BUFFER_BYTES / sizeof(short));
    const short* dma_samples = (const short*)g_active_driver->get_dma_buffer();
    if (dma_samples) {
        int peak_l = 0, peak_r = 0;
        int band_acc[10] = {0};

        for (int i = 0; i < 128; i++) {
            unsigned int idx =
                (sample_offset + (unsigned int)(i * 2)) %
                (AUDIO_DMA_BUFFER_BYTES / sizeof(short));
            int sl = dma_samples[idx];
            int sr = dma_samples[idx + 1];
            if (sl < 0) sl = -sl;
            if (sr < 0) sr = -sr;
            if (sl > peak_l) peak_l = sl;
            if (sr > peak_r) peak_r = sr;

            int diff = sl - sr;
            if (diff < 0) diff = -diff;
            int sum = (sl + sr) >> 1;
            band_acc[i % 10] += (i < 40) ? (sum >> 3) : (diff >> 2);
        }

        int cur_l = (peak_l * 255) / 32768;
        int cur_r = (peak_r * 255) / 32768;
        if (cur_l > g_vu_l) g_vu_l = (unsigned char)cur_l;
        else g_vu_l = (unsigned char)((g_vu_l * 8) / 10);
        if (cur_r > g_vu_r) g_vu_r = (unsigned char)cur_r;
        else g_vu_r = (unsigned char)((g_vu_r * 8) / 10);

        for (int b = 0; b < 10; b++) {
            int target = (band_acc[b] * 16) / 128;
            if (target > 15) target = 15;
            if (target > (int)g_spectrum_bands[b]) {
                g_spectrum_bands[b] = (unsigned char)target;
            } else if (g_spectrum_bands[b] > 0) {
                g_spectrum_bands[b]--;
            }
        }
    }
}

void audio_get_spectrum(unsigned char bands[10]) {
    for (int i = 0; i < 10; i++) {
        bands[i] = g_spectrum_bands[i];
    }
}

void audio_get_vu(unsigned char* left_vu, unsigned char* right_vu) {
    if (left_vu) *left_vu = g_vu_l;
    if (right_vu) *right_vu = g_vu_r;
}
