// testcore.c: a tiny libretro core for BananaFront's PC tests. It asks the environment the questions real cores ask
// (with and without the EXPERIMENTAL bit), publishes a memory map with a known pattern, draws a counter, and prints
// what it was answered to stderr ("testcore: <question> = <answer>") so tests/host-test.sh can check them.
#include <stdio.h>
#include <string.h>

#include "../src/libretro.h"

static retro_environment_t env;
static retro_video_refresh_t video;
static retro_audio_sample_batch_t audio;
static retro_input_poll_t poll_cb;
static retro_input_state_t input;
static unsigned char ram[0x2000];
static unsigned short fb[64 * 48];
static unsigned frame;

RETRO_API void retro_set_environment(retro_environment_t e) { env = e; }
RETRO_API void retro_set_video_refresh(retro_video_refresh_t v) { video = v; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t a) { (void)a; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t a) { audio = a; }
RETRO_API void retro_set_input_poll(retro_input_poll_t p) { poll_cb = p; }
RETRO_API void retro_set_input_state(retro_input_state_t s) { input = s; }
RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) {}
RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }
RETRO_API void retro_get_system_info(struct retro_system_info* i)
{
    memset(i, 0, sizeof *i);
    i->library_name = "TestCore";
    i->library_version = "1";
    i->valid_extensions = "tst";
}
RETRO_API void retro_get_system_av_info(struct retro_system_av_info* i)
{
    memset(i, 0, sizeof *i);
    i->geometry.base_width = i->geometry.max_width = 64;
    i->geometry.base_height = i->geometry.max_height = 48;
    i->timing.fps = 60.0;
    i->timing.sample_rate = 48000;
}
RETRO_API void retro_set_controller_port_device(unsigned p, unsigned d) { (void)p; (void)d; }
RETRO_API void retro_reset(void) { frame = 0; }
RETRO_API void retro_run(void)
{
    poll_cb();
    frame++;
    ram[0] = (unsigned char)frame;
    for (int i = 0; i < 64 * 48; i++) fb[i] = (unsigned short)(i + frame);
    video(fb, 64, 48, 128);
    static short s[800 * 2];
    audio(s, 800);
}
RETRO_API size_t retro_serialize_size(void) { return sizeof ram + 4; }
RETRO_API bool retro_serialize(void* d, size_t n) { if (n < sizeof ram + 4) return false; memcpy(d, &frame, 4); memcpy((char*)d + 4, ram, sizeof ram); return true; }
RETRO_API bool retro_unserialize(const void* d, size_t n) { if (n < sizeof ram + 4) return false; memcpy(&frame, d, 4); memcpy(ram, (const char*)d + 4, sizeof ram); return true; }
RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned i, bool e, const char* c) { (void)i; (void)e; (void)c; }
RETRO_API bool retro_load_game(const struct retro_game_info* g)
{
    (void)g;
    enum retro_pixel_format f = RETRO_PIXEL_FORMAT_RGB565;
    fprintf(stderr, "testcore: pixel_format = %d\n", env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &f));
    bool b = false;
    fprintf(stderr, "testcore: input_bitmasks = %d\n", env(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, &b));
    bool ach = true;
    fprintf(stderr, "testcore: support_achievements = %d\n", env(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &ach));
    fprintf(stderr, "testcore: fastforwarding = %d\n", env(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &b));
    for (unsigned i = 0; i < sizeof ram; i++) ram[i] = (unsigned char)(i * 7);
    memcpy(ram + 0x100, "TESTCORE", 8);
    struct retro_memory_descriptor d[1] = { { 0, ram, 0, 0xC000, 0, 0, sizeof ram, "wram" } };
    struct retro_memory_map m = { d, 1 };
    fprintf(stderr, "testcore: memory_maps = %d\n", env(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &m));
    return true;
}
RETRO_API bool retro_load_game_special(unsigned t, const struct retro_game_info* g, size_t n) { (void)t; (void)g; (void)n; return false; }
RETRO_API void retro_unload_game(void) {}
RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
RETRO_API void* retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SYSTEM_RAM ? ram : NULL; }
RETRO_API size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SYSTEM_RAM ? sizeof ram : 0; }
