// video_null.c: no panel (PC tests: saves, states, commands, the companion, frame checksums). Paced by a 60 Hz
// timerfd that stands in for the flip event. Never for performance numbers (BananaOS rule: no headless numbers).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "bf.h"
#include "video.h"

static int tfd = -1;
static bool pending;
static uint8_t* buf;
static size_t cap;
static unsigned W, H;
static int bpp;
static int pitch_;
static uint64_t hash = 1469598103934665603ull, frames;
static double last_flip;

bool bf_video_open(const bf_video_opts* o)
{
    (void)o;
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    // BANANAFRONT_NULL_HZ=0: as fast as it goes (correctness runs)
    const char* hz = getenv("BANANAFRONT_NULL_HZ");
    double f = hz ? atof(hz) : 60.0;
    struct itimerspec t = { 0 };
    long ns = f > 0 ? (long)(1e9 / f) : 100000;
    t.it_interval.tv_nsec = t.it_value.tv_nsec = ns;
    timerfd_settime(tfd, 0, &t, NULL);
    bf_log("video: none (null driver, %.0f Hz)", f);
    return true;
}

void* bf_video_begin(unsigned w, unsigned h, int fmt, float aspect, int* pitch)
{
    (void)aspect;
    bpp = fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
    W = w; H = h;
    pitch_ = (int)(w * bpp);
    if ((size_t)pitch_ * h > cap) { cap = (size_t)pitch_ * h; buf = realloc(buf, cap); }
    *pitch = pitch_;
    return buf;
}

bool bf_video_commit(bool dupe)
{
    if (pending) return false;
    if (!dupe && buf) {
        // FNV-1a over the visible picture: two builds that draw the same give the same hash
        for (size_t i = 0; i < (size_t)pitch_ * H; i++) { hash ^= buf[i]; hash *= 1099511628211ull; }
    }
    frames++;
    pending = true;
    return true;
}

bool bf_video_flip_pending(void) { return pending; }
int bf_video_fd(void) { return tfd; }
void bf_video_events(void)
{
    uint64_t n;
    if (read(tfd, &n, sizeof n) > 0) { pending = false; last_flip = bf_now_ms(); }
}
double bf_video_refresh(void) { return 60.0; }
int bf_video_size(int* w, int* h) { *w = 640; *h = 480; return 0; }
uint32_t* bf_video_grab(int* w, int* h) { (void)w; (void)h; return NULL; }
double bf_video_last_flip_ms(void) { return last_flip; }
void bf_video_close(void)
{
    bf_log("video: %llu frames shown, picture hash %016llx", (unsigned long long)frames, (unsigned long long)hash);
    if (tfd >= 0) close(tfd);
    free(buf);
}

static uint32_t* cv[2];
int bf_video_canvas(uint32_t** px, int* pitch, int* w, int* h)
{
    static int k;
    k ^= 1;
    if (!cv[k]) cv[k] = calloc(640 * 480, 4);
    *px = cv[k]; *pitch = 640 * 4; *w = 640; *h = 480;
    return k;
}
void bf_video_canvas_show(int k)
{
    // (tests: the menu's picture written out when asked)
    const char* d = getenv("BANANAFRONT_NULL_CANVAS");
    if (!d) return;
    FILE* f = fopen(d, "wb");
    if (!f) return;
    fprintf(f, "P6 640 480 255\n");
    for (int i = 0; i < 640 * 480; i++) { uint32_t p = cv[k][i]; fputc(p >> 16, f); fputc(p >> 8, f); fputc(p, f); }
    fclose(f);
}
void bf_video_canvas_done(void) {}
bool bf_video_shader(const char* name) { (void)name; return false; }
bool bf_video_wants_xrgb(void) { return false; }
uint32_t* bf_video_overlay(int rows, int* pitch, int* w) { (void)rows; (void)pitch; (void)w; return NULL; }
void bf_video_overlay_show(bool on) { (void)on; }
bool bf_video_has_overlay(void) { return false; }
void bf_video_fixed_rect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void bf_video_shader_off(void) {}
const char* bf_video_shader_name(void) { return ""; }
static enum bf_scale sc;
void bf_video_set_scale(enum bf_scale s) { sc = s; }
enum bf_scale bf_video_get_scale(void) { return sc; }
