// resample.c: the core's sound (its own rate: 32768 Hz GB/GBA, 32040 SNES, 44100/48000 others) to the amps' 48 kHz,
// with RetroArch-style dynamic rate control: the frontend runs one core frame per panel refresh (60.005 Hz) whatever
// the core's frame rate (59.73 GB/GBA, 60.10 NES/SNES), so the input rate is scaled by refresh/fps and then nudged by
// at most +-0.5 % by how full the ALSA buffer is. Cubic (Catmull-Rom) interpolation: 4 taps, no tables, a few
// multiply-adds per output sample (the A55's cost here is noise next to a frame).
#include <math.h>
#include <string.h>

#include "bf.h"

void bf_resampler_reset(bf_resampler* r) { memset(r, 0, sizeof *r); }

static inline int16_t clamp16(float v)
{
    if (v > 32767.f) return 32767;
    if (v < -32768.f) return -32768;
    return (int16_t)lrintf(v);
}

size_t bf_resample(bf_resampler* r, const int16_t* in, size_t in_frames, int16_t* out, size_t out_max, double ratio)
{
    if (ratio <= 0) return 0;
    const double step = 1.0 / ratio;  // input frames per output frame
    size_t o = 0, i = 0;
    // hist[0..3] = x[-2], x[-1], x[0], x[1]: the output point lies between x[0] and x[1] at pos (0 <= pos < 1)
    if (!r->primed && in_frames) {
        for (int k = 0; k < 4; k++) { r->hist[k][0] = in[0]; r->hist[k][1] = in[1]; }
        r->primed = 1;
        r->pos = 0;
    }
    for (;;) {
        while (r->pos >= 1.0) {
            if (i >= in_frames) return o;
            memmove(r->hist[0], r->hist[1], sizeof r->hist[0] * 3);
            r->hist[3][0] = in[i * 2];
            r->hist[3][1] = in[i * 2 + 1];
            i++;
            r->pos -= 1.0;
        }
        if (o >= out_max) {
            // no room: the rest of the input is dropped (the caller sized out for the ratio, so only rounding)
            return o;
        }
        const float t = (float)r->pos, t2 = t * t, t3 = t2 * t;
        // Catmull-Rom between hist[1] and hist[2]
        for (int c = 0; c < 2; c++) {
            const float p0 = r->hist[0][c], p1 = r->hist[1][c], p2 = r->hist[2][c], p3 = r->hist[3][c];
            const float v = 0.5f * (2.f * p1 + (-p0 + p2) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t2 +
                                    (-p0 + 3.f * p1 - 3.f * p2 + p3) * t3);
            out[o * 2 + c] = clamp16(v);
        }
        o++;
        r->pos += step;
    }
}

double bf_drc_ratio(double base_ratio, double fill, double max_delta)
{
    if (fill < 0) fill = 0;
    if (fill > 1) fill = 1;
    // fill below half: produce a little more output per input (ratio up), above: a little less
    const double dir = (0.5 - fill) * 2.0;  // +1 empty .. -1 full
    return base_ratio * (1.0 + max_delta * dir);
}
