// cpugov.c: BananaFront's CPU governor, BananaDS's idea (its cpugov.cpp) for a frontend that runs one core frame per
// refresh: the lowest clock at which the frames still fit, set as scaling_max_freq under the "performance" governor
// (so the clock doesn't float as schedutil makes it: animations from 408 MHz), remembered per game.
// - Each frame main reports the work it took (core frames + the copy, ms) and whether it missed its refresh.
// - Down one step after STEP_DOWN_S seconds with no missed refresh, if the slowest frame of that time (p99 of the last
//   second's ~60) would still fit at the lower clock with HEADROOM to spare (work scales by the clock ratio: the A55
//   here is memory-bound, so that's pessimistic, i.e. safe).
// - Up one step at once on a missed refresh (two at once for two in a second), and that clock is banned for a while
//   that doubles each time it fails again (30 s, 60 s, ... BananaDS's escalating ban).
// - Saves, the menu: the top clock for a moment (bf_cpugov_boost): a stall that isn't the clock's fault.
// - The clock found is kept in $BANANAFRONT_DATA/cpugov/<game>.khz and the next start begins there.
// - On exit the governor, min and max from before are put back.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bf.h"
#include "cpugov.h"

#define CPU "/sys/devices/system/cpu/cpufreq/policy0"
#define MAXF 16
#define STEP_DOWN_S 5.0
#define HEADROOM 0.65  // the slowest frame at the lower clock must fit in 65 % of a refresh (0.75 let the NES stress ROM reach 14 of 16.7 ms at 600 MHz)

static struct {
    bool on;
    unsigned f[MAXF];
    int nf, cur;
    double ban_until[MAXF], ban_len[MAXF];
    char old_gov[32];
    unsigned old_min, old_max;
    char store[1100];
    double period_ms;
    double win[64];
    int nwin;
    double win_p99;
    int late_1s;
    double next_eval, last_change, last_late, boost_until;
    int boost_from;
    long changes, lates;
} G;

static bool rd(const char* f, char* out, size_t n)
{
    char p[160];
    snprintf(p, sizeof p, CPU "/%s", f);
    FILE* fp = fopen(p, "r");
    if (!fp) return false;
    bool ok = fgets(out, (int)n, fp) != NULL;
    fclose(fp);
    if (ok) out[strcspn(out, "\n")] = 0;
    return ok;
}
static void wr(const char* f, const char* v)
{
    char p[160];
    snprintf(p, sizeof p, CPU "/%s", f);
    FILE* fp = fopen(p, "w");
    if (fp) { fputs(v, fp); fclose(fp); }
}
static void wru(const char* f, unsigned v) { char b[32]; snprintf(b, sizeof b, "%u", v); wr(f, b); }

static void set_clock(int i, const char* why)
{
    if (i < 0) i = 0;
    if (i >= G.nf) i = G.nf - 1;
    if (i == G.cur) return;
    // (max first when going down, min (kept at the lowest) never above max)
    wru("scaling_max_freq", G.f[i]);
    if (G.cur >= 0 && getenv("BANANAFRONT_DEBUG")) bf_log("cpugov: %u -> %u MHz (%s)", G.f[G.cur] / 1000, G.f[i] / 1000, why);
    G.cur = i;
    G.changes++;
    G.last_change = bf_now_ms();
    G.nwin = 0;
}

bool bf_cpugov_start(const char* game, double refresh_hz)
{
    memset(&G, 0, sizeof G);
    G.cur = -1;
    G.boost_from = -1;
    char b[512];
    if (!rd("scaling_available_frequencies", b, sizeof b)) { bf_log("cpugov: no cpufreq"); return false; }
    for (char *s = NULL, *t = strtok_r(b, " ", &s); t && G.nf < MAXF; t = strtok_r(NULL, " ", &s)) G.f[G.nf++] = (unsigned)atol(t);
    if (G.nf < 2) return false;
    for (int i = 0; i < G.nf; i++) G.ban_len[i] = 30000;
    rd("scaling_governor", G.old_gov, sizeof G.old_gov);
    if (rd("scaling_min_freq", b, sizeof b)) G.old_min = (unsigned)atol(b);
    if (rd("scaling_max_freq", b, sizeof b)) G.old_max = (unsigned)atol(b);
    G.period_ms = 1000.0 / (refresh_hz > 1 ? refresh_hz : 60.0);
    const char* data = getenv("BANANAFRONT_DATA");
    snprintf(G.store, sizeof G.store, "%s/cpugov/%s.khz", data && *data ? data : "/data/bananafront", game);
    // start where this game was last time; else at the top (stepping down is cheap, a late start isn't)
    int start = G.nf - 1;
    FILE* f = fopen(G.store, "r");
    if (f) {
        unsigned k = 0;
        if (fscanf(f, "%u", &k) == 1)
            for (int i = 0; i < G.nf; i++) if (G.f[i] == k) start = i;
        fclose(f);
    }
    wru("scaling_min_freq", G.f[0]);
    wr("scaling_governor", "performance");
    set_clock(start, "start");
    G.on = true;
    G.next_eval = bf_now_ms() + 1000;
    bf_log("cpugov: on, %u MHz to start (%s), %d clocks %u-%u MHz", G.f[G.cur] / 1000, f ? "this game's" : "the top",
           G.nf, G.f[0] / 1000, G.f[G.nf - 1] / 1000);
    return true;
}

static int cmpd(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }

void bf_cpugov_frame(double work_ms, bool late)
{
    if (!G.on) return;
    if (G.nwin < 64) G.win[G.nwin++] = work_ms;
    if (late && bf_now_ms() >= G.boost_until && bf_now_ms() - G.last_change > 300) {
        // a missed refresh at this clock: up now, this clock banned for a while (longer each time)
        const int bad = G.cur;
        G.ban_until[bad] = bf_now_ms() + G.ban_len[bad];
        G.ban_len[bad] *= 2;
        G.lates++;
        G.late_1s++;
        G.last_late = bf_now_ms();
        set_clock(G.cur + (G.late_1s >= 2 ? 2 : 1), "late frame");
    }
}

void bf_cpugov_tick(void)
{
    if (!G.on) return;
    const double t = bf_now_ms();
    if (G.boost_from >= 0 && t >= G.boost_until) { set_clock(G.boost_from, "boost over"); G.boost_from = -1; }
    if (t < G.next_eval) return;
    G.next_eval = t + 1000;
    G.late_1s = 0;
    if (G.nwin < 20 || G.boost_from >= 0) { G.nwin = 0; return; }
    qsort(G.win, G.nwin, sizeof *G.win, cmpd);
    G.win_p99 = G.win[(int)(G.nwin * 0.99)];
    G.nwin = 0;
    if (G.cur <= 0) return;
    if (t - G.last_change < STEP_DOWN_S * 1000 || t - G.last_late < STEP_DOWN_S * 1000) return;
    const int dn = G.cur - 1;
    if (t < G.ban_until[dn]) return;
    const double at_lower = G.win_p99 * G.f[G.cur] / G.f[dn];
    if (at_lower < G.period_ms * HEADROOM) set_clock(dn, "headroom");
}

void bf_cpugov_boost(int ms)
{
    if (!G.on) return;
    if (G.boost_from < 0) G.boost_from = G.cur;
    G.boost_until = bf_now_ms() + ms;
    set_clock(G.nf - 1, "boost");
}

unsigned bf_cpugov_mhz(void) { return G.on && G.cur >= 0 ? G.f[G.cur] / 1000 : 0; }

void bf_cpugov_stop(void)
{
    if (!G.on) return;
    if (G.boost_from >= 0) G.cur = G.boost_from;
    // remember this game's clock (the one it settled at), for the next start
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", G.store);
    char* sl = strrchr(dir, '/');
    if (sl) { *sl = 0; bf_mkdirs(dir); }
    char b[32];
    int n = snprintf(b, sizeof b, "%u\n", G.f[G.cur]);
    bf_write_file(G.store, b, (size_t)n);
    bf_log("cpugov: ended at %u MHz, %ld changes, %ld late-frame steps up", G.f[G.cur] / 1000, G.changes, G.lates);
    if (G.old_gov[0]) wr("scaling_governor", G.old_gov);
    if (G.old_max) wru("scaling_max_freq", G.old_max);
    if (G.old_min) wru("scaling_min_freq", G.old_min);
    G.on = false;
}
