// cpugov's decisions on a made-up cpufreq folder (BANANAFRONT_CPUFREQ_DIR) with a made-up clock (bf_now_ms below)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/cpugov.h"

static double now;
double bf_now_ms(void) { return now; }
void bf_log(const char* fmt, ...) { (void)fmt; }
bool bf_mkdirs(const char* d) { (void)d; return true; }
bool bf_write_file(const char* p, const void* d, size_t n) { (void)p; (void)d; (void)n; return true; }

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (clock %u)\n", __FILE__, __LINE__, #c, bf_cpugov_mhz()); fails++; } } while (0)

static void put(const char* dir, const char* f, const char* v)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, f);
    FILE* o = fopen(p, "w");
    fputs(v, o);
    fclose(o);
}

// a second of frames: each one costs work_at_1992 * 1992 / clock (the A55 here scales about that way, or better)
static void second(double work_at_1992)
{
    for (int i = 0; i < 60; i++) {
        now += 1000.0 / 60;
        const double w = work_at_1992 * 1992.0 / bf_cpugov_mhz();
        bf_cpugov_frame(w, w > 1000.0 / 60);
        bf_cpugov_tick();
    }
}

int main(void)
{
    char dir[] = "/tmp/bf-cpugov-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    setenv("BANANAFRONT_CPUFREQ_DIR", dir, 1);
    setenv("BANANAFRONT_DATA", dir, 1);
    put(dir, "scaling_available_frequencies", "408000 600000 816000 1104000 1416000 1608000 1800000 1992000\n");
    put(dir, "scaling_governor", "schedutil\n");
    put(dir, "scaling_min_freq", "408000\n");
    put(dir, "scaling_max_freq", "1992000\n");
    now = 1000;
    CHECK(bf_cpugov_start("game", 60.0));
    CHECK(bf_cpugov_mhz() == 1992);
    // a light game (2 ms at the top): steps down every 5 s to the lowest clock that keeps 65 % headroom
    for (int s = 0; s < 60; s++) second(2.0);
    CHECK(bf_cpugov_mhz() == 408);     // 2 * 1992/408 = 9.8 ms < 10.8 ms
    // it gets heavy (5 ms at the top = 24 ms at 408): late frames push it up at once, two steps for two in a second
    second(5.0);
    CHECK(bf_cpugov_mhz() >= 816);
    for (int s = 0; s < 30; s++) second(5.0);
    const unsigned settled = bf_cpugov_mhz();
    CHECK(settled >= 816 && settled <= 1608);  // 5 * 1992 / settled must fit a refresh
    // a boost (a save): the top for its time, then back where it was
    bf_cpugov_boost(1000);
    CHECK(bf_cpugov_mhz() == 1992);
    second(5.0);
    second(5.0);
    CHECK(bf_cpugov_mhz() == settled);
    bf_cpugov_stop();
    char gov[64] = "";
    char p[512];
    snprintf(p, sizeof p, "%s/scaling_governor", dir);
    FILE* f = fopen(p, "r");
    if (f) { if (!fgets(gov, sizeof gov, f)) gov[0] = 0; fclose(f); }
    CHECK(!strncmp(gov, "schedutil", 9));  // the governor from before, back
    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    printf("cpugov tests passed (settled at %u MHz for 5 ms of work at 1992)\n", settled);
    return 0;
}
