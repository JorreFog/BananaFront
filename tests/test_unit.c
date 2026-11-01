// PC tests of the logic that decides whether today's files keep working: paths, the state format, the resampler.
//   make test   (BF_RA_STATE=<a RetroArch .state> also loads a real one)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/bf.h"
#include "../src/memmap.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKS(a, b) do { if (strcmp((a), (b))) { fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); fails++; } } while (0)

static void test_paths(void)
{
    char b[1200];
    bf_game_base("/data/roms/gba/Pokemon Emerald Version.gba", b, sizeof b);
    CHECKS(b, "Pokemon Emerald Version");
    bf_game_base("/data/roms/gb/Tetris (World) (Rev 1).zip", b, sizeof b);
    CHECKS(b, "Tetris (World) (Rev 1)");
    bf_game_base("/data/roms/nes/v1.2.final.nes", b, sizeof b);
    CHECKS(b, "v1.2.final");
    bf_game_base("noext", b, sizeof b);
    CHECKS(b, "noext");

    bf_paths p;
    bf_paths_init(&p, "/data/roms/snes/EarthBound (USA).sfc", "Snes9x", "/data/saves", "/data/states",
                  "/data/screenshots", "/data/bios", NULL);
    bf_path_save(&p, ".srm", b, sizeof b);
    CHECKS(b, "/data/saves/Snes9x/EarthBound (USA).srm");
    bf_path_state(&p, 0, b, sizeof b);
    CHECKS(b, "/data/states/Snes9x/EarthBound (USA).state");
    bf_path_state(&p, 3, b, sizeof b);
    CHECKS(b, "/data/states/Snes9x/EarthBound (USA).state3");
    bf_path_state(&p, -1, b, sizeof b);
    CHECKS(b, "/data/states/Snes9x/EarthBound (USA).state.auto");
    // the GBA's shared save folder (run-retroarch), states still per core
    bf_paths_init(&p, "/data/roms/gba/Aladdin.gba", "gpSP", "/data/saves", "/data/states", "/data/screenshots",
                  "/data/bios", "/data/saves/gba");
    bf_path_save(&p, ".srm", b, sizeof b);
    CHECKS(b, "/data/saves/gba/Aladdin.srm");
    bf_path_state(&p, 0, b, sizeof b);
    CHECKS(b, "/data/states/gpSP/Aladdin.state");
}

static void test_conf(void)
{
    const char* f = "/tmp/bf-test-conf.cfg";
    FILE* o = fopen(f, "w");
    fprintf(o, "# c\nvideo_monitor_index = \"2\"\nstate_slot = \"0\"\n ra_shader=lcd \nstate_slot = \"4\"\n[x]\n");
    fclose(o);
    bf_conf c = { 0 };
    CHECK(bf_conf_load(&c, f));
    CHECKS(bf_conf_get(&c, "video_monitor_index"), "2");
    CHECKS(bf_conf_get(&c, "state_slot"), "4");
    CHECKS(bf_conf_get(&c, "ra_shader"), "lcd");
    CHECK(bf_conf_get(&c, "nope") == NULL);
    char v[64];
    CHECK(bf_conf_file_get(f, "state_slot", v, sizeof v));
    CHECKS(v, "4");
    bf_conf_free(&c);
    remove(f);
}

static void test_state(void)
{
    // round trip, both forms, sizes that need padding and that span several RZIP chunks
    size_t sizes[] = { 1, 13, 4096, 300001 };
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        size_t n = sizes[k];
        unsigned char* d = malloc(n);
        for (size_t i = 0; i < n; i++) d[i] = (unsigned char)(i * 7 + (i >> 9));
        for (int z = 0; z < 2; z++) {
            size_t fs = 0, cs = 0;
            void* f = bf_state_pack(d, n, z, &fs);
            CHECK(f != NULL);
            void* c = bf_state_unpack(f, fs, &cs);
            CHECK(c != NULL && cs == n && !memcmp(c, d, n));
            if (z) CHECK(!memcmp(f, "#RZIPv\1#", 8));
            else CHECK(!memcmp(f, "RASTATE\1", 8) && fs % 8 == 0);
            free(f);
            free(c);
        }
        // a raw state (older RetroArch) is the core's data as is
        size_t cs = 0;
        void* c = bf_state_unpack(d, n, &cs);
        CHECK(c && cs == n && !memcmp(c, d, n));
        free(c);
        free(d);
    }
    // a real RetroArch state, when given
    const char* ra = getenv("BF_RA_STATE");
    if (ra) {
        size_t fs = 0, cs = 0;
        void* f = bf_read_file(ra, &fs);
        CHECK(f != NULL);
        void* c = f ? bf_state_unpack(f, fs, &cs) : NULL;
        CHECK(c != NULL && cs > 1000);
        printf("real state %s: %zu bytes -> core data %zu bytes\n", ra, fs, cs);
        // re-packed the way we write it, then read back the same
        size_t fs2 = 0, cs2 = 0;
        void* f2 = c ? bf_state_pack(c, cs, true, &fs2) : NULL;
        void* c2 = f2 ? bf_state_unpack(f2, fs2, &cs2) : NULL;
        CHECK(c2 && cs2 == cs && !memcmp(c, c2, cs));
        free(f); free(c); free(f2); free(c2);
    }
}

static void test_resample(void)
{
    // a 1 kHz sine at 32768 Hz to 48000: the output has the input's frequency and amplitude, no clicks between calls
    const int in_rate = 32768, out_rate = 48000, secs = 1;
    const size_t in_n = (size_t)in_rate * secs;
    int16_t* in = malloc(in_n * 4);
    for (size_t i = 0; i < in_n; i++) in[i * 2] = in[i * 2 + 1] = (int16_t)lrint(16000 * sin(2 * M_PI * 1000 * i / in_rate));
    int16_t* out = malloc((size_t)out_rate * secs * 4 + 4096 * 4);
    bf_resampler r;
    bf_resampler_reset(&r);
    size_t o = 0;
    const double ratio = (double)out_rate / in_rate;
    for (size_t i = 0; i < in_n; i += 547) {  // odd chunks: the history must carry across calls
        size_t n = in_n - i < 547 ? in_n - i : 547;
        o += bf_resample(&r, in + i * 2, n, out + o * 2, 4096, ratio);
    }
    CHECK(o > (size_t)out_rate * secs - 8 && o < (size_t)out_rate * secs + 8);
    // compare against the ideal sine (delayed by the 3 input samples the history holds)
    double err = 0, peak = 0;
    for (size_t k = 100; k < o - 100; k++) {
        double t = (double)k / ratio - 3.0;  // input sample position (the history holds 3 behind)
        double ideal = 16000 * sin(2 * M_PI * 1000 * t / in_rate);
        double e = fabs(out[k * 2] - ideal);
        if (e > err) err = e;
        if (abs(out[k * 2]) > peak) peak = abs(out[k * 2]);
    }
    printf("resample 32768->48000 1 kHz: %zu frames, max error %.1f / 16000, peak %.0f\n", o, err, peak);
    CHECK(err < 250);  // cubic at ~1/33 of the input rate: well under 2 %
    CHECK(peak > 15800 && peak <= 16001);
    // DRC: half full keeps the ratio, empty raises it by the delta, full lowers it
    CHECK(fabs(bf_drc_ratio(1.5, 0.5, 0.005) - 1.5) < 1e-12);
    CHECK(fabs(bf_drc_ratio(1.0, 0.0, 0.005) - 1.005) < 1e-12);
    CHECK(fabs(bf_drc_ratio(1.0, 1.0, 0.005) - 0.995) < 1e-12);
    free(in);
    free(out);
}

static void test_memmap(void)
{
    // gpSP's two descriptors (select 0, power-of-two lengths): EWRAM at 0x2000000, IWRAM at 0x3000000
    static uint8_t iw[0x8000], ew[0x40000];
    struct retro_memory_descriptor d[2] = {
        { 0, iw, 0, 0x3000000, 0, 0, 0x8000, NULL },
        { 0, ew, 0, 0x2000000, 0, 0, 0x40000, NULL } };
    CHECK(bf_mmap_preprocess(d, 2));
    size_t off = 0;
    const struct retro_memory_descriptor* f = bf_mmap_find(d, 2, 0x2000010, &off);
    CHECK(f == &d[1] && off == 0x10);
    f = bf_mmap_find(d, 2, 0x3007FFF, &off);
    CHECK(f == &d[0] && off == 0x7FFF);
    // no mirror: RetroArch's select from a power-of-two length covers that length only (0x3008004 isn't IWRAM's)
    CHECK(bf_mmap_find(d, 2, 0x3008004, &off) == NULL);
    CHECK(bf_mmap_find(d, 2, 0x4000000, &off) == NULL);
    // a length that isn't a power of two with no select: preprocessing stops (as RetroArch's), still found by range
    struct retro_memory_descriptor e[1] = { { 0, ew, 0, 0x6000, 0, 0, 0x1800, NULL } };
    CHECK(!bf_mmap_preprocess(e, 1));
    f = bf_mmap_find(e, 1, 0x6100, &off);
    CHECK(f == &e[0] && off == 0x100);
}

int main(void)
{
    test_memmap();
    test_paths();
    test_conf();
    test_state();
    test_resample();
    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    printf("all unit tests passed\n");
    return 0;
}
