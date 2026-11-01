// gpufloor.c: a floor under the GPU's clock while a shader runs. The profile's simple_ondemand kept the Mali-G52 at its
// lowest clock (200 MHz) through a shader pass: fine at the RG DS's 640x480, but at the RG DS Plus's 1024x768 the
// pass (2.56x the pixels) plus BananaGBA's frame missed the refresh's latch - the flip came a refresh late with the
// frame itself on time (owner, 0.5.1: "missed 19, caught-up 19" in 10 s, vblank 58.3 Hz). The CPU floor can't help
// there. So: devfreq's min_freq up a step at each such miss, held 30 s (doubled for a clock that missed again within a minute of a step down to it),
// then down a step after 30 s with none; put back at exit. Only with a shader; never above the GPU's max.
#define _GNU_SOURCE
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bf.h"
#include "gpufloor.h"

#define MAXF 16
static struct {
    bool on;
    char dir[256];
    unsigned long f[MAXF], old_min;
    int nf, cur;
    double last_up, last_miss, hold_ms[MAXF], down_at;
    int down_to;
    long ups, misses;
} G;

static bool rd(const char* f, char* out, size_t n)
{
    char p[300];
    snprintf(p, sizeof p, "%s/%s", G.dir, f);
    FILE* fp = fopen(p, "r");
    if (!fp) return false;
    bool ok = fgets(out, (int)n, fp) != NULL;
    fclose(fp);
    return ok;
}
static void wr_min(unsigned long v)
{
    char p[300];
    snprintf(p, sizeof p, "%s/min_freq", G.dir);
    FILE* fp = fopen(p, "w");
    if (fp) { fprintf(fp, "%lu", v); fclose(fp); }
}

bool bf_gpufloor_start(void)
{
    memset(&G, 0, sizeof G);
    const char* d = getenv("BANANAFRONT_GPUFREQ_DIR");  // (tests)
    if (d && *d) snprintf(G.dir, sizeof G.dir, "%s", d);
    else {
        glob_t g;
        if (glob("/sys/class/devfreq/*.gpu", 0, NULL, &g) != 0 || !g.gl_pathc) { globfree(&g); return false; }
        snprintf(G.dir, sizeof G.dir, "%s", g.gl_pathv[0]);
        globfree(&g);
    }
    char b[512];
    if (!rd("available_frequencies", b, sizeof b)) return false;
    for (char *s = NULL, *t = strtok_r(b, " \n", &s); t && G.nf < MAXF; t = strtok_r(NULL, " \n", &s)) G.f[G.nf++] = strtoul(t, NULL, 10);
    if (G.nf < 2) return false;
    if (rd("min_freq", b, sizeof b)) G.old_min = strtoul(b, NULL, 10);
    unsigned long mx = G.f[G.nf - 1];
    if (rd("max_freq", b, sizeof b)) mx = strtoul(b, NULL, 10);
    while (G.nf > 1 && G.f[G.nf - 1] > mx) G.nf--;
    G.cur = 0;
    for (int i = 0; i < G.nf; i++) { if (G.f[i] <= G.old_min) G.cur = i; G.hold_ms[i] = 30000; }
    G.on = true;
    bf_log("gpufloor: on (a shader runs), %lu MHz to start, up to %lu", G.f[G.cur] / 1000000, G.f[G.nf - 1] / 1000000);
    return true;
}

// a flip a refresh late with the frame on time: the GPU's pass was too slow
void bf_gpufloor_missed(void)
{
    if (!G.on) return;
    const double t = bf_now_ms();
    G.misses++;
    // (a step down that missed again within a minute: that clock is held off twice as long next time)
    if (G.down_at > 0 && G.cur == G.down_to && t - G.down_at < 60000) { G.hold_ms[G.cur] *= 2; G.down_at = 0; }
    G.last_miss = t;
    if (G.cur + 1 < G.nf && t - G.last_up > 250) {
        G.cur++;
        G.last_up = t;
        G.ups++;
        wr_min(G.f[G.cur]);
        if (getenv("BANANAFRONT_DEBUG")) bf_log("gpufloor: up to %lu MHz", G.f[G.cur] / 1000000);
    }
}

void bf_gpufloor_tick(void)
{
    if (!G.on || G.cur == 0) return;
    const double t = bf_now_ms();
    const double hold = G.hold_ms[G.cur - 1] > 30000 ? G.hold_ms[G.cur - 1] : 30000;
    if (t - G.last_miss > hold && t - G.last_up > hold) {
        G.cur--;
        G.last_up = t;  // (the next step down waits again)
        G.down_at = t;
        G.down_to = G.cur;
        wr_min(G.f[G.cur]);
        if (getenv("BANANAFRONT_DEBUG")) bf_log("gpufloor: down to %lu MHz", G.f[G.cur] / 1000000);
    }
}

unsigned bf_gpufloor_mhz(void) { return G.on ? (unsigned)(G.f[G.cur] / 1000000) : 0; }

void bf_gpufloor_stop(void)
{
    if (!G.on) return;
    bf_log("gpufloor: ended at %lu MHz, %ld missed refreshes, %ld steps up", G.f[G.cur] / 1000000, G.misses, G.ups);
    wr_min(G.old_min ? G.old_min : G.f[0]);
    G.on = false;
}
