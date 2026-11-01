// gpufloor's decisions on a made-up devfreq folder (BANANAFRONT_GPUFREQ_DIR) with a made-up clock
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gpufloor.h"

static double now;
double bf_now_ms(void) { return now; }
void bf_log(const char* fmt, ...) { (void)fmt; }

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s (%u MHz)\n", __FILE__, __LINE__, #c, bf_gpufloor_mhz()); fails++; } } while (0)
static char dir[] = "/tmp/bf-gpufloor-XXXXXX";
static void put(const char* f, const char* v) { char p[256]; snprintf(p, sizeof p, "%s/%s", dir, f); FILE* o = fopen(p, "w"); fputs(v, o); fclose(o); }
static unsigned long get_min(void) { char p[256], b[64] = ""; snprintf(p, sizeof p, "%s/min_freq", dir); FILE* f = fopen(p, "r"); if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); } return strtoul(b, NULL, 10); }

int main(void)
{
    if (!mkdtemp(dir)) return 1;
    setenv("BANANAFRONT_GPUFREQ_DIR", dir, 1);
    put("available_frequencies", "200000000 300000000 400000000 600000000 700000000 800000000\n");
    put("min_freq", "200000000\n");
    put("max_freq", "800000000\n");
    now = 1000;
    CHECK(bf_gpufloor_start());
    CHECK(bf_gpufloor_mhz() == 200);
    // misses a refresh: up a step each (at most one every 250 ms)
    bf_gpufloor_missed(); CHECK(bf_gpufloor_mhz() == 300); CHECK(get_min() == 300000000);
    now += 100; bf_gpufloor_missed(); CHECK(bf_gpufloor_mhz() == 300);
    now += 300; bf_gpufloor_missed(); CHECK(bf_gpufloor_mhz() == 400);
    // calm: held 30 s, then down a step
    now += 20000; bf_gpufloor_tick(); CHECK(bf_gpufloor_mhz() == 400);
    now += 15000; bf_gpufloor_tick(); CHECK(bf_gpufloor_mhz() == 300);
    // it missed again right after that step down: back up, and 300 is held off for 60 s now
    now += 2000; bf_gpufloor_missed(); CHECK(bf_gpufloor_mhz() == 400);
    now += 40000; bf_gpufloor_tick(); CHECK(bf_gpufloor_mhz() == 400);
    now += 25000; bf_gpufloor_tick(); CHECK(bf_gpufloor_mhz() == 300);
    for (int i = 0; i < 20; i++) { now += 1000; bf_gpufloor_missed(); }
    CHECK(bf_gpufloor_mhz() == 800);  // never past the max
    bf_gpufloor_stop();
    CHECK(get_min() == 200000000);    // the floor from before, back
    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    printf("gpufloor tests passed\n");
    return 0;
}
