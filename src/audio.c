// audio.c: ALSA, 48 kHz stereo S16 (the amps' own rate: the Plus's aw88166 runs 48 kHz whatever it is fed), written
// directly (no PipeWire: 4-9 % of a core), nonblocking. The core's sound is resampled (resample.c) at a ratio set
// from the display: main runs one core frame per refresh, so the core's rate is scaled by refresh/fps, and dynamic
// rate control nudges it by at most 0.5 % by how full the device buffer is (RetroArch's method, against the display
// clock, not a servo that hunts on the ring level).
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "bf.h"

static snd_pcm_t* pcm;
static snd_pcm_uframes_t buf_frames, period_frames;
static bf_resampler rs;
static double base_ratio = 1.0, cur_ratio = 1.0, drc_delta = 0.005;
static bool muted;
static int16_t* acc;   // this refresh's sound, resampled, waiting for bf_audio_flush
static size_t acc_n, acc_cap;
static unsigned calls;
static bf_audio_stats st;

bool bf_audio_open(const char* device, int latency_ms)
{
    int e = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (e < 0) { bf_log("audio: %s: %s", device, snd_strerror(e)); pcm = NULL; return false; }
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    unsigned rate = BF_AUDIO_RATE;
    unsigned buf_us = (unsigned)latency_ms * 1000, per_us = buf_us / 4;
    if ((e = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (e = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (e = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
        (e = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL)) < 0 ||
        (e = snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buf_us, NULL)) < 0 ||
        (e = snd_pcm_hw_params_set_period_time_near(pcm, hw, &per_us, NULL)) < 0 ||
        (e = snd_pcm_hw_params(pcm, hw)) < 0) {
        bf_log("audio: %s: %s", device, snd_strerror(e));
        snd_pcm_close(pcm);
        pcm = NULL;
        return false;
    }
    snd_pcm_hw_params_get_buffer_size(hw, &buf_frames);
    snd_pcm_hw_params_get_period_size(hw, &period_frames, NULL);
    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    // starts once half the buffer is there: the target fill DRC keeps
    snd_pcm_sw_params_set_start_threshold(pcm, sw, buf_frames / 2);
    snd_pcm_sw_params(pcm, sw);
    if (rate != BF_AUDIO_RATE) bf_log("audio: the device gave %u Hz, not %d", rate, BF_AUDIO_RATE);
    bf_log("audio: %s %u Hz, buffer %lu frames (%.1f ms), period %lu", device, rate, (unsigned long)buf_frames,
           buf_frames * 1000.0 / rate, (unsigned long)period_frames);
    bf_resampler_reset(&rs);
    return true;
}

void bf_audio_set_rate(double core_rate, double core_fps, double display_hz, bool vsync_locked)
{
    double in = core_rate;
    if (vsync_locked && core_fps > 0 && display_hz > 0) in = core_rate * display_hz / core_fps;
    base_ratio = BF_AUDIO_RATE / in;
    cur_ratio = base_ratio;
}

void bf_audio_mute(bool m) { muted = m; if (m) acc_n = 0; }

static void write_silence(snd_pcm_uframes_t n)
{
    static const int16_t z[2 * 256];
    while (n) {
        snd_pcm_uframes_t k = n > 256 ? 256 : n;
        if (snd_pcm_writei(pcm, z, k) < 0) break;
        n -= k;
    }
}

static double audio_ms;  // time spent here (the frontend's cost), taken by the stats line
double bf_audio_ms_take(void) { double v = audio_ms; audio_ms = 0; return v; }
static size_t audio_batch_(const int16_t* data, size_t frames);
size_t bf_audio_batch(const int16_t* data, size_t frames)
{
    const double t = bf_now_ms();
    size_t r = audio_batch_(data, frames);
    audio_ms += bf_now_ms() - t;
    return r;
}
// the core's sound of a frame is resampled as it comes and written to the device once per refresh
// (bf_audio_flush): cores call the batch callback several times a frame, and each write is a trip through
// ALSA's plugin chain and the kernel
static size_t audio_batch_(const int16_t* data, size_t frames)
{
    if (!pcm || muted || !frames) return frames;
    calls++;
    const size_t want = (size_t)(frames * cur_ratio) + 8;
    if (acc_n + want > acc_cap) { acc_cap = (acc_n + want) * 2; acc = realloc(acc, acc_cap * 4); }
    acc_n += bf_resample(&rs, data, frames, acc + acc_n * 2, want, cur_ratio);
    return frames;
}

void bf_audio_flush(void)
{
    const double t0 = bf_now_ms();
    if (!pcm || !acc_n) { acc_n = 0; return; }
    snd_pcm_sframes_t avail = snd_pcm_avail(pcm);
    if (avail < 0) {
        if (avail == -EPIPE) st.underruns++;
        snd_pcm_recover(pcm, (int)avail, 1);
        snd_pcm_prepare(pcm);
        // half a buffer of silence: the fill DRC aims for, so it doesn't have to climb back from empty
        write_silence(buf_frames / 2);
        avail = snd_pcm_avail(pcm);
        if (avail < 0) { acc_n = 0; return; }
    }
    // the ratio for the next frame's sound, from how full the device is before this frame's goes in
    const double fill = 1.0 - (double)avail / buf_frames;
    cur_ratio = bf_drc_ratio(base_ratio, fill, drc_delta);
    st.fill_sum += fill;
    st.fill_n++;
    snd_pcm_sframes_t w = snd_pcm_writei(pcm, acc, acc_n);
    if (w == -EPIPE) {
        st.underruns++;
        snd_pcm_prepare(pcm);
        write_silence(buf_frames / 2);
        w = snd_pcm_writei(pcm, acc, acc_n);
    }
    // what didn't fit (a core that sends its sound in bursts: fake-08) waits for the next refresh instead of being
    // dropped; only past a buffer's worth is any let go
    const size_t done = w > 0 ? (size_t)w : 0;
    st.written += done;
    size_t left = acc_n - done;
    if (left > buf_frames) { st.dropped += left - buf_frames; memmove(acc, acc + (acc_n - buf_frames) * 2, buf_frames * 4); left = buf_frames; }
    else if (done && left) memmove(acc, acc + done * 2, left * 4);
    acc_n = left;
    audio_ms += bf_now_ms() - t0;
}

// the menu: the game's sound stopped (what's queued dropped), the device free for the menu's own sounds
void bf_audio_pause(bool on)
{
    if (!pcm) return;
    if (on) { snd_pcm_drop(pcm); snd_pcm_prepare(pcm); }
    else {
        snd_pcm_drop(pcm);
        snd_pcm_prepare(pcm);
        write_silence(buf_frames / 2);  // (the fill DRC aims for)
        bf_resampler_reset(&rs);
    }
}

// 48 kHz stereo frames straight to the device (the menu's sounds); what doesn't fit is dropped
void bf_audio_raw(const int16_t* data, size_t frames)
{
    if (!pcm) return;
    snd_pcm_sframes_t w = snd_pcm_writei(pcm, data, frames);
    if (w == -EPIPE) { snd_pcm_prepare(pcm); snd_pcm_writei(pcm, data, frames); }
}

void bf_audio_get_stats(bf_audio_stats* out, bool reset)
{
    *out = st;
    out->ratio = cur_ratio;
    out->base_ratio = base_ratio;
    out->buffer_ms = pcm ? buf_frames * 1000.0 / BF_AUDIO_RATE : 0;
    if (reset) memset(&st, 0, sizeof st);
}

double bf_audio_delay_ms(void)
{
    snd_pcm_sframes_t d = 0;
    if (!pcm || snd_pcm_delay(pcm, &d) < 0) return 0;
    return d * 1000.0 / BF_AUDIO_RATE;
}

void bf_audio_close(void)
{
    if (!pcm) return;
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = NULL;
    free(acc);
    acc = NULL;
    acc_cap = acc_n = 0;
}
