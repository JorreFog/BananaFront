// audio.h: ALSA output (audio.c) or none (audio_null.c, PC tests).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BF_AUDIO_RATE 48000

typedef struct {
    unsigned underruns;
    size_t written, dropped;  // output frames
    double fill_sum;
    unsigned fill_n;
    double ratio, base_ratio, buffer_ms;
} bf_audio_stats;

bool bf_audio_open(const char* device, int latency_ms);
// the core's sound rate and frame rate, and the display's refresh: vsync_locked when one core frame runs per refresh
void bf_audio_set_rate(double core_rate, double core_fps, double display_hz, bool vsync_locked);
void bf_audio_mute(bool m);  // fast-forward, menu: the core's sound dropped
void bf_audio_get_stats(bf_audio_stats* st, bool reset);
double bf_audio_delay_ms(void);
void bf_audio_pause(bool on);
void bf_audio_raw(const int16_t* data, size_t frames);
void bf_audio_close(void);
