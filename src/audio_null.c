// audio_null.c: no sound device (PC tests): the samples counted and hashed after the resampler's input.
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "bf.h"

static uint64_t hash = 1469598103934665603ull, samples;
static bf_audio_stats st;

bool bf_audio_open(const char* device, int latency_ms) { (void)device; (void)latency_ms; return true; }
void bf_audio_set_rate(double r, double f, double d, bool l) { (void)r; (void)f; (void)d; (void)l; }
void bf_audio_mute(bool m) { (void)m; }
size_t bf_audio_batch(const int16_t* data, size_t frames)
{
    const uint8_t* b = (const uint8_t*)data;
    for (size_t i = 0; i < frames * 4; i++) { hash ^= b[i]; hash *= 1099511628211ull; }
    samples += frames;
    return frames;
}
void bf_audio_get_stats(bf_audio_stats* out, bool reset) { *out = st; if (reset) memset(&st, 0, sizeof st); }
double bf_audio_delay_ms(void) { return 0; }
void bf_audio_close(void)
{
    if (samples) bf_log("audio: none (null driver), %llu frames, hash %016llx", (unsigned long long)samples, (unsigned long long)hash);
    samples = 0;
}
void bf_audio_pause(bool on) { (void)on; }
void bf_audio_raw(const int16_t* data, size_t frames) { (void)data; (void)frames; }
double bf_audio_ms_take(void) { return 0; }
void bf_audio_flush(void) {}
