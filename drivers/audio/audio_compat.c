#include "audio.h"
#include <stdint.h>
#include <stdbool.h>
#include "string.h"

static audio_info_t g_audio_info;
static audio_state_t g_audio_state;
static int g_audio_volume = 80;
static bool g_audio_muted;

void audio_init(void) {
    memset(&g_audio_info, 0, sizeof(g_audio_info));
    g_audio_info.target_rate = 48000;
    g_audio_state = AUDIO_STATE_STOPPED;
}

void audio_poll(void) {
}

void enable_fpu(void) {
}

int audio_has_hardware(void) {
    return 0;
}

const char* audio_get_hardware_name(void) {
    return "PC Speaker";
}

const char* audio_get_probe_status(void) {
    return "PCM hardware validation deferred to P7";
}

int audio_play_file(const char* filepath) {
    (void)filepath;
    return -1;
}

void audio_pause(void) {
    if (g_audio_state == AUDIO_STATE_PLAYING) {
        g_audio_state = AUDIO_STATE_PAUSED;
    }
}

void audio_resume(void) {
    if (g_audio_state == AUDIO_STATE_PAUSED) {
        g_audio_state = AUDIO_STATE_PLAYING;
    }
}

void audio_stop(void) {
    g_audio_state = AUDIO_STATE_STOPPED;
}

void audio_seek_sec(unsigned int seconds) {
    g_audio_info.current_time_s =
        seconds > UINT32_MAX ? INT32_MAX : (int)seconds;
}

audio_state_t audio_get_state(void) {
    return g_audio_state;
}

const char* audio_get_title(void) {
    return g_audio_info.title[0] ? g_audio_info.title : "No audio hardware";
}

unsigned int audio_get_position_sec(void) {
    return (unsigned int)g_audio_info.current_time_s;
}

unsigned int audio_get_duration_sec(void) {
    return (unsigned int)g_audio_info.total_duration_s;
}

void audio_set_time(int current_s) {
    g_audio_info.current_time_s = current_s;
}

void audio_set_bitrate(int kbps, int is_vbr) {
    g_audio_info.bitrate_kbps = kbps;
    g_audio_info.is_vbr = is_vbr;
}

void audio_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    g_audio_volume = volume;
}

int audio_get_volume(void) {
    return g_audio_muted ? 0 : g_audio_volume;
}

void audio_toggle_mute(void) {
    g_audio_muted = !g_audio_muted;
}

int audio_is_muted(void) {
    return g_audio_muted;
}

const audio_info_t* audio_get_info(void) {
    return &g_audio_info;
}

int audio_feed_raw_pcm(const void* samples, unsigned int sample_count,
                       int source_rate, int source_channels, int source_bits,
                       int source_float) {
    (void)samples;
    (void)sample_count;
    (void)source_rate;
    (void)source_channels;
    (void)source_bits;
    (void)source_float;
    return 0;
}

void audio_flush_queue(void) {
}

void audio_get_spectrum(unsigned char bands[10]) {
    if (bands) memset(bands, 0, 10);
}

void audio_get_vu(unsigned char* left_vu, unsigned char* right_vu) {
    if (left_vu) *left_vu = 0;
    if (right_vu) *right_vu = 0;
}
