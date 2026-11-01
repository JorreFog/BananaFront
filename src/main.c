// main.c: BananaFront's loop. One core frame per panel refresh: the flip of the last frame done -> input -> retro_run
// -> the picture into a scanout buffer -> one atomic commit -> its sound out at a rate tied to the display. Saves,
// states, the command pipe (banana-ui's game panel: RetroArch's words), hotkeys, signals.
//
//   bananafront [options] <core.so> <content>
//     --system <id>         BananaOS system id (gba, gb, snes...), for the companion and the GBA's shared saves
//     --saves <dir>         /data/saves      (saves go in <dir>/<core's library_name>, as RetroArch sorts them)
//     --save-dir <dir>      a fixed save folder instead (run-retroarch's /data/saves/gba for every GBA core)
//     --states <dir>        /data/states
//     --shots <dir>         /data/screenshots
//     --bios <dir>          /data/bios
//     --config <dir>        RetroArch's config folder (core options: <dir>/<lib>/<lib>.opt), read only
//     --screen top|bottom   without a lease: the panel (RetroArch's video_monitor_index 2 / 1)
//     --scale fit|integer|stretch
//     --audio-device <pcm> --audio-latency <ms>
//     --resume-load         load the automatic state (.state.auto) at the start
//     --resume-save         write it when the game quits
//     --frames <n>          quit after n frames (benchmarks)
//     --stats <file>        a line of numbers every 10 s, and a summary at the end
//     --shader <name>       a GPU shader pass (<name>.frag in DSFLIP_SHADER_DIR: zfast-lcd, zfast-crt, sharp-bilinear...)
//     --lowlat              each frame started as late as it can be (fresher input): the frame delay found by itself
//     --border <png> --border-rect x,y,w,h   the screen border (panel-sized PNG with a hole) and the picture's place
//     --cpugov              BananaFront's own clock governor (cpugov.c) instead of the system's profile
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "audio.h"
#include "cpugov.h"
#include "bf.h"
#include "frontend.h"
#include "input.h"
#include "memmap.h"
#include "ra.h"

__attribute__((weak)) bool bf_menu_run_pending(void) { return false; }
#include "video.h"

extern const char* bf_env_system_dir;
extern const char* bf_env_save_dir;
extern bool bf_env_fastforward;

static struct {
    const char* core;
    const char* rom;
    const char* system;
    const char* saves;
    const char* save_dir;
    const char* states;
    const char* shots;
    const char* bios;
    const char* config;
    const char* audio_dev;
    int audio_ms;
    int screen;
    enum bf_scale scale;
    bool resume_load, resume_save, cpugov, lowlat;
    const char* shader;
    const char* border;
    int brect[4];
    long frames;
    const char* stats;
} A = { .saves = "/data/saves", .states = "/data/states", .shots = "/data/screenshots", .bios = "/data/bios",
        .config = "/data/retroarch/config", .audio_dev = "default", .audio_ms = 64, .screen = 0 };

bf_paths g_paths;
static volatile sig_atomic_t sig_quit, sig_state;
static bool quit;
static bool ff, ff_hold, paused;
static int slot;
static uint32_t job_mask;
static char jobs[16][12];   // RETRO_DEVICE_ID_JOYPAD_* -> job name (PR #151's ra_btn_*)
static uint64_t frames_run;
uint64_t bf_frames_run(void) { return frames_run; }

// the core's last picture (its own buffer, valid until the next retro_run): thumbnails and screenshots
static struct { const void* data; unsigned w, h; size_t pitch; } last;
static bool frame_new, frame_dupe;

// ---- stats -------------------------------------------------------------------------------------------------------
#define NSAMP 1200
static struct {
    double run_ms[NSAMP];
    int n;
    uint64_t frames, vblanks, dupes, late;
    double t0, cpu0;
    double flip_prev, flip_sum;
    uint64_t flip_n;
    double lat_sum;
    uint64_t lat_n;
    double copy_ms, commit_ms;  // (the parts of fe_ms: the picture's copy, and frontend_frame + the commit)
    double fe_ms;  // BananaFront's own time (the picture's copy, the sound, the commit, the overlay's wake-up)
} S;

static double cpu_ms(void)
{
    struct rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec * 1e3 + u.ru_utime.tv_usec / 1e3 + u.ru_stime.tv_sec * 1e3 + u.ru_stime.tv_usec / 1e3;
}

static int cmpd(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }

static void stats_line(bool final)
{
    const double now = bf_now_ms(), wall = now - S.t0, cpu = cpu_ms() - S.cpu0;
    if (wall < 1) return;
    double sorted[NSAMP];
    const int n = S.n < NSAMP ? S.n : NSAMP;
    memcpy(sorted, S.run_ms, sizeof(double) * n);
    qsort(sorted, n, sizeof(double), cmpd);
    double avg = 0;
    for (int i = 0; i < n; i++) avg += sorted[i];
    avg = n ? avg / n : 0;
    bf_audio_stats as;
    bf_audio_get_stats(&as, true);
    const double aud = bf_audio_ms_take();
    char b[512];
    snprintf(b, sizeof b,
             "%s fps %.2f vblank %.3f Hz run %.2f/%.2f/%.2f ms (avg/p50/p99) cpu %.1f%% dupes %llu late %llu "
             "audio underruns %u fill %.2f ratio %.5f/%.5f dropped %zu clock %u input-age %.1f ms frontend %.3f ms/frame "
             "(copy %.3f audio %.3f commit %.3f)",
             final ? "END" : "stats", S.frames * 1000.0 / wall, S.flip_n ? 1000.0 / (S.flip_sum / S.flip_n) : 0, avg,
             n ? sorted[n / 2] : 0, n ? sorted[(int)(n * 0.99)] : 0, cpu * 100.0 / wall, (unsigned long long)S.dupes,
             (unsigned long long)S.late, as.underruns, as.fill_n ? as.fill_sum / as.fill_n : 0, as.ratio, as.base_ratio,
             as.dropped, bf_cpugov_mhz(), S.lat_n ? S.lat_sum / S.lat_n : 0, S.frames ? (S.fe_ms + aud) / S.frames : 0,
             S.frames ? S.copy_ms / S.frames : 0, S.frames ? aud / S.frames : 0, S.frames ? S.commit_ms / S.frames : 0);
    bf_log("%s", b);
    if (A.stats) {
        FILE* f = fopen(A.stats, "a");
        if (f) { fprintf(f, "%.0f %s\n", now, b); fclose(f); }
    }
    S.n = 0;
    S.frames = S.vblanks = S.dupes = S.late = 0;
    S.flip_sum = 0;
    S.flip_n = 0;
    S.lat_sum = 0;
    S.lat_n = 0;
    S.fe_ms = S.copy_ms = S.commit_ms = 0;
    S.t0 = now;
    S.cpu0 = cpu_ms();
}

// ---- the picture -----------------------------------------------------------------------------------------------

// A core that asks for it (GET_CURRENT_SOFTWARE_FRAMEBUFFER: FCEUmm/BananaNES, of tonight's) draws straight into
// the scanout buffer, so the frame needs no copy: BananaNES on the RG DS 1.49 -> 1.34 ms/frame (nes, 03:40). The
// buffer is uncached (write-combined) memory - a core reading back what it drew would get slower, not wrong -
// so BANANAFRONT_DIRECT=0 turns it off for an A/B.
static void* direct_buf;
static int direct_on = -1;
bool bf_direct_framebuffer(struct retro_framebuffer* fb)
{
    if (direct_on < 0) direct_on = !getenv("BANANAFRONT_DIRECT") || atoi(getenv("BANANAFRONT_DIRECT"));
    if (!direct_on || bf_video_wants_xrgb() || g_core.fmt == RETRO_PIXEL_FORMAT_0RGB1555) return false;
    int pitch = 0;
    void* p = bf_video_begin(fb->width, fb->height, g_core.fmt, g_core.av.geometry.aspect_ratio, &pitch);
    if (!p) return false;
    fb->data = p;
    fb->pitch = (size_t)pitch;
    fb->format = g_core.fmt;
    fb->memory_flags = 0;  // (uncached)
    direct_buf = p;
    return true;
}

static void video_frame_(const void* data, unsigned w, unsigned h, size_t pitch);
void bf_video_frame(const void* data, unsigned w, unsigned h, size_t pitch)
{
    const double t = bf_now_ms();
    video_frame_(data, w, h, pitch);
    S.fe_ms += bf_now_ms() - t;
    S.copy_ms += bf_now_ms() - t;
}
static void video_frame_(const void* data, unsigned w, unsigned h, size_t pitch)
{
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) { frame_dupe = true; return; }
    if (data == direct_buf) {  // drawn in place: nothing to copy
        last.data = data; last.w = w; last.h = h; last.pitch = pitch;
        direct_buf = NULL;
        frame_new = true;
        return;
    }
    last.data = data; last.w = w; last.h = h; last.pitch = pitch;
    const float aspect = g_core.av.geometry.aspect_ratio > 0 ? g_core.av.geometry.aspect_ratio : 0;
    int dp = 0;
    const bool x8 = g_core.fmt == RETRO_PIXEL_FORMAT_XRGB8888 || bf_video_wants_xrgb();
    const int fmt = x8 ? RETRO_PIXEL_FORMAT_XRGB8888 : RETRO_PIXEL_FORMAT_RGB565;
    uint8_t* dst = bf_video_begin(w, h, fmt, aspect, &dp);
    if (!dst) return;
    const uint8_t* src = data;
    if (x8 && g_core.fmt != RETRO_PIXEL_FORMAT_XRGB8888) {
        // 16-bit core picture into the shader's 32-bit input
        const bool p565 = g_core.fmt == RETRO_PIXEL_FORMAT_RGB565;
        for (unsigned y = 0; y < h; y++) {
            const uint16_t* s = (const uint16_t*)(src + y * pitch);
            uint32_t* d = (uint32_t*)(dst + (size_t)y * dp);
            for (unsigned x = 0; x < w; x++) {
                const unsigned p = s[x];
                unsigned r, g, b;
                if (p565) { r = p >> 11; g = (p >> 5) & 63; b = p & 31; g = g << 2 | g >> 4; }
                else { r = (p >> 10) & 31; g = (p >> 5) & 31; b = p & 31; g = g << 3 | g >> 2; }
                d[x] = 0xff000000u | (r << 3 | r >> 2) << 16 | g << 8 | (b << 3 | b >> 2);
            }
        }
    } else if (g_core.fmt == RETRO_PIXEL_FORMAT_0RGB1555) {
        for (unsigned y = 0; y < h; y++) {
            const uint16_t* s = (const uint16_t*)(src + y * pitch);
            uint16_t* d = (uint16_t*)(dst + (size_t)y * dp);
            for (unsigned x = 0; x < w; x++) d[x] = (uint16_t)(((s[x] & 0x7fe0) << 1) | (s[x] & 0x1f) | ((s[x] >> 4) & 0x20));
        }
    } else {
        const size_t row = (size_t)w * (fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2);
        if (row == pitch && (size_t)dp == pitch) memcpy(dst, src, row * h);
        else
            for (unsigned y = 0; y < h; y++) memcpy(dst + (size_t)y * dp, src + y * pitch, row);
    }
    frame_new = true;
}

// XRGB8888 copy of the last picture (thumbnails, screenshots); malloc'd
uint32_t* bf_frame_rgb(unsigned* w, unsigned* h)
{
    if (!last.data) return NULL;
    uint32_t* o = malloc((size_t)last.w * last.h * 4);
    if (!o) return NULL;
    for (unsigned y = 0; y < last.h; y++) {
        const uint8_t* s = (const uint8_t*)last.data + y * last.pitch;
        uint32_t* d = o + (size_t)y * last.w;
        for (unsigned x = 0; x < last.w; x++) {
            if (g_core.fmt == RETRO_PIXEL_FORMAT_XRGB8888) d[x] = ((const uint32_t*)s)[x] | 0xff000000u;
            else {
                const uint16_t p = ((const uint16_t*)s)[x];
                unsigned r, g, b;
                if (g_core.fmt == RETRO_PIXEL_FORMAT_RGB565) { r = p >> 11; g = (p >> 5) & 63; b = p & 31; g = g << 2 | g >> 4; }
                else { r = (p >> 10) & 31; g = (p >> 5) & 31; b = p & 31; g = g << 3 | g >> 2; }
                r = r << 3 | r >> 2;
                b = b << 3 | b >> 2;
                d[x] = 0xff000000u | r << 16 | g << 8 | b;
            }
        }
    }
    *w = last.w;
    *h = last.h;
    return o;
}

// ---- saves -------------------------------------------------------------------------------------------------------

typedef struct { unsigned id; const char* ext; void* last; size_t size; } sram_t;
static sram_t srams[2] = { { RETRO_MEMORY_SAVE_RAM, ".srm", NULL, 0 }, { RETRO_MEMORY_RTC, ".rtc", NULL, 0 } };

static void sram_load(void)
{
    for (int i = 0; i < 2; i++) {
        sram_t* s = &srams[i];
        void* mem = g_core.get_memory_data(s->id);
        size_t sz = g_core.get_memory_size(s->id);
        if (!mem || !sz) continue;
        char p[1600];
        bf_path_save(&g_paths, s->ext, p, sizeof p);
        size_t n = 0;
        void* d = bf_read_file(p, &n);
        if (d) {
            // (RetroArch loads what fits: gpSP's 128 KB file into mGBA's 512-byte EEPROM)
            memcpy(mem, d, n < sz ? n : sz);
            free(d);
            bf_log("save: %s (%zu of %zu bytes)", p, n < sz ? n : sz, sz);
        }
        s->last = malloc(sz);
        s->size = s->last ? sz : 0;  // (out of memory: this save is left as it is on the card)
        if (s->last) memcpy(s->last, mem, sz);
    }
}

// the save's write off the main thread while the game runs (an fsync on the card can take 100 ms: a dropped frame
// every 10 s for a game that writes its save all the time); the last one on quit is written here and waited for
typedef struct { char path[1600]; void* data; size_t size; } sram_job;
static int writes_in_flight;  // (saves and states being written on threads: waited for before the program ends)
static pthread_mutex_t sram_mu = PTHREAD_MUTEX_INITIALIZER;
static void* sram_thread(void* a)
{
    sram_job* j = a;
    pthread_mutex_lock(&sram_mu);  // (one write at a time, in order)
    const bool ok = bf_write_file(j->path, j->data, j->size);
    pthread_mutex_unlock(&sram_mu);
    if (!ok) bf_log("save: can't write %s: %s", j->path, strerror(errno));
    free(j->data);
    free(j);
    __atomic_sub_fetch(&writes_in_flight, 1, __ATOMIC_ACQ_REL);
    return NULL;
}

static void sram_save(bool final)
{
    if (getenv("BANANAFRONT_NO_SAVE_WRITE")) return;  // (tests in someone's game: their save left alone)
    for (int i = 0; i < 2; i++) {
        sram_t* s = &srams[i];
        void* mem = g_core.get_memory_data(s->id);
        if (!mem || !s->size || !s->last) continue;
        if (!memcmp(mem, s->last, s->size)) continue;
        memcpy(s->last, mem, s->size);
        sram_job* j = malloc(sizeof *j);
        if (!j || !(j->data = malloc(s->size))) { free(j); continue; }
        bf_path_save(&g_paths, s->ext, j->path, sizeof j->path);
        bf_mkdirs(g_paths.save_dir);
        memcpy(j->data, mem, s->size);
        j->size = s->size;
        bf_log("save written: %s%s", j->path, final ? " (quit)" : "");
        pthread_t t;
        __atomic_add_fetch(&writes_in_flight, 1, __ATOMIC_ACQ_REL);
        if (final || pthread_create(&t, NULL, sram_thread, j) != 0) sram_thread(j);
        else pthread_detach(t);
    }
}

// ---- states ------------------------------------------------------------------------------------------------------

__attribute__((weak)) void bf_thumbnail_write(const char* state_path) { (void)state_path; }

// a state: the core's data and the picture taken here (main thread, between frames), compressed and written on a
// thread (the game goes on: ~10-60 ms of zlib + the card's fsync); written here when quitting
typedef struct { int slot; char path[1600]; void* core; size_t n; uint32_t* px; unsigned w, h; double t0; } state_job;
static pthread_mutex_t state_mu = PTHREAD_MUTEX_INITIALIZER;
extern void bf_png_write(const char* path, const uint32_t* px, unsigned w, unsigned h);
static void* state_thread(void* a)
{
    state_job* j = a;
    pthread_mutex_lock(&state_mu);
    size_t fs = 0;
    void* f = bf_state_pack(j->core, j->n, true, &fs);
    bool ok = f && bf_write_file(j->path, f, fs);
    free(f);
    if (ok && j->px) {
        char p[1700];
        snprintf(p, sizeof p, "%s.png", j->path);
        bf_png_write(p, j->px, j->w, j->h);
    }
    pthread_mutex_unlock(&state_mu);
    bf_log("state %s: %s (%zu -> %zu bytes, %.0f ms)", ok ? "saved" : "NOT saved", j->path, j->n, fs, bf_now_ms() - j->t0);
    frontend_event(ok ? BF_EV_STATE_SAVED : BF_EV_STATE_FAILED, j->slot);
    free(j->core);
    free(j->px);
    free(j);
    __atomic_sub_fetch(&writes_in_flight, 1, __ATOMIC_ACQ_REL);
    return (void*)(intptr_t)ok;
}

bool bf_state_save(int s)
{
    size_t n = g_core.serialize_size();
    if (!n) { bf_log("state: the core can't save states"); return false; }
    bf_cpugov_boost(1000);
    state_job* j = calloc(1, sizeof *j);
    if (!j || !(j->core = malloc(n))) { free(j); return false; }
    j->t0 = bf_now_ms();
    if (!g_core.serialize(j->core, n)) { free(j->core); free(j); bf_log("state: the core's save failed"); return false; }
    j->n = n;
    j->slot = s;
    j->px = bf_frame_rgb(&j->w, &j->h);
    bf_path_state(&g_paths, s, j->path, sizeof j->path);
    bf_mkdirs(g_paths.state_dir);
    pthread_t t;
    __atomic_add_fetch(&writes_in_flight, 1, __ATOMIC_ACQ_REL);
    if (bf_quitting() || pthread_create(&t, NULL, state_thread, j) != 0) return state_thread(j) != NULL;
    pthread_detach(t);
    return true;
}

bool bf_state_load(int s)
{
    pthread_mutex_lock(&state_mu);  // (a state still being written is waited for)
    pthread_mutex_unlock(&state_mu);
    if (ra_hardcore()) { bf_log("state: no loading in hardcore"); bf_popup_push("RetroAchievements", "Hardcore: no states", 0xff7a4a, 3000); return false; }
    char p[1600];
    bf_path_state(&g_paths, s, p, sizeof p);
    size_t fs = 0, cs = 0;
    void* f = bf_read_file(p, &fs);
    if (!f) { bf_log("state: no %s", p); frontend_event(BF_EV_STATE_MISSING, s); return false; }
    void* c = bf_state_unpack(f, fs, &cs);
    free(f);
    bool ok = c && g_core.unserialize(c, cs);
    free(c);
    if (ok) ra_state_loaded();
    bf_log("state %s: %s", ok ? "loaded" : "NOT loaded", p);
    frontend_event(ok ? BF_EV_STATE_LOADED : BF_EV_STATE_FAILED, s);
    return ok;
}

// ---- commands: banana-ui's game panel and bananad, RetroArch's words on stdin ------------------------------------

static int cmd_fd = -1;
static char cmd_buf[4096];
static size_t cmd_len;
static FILE* reply_out;

static void read_memory(const char* arg)
{
    unsigned addr = 0, n = 0;
    if (sscanf(arg, "%x %u", &addr, &n) != 2) return;
    if (n > 65536) n = 65536;
    unsigned max = 0;
    int err = 0;
    const uint8_t* p = bf_mmap_ptr(addr, &max, &err);
    if (!reply_out) return;
    if (!p) { fprintf(reply_out, "READ_CORE_MEMORY %x -1 %s\n", addr, bf_mmap_err(err)); fflush(reply_out); return; }
    if (n > max) n = max;
    // (one write: hex, as RetroArch's " %02X")
    char* b = malloc(32 + n * 3 + 2);
    int o = sprintf(b, "READ_CORE_MEMORY %x", addr);
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < n; i++) { b[o++] = ' '; b[o++] = hex[p[i] >> 4]; b[o++] = hex[p[i] & 15]; }
    b[o++] = '\n';
    fwrite(b, 1, o, reply_out);
    fflush(reply_out);
    free(b);
}

void bf_command(const char* line)
{
    char w[64] = { 0 };
    const char* arg = "";
    sscanf(line, "%63s", w);
    const char* sp = strchr(line, ' ');
    if (sp) arg = sp + 1;
    if (!strcmp(w, "QUIT")) quit = true;
    else if (!strcmp(w, "SAVE_STATE")) bf_state_save(slot);
    else if (!strcmp(w, "LOAD_STATE")) bf_state_load(slot);
    else if (!strcmp(w, "SAVE_STATE_SLOT")) bf_state_save(atoi(arg));
    else if (!strcmp(w, "LOAD_STATE_SLOT")) bf_state_load(atoi(arg));
    else if (!strcmp(w, "STATE_SLOT_PLUS")) { if (slot < 999) slot++; frontend_event(BF_EV_SLOT, slot); }
    else if (!strcmp(w, "STATE_SLOT_MINUS")) { if (slot > -1) slot--; frontend_event(BF_EV_SLOT, slot); }
    else if (!strcmp(w, "FAST_FORWARD") || !strcmp(w, "FAST_FORWARD_HOLD")) ff = !ff;
    else if (!strcmp(w, "PAUSE_TOGGLE")) paused = !paused;
    else if (!strcmp(w, "MENU_TOGGLE")) frontend_menu();
    else if (!strcmp(w, "SCREENSHOT")) frontend_screenshot();
    else if (!strcmp(w, "RESET")) { g_core.reset(); ra_reset(); }
    else if (!strcmp(w, "READ_CORE_MEMORY")) read_memory(arg);
    else if (!strcmp(w, "AUDIO_REINIT")) { bf_audio_close(); bf_audio_open(A.audio_dev, A.audio_ms); }
    else if (w[0]) bf_log("command not known: %s", w);
}

static void bf_commands_poll_(void);
void bf_commands_poll(void)
{
    static bool busy;  // (not re-entered: a command may run the menu, whose loop reads the pipe)
    if (cmd_fd < 0 || busy) return;
    busy = true;
    bf_commands_poll_();
    busy = false;
}

static void bf_commands_poll_(void)
{
    for (;;) {
        ssize_t r = read(cmd_fd, cmd_buf + cmd_len, sizeof cmd_buf - 1 - cmd_len);
        if (r == 0) { cmd_fd = -1; return; }  // (EOF: /dev/null, or the writer gone)
        if (r < 0) return;
        cmd_len += (size_t)r;
        cmd_buf[cmd_len] = 0;
        char* s = cmd_buf;
        char* nl;
        while ((nl = strchr(s, '\n'))) {
            *nl = 0;
            bf_command(s);
            s = nl + 1;
        }
        cmd_len = strlen(s);
        memmove(cmd_buf, s, cmd_len);
        if (cmd_len == sizeof cmd_buf - 1) cmd_len = 0;  // (a line too long: dropped)
    }
}

// the menu's Quit: with the automatic state when Quick resume is on (nosave: never)
static bool quit_nosave;
void bf_quit(bool nosave) { quit = true; quit_nosave = nosave; }
bool bf_quitting(void) { return quit || sig_quit; }
bool bf_resume_on(void) { return A.resume_save; }
const char* bf_rom(void) { return A.rom; }
const char* bf_audio_device(void) { return A.audio_dev; }

// ---- hotkeys -----------------------------------------------------------------------------------------------------

static void job(const char* j)
{
    if (!strcmp(j, "ff")) ff = !ff;
    else if (!strcmp(j, "save")) bf_state_save(slot);
    else if (!strcmp(j, "load")) bf_state_load(slot);
    else if (!strcmp(j, "menu")) frontend_menu();
    else if (!strcmp(j, "pause")) paused = !paused;
    else if (!strcmp(j, "screenshot")) frontend_screenshot();
}

static bool hotkeys_on;
// (called from input reading, between frames)
static void on_button(int id, bool down, bool f)
{
    if (!down) {
        if (id == RETRO_DEVICE_ID_JOYPAD_R2 && ff_hold) { ff = false; ff_hold = false; }
        return;
    }
    if (!f) {
        if (id >= 0 && id < 16 && jobs[id][0]) job(jobs[id]);
        return;
    }
    // F + a button: RetroArch's hotkeys on this pad (retrogame_joypad.cfg), so hands already know them; with
    // Settings > Other systems > RetroArch hotkeys off (ra_hotkeys), only the two needed to get out: F + X and F + START
    if (!hotkeys_on && id != RETRO_DEVICE_ID_JOYPAD_START && id != RETRO_DEVICE_ID_JOYPAD_X && id != BF_HK_ALONE) return;
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_START: quit = true; break;
    case RETRO_DEVICE_ID_JOYPAD_R: bf_state_save(slot); break;
    case RETRO_DEVICE_ID_JOYPAD_L: bf_state_load(slot); break;
    case RETRO_DEVICE_ID_JOYPAD_R2: ff = !ff; break;
    case RETRO_DEVICE_ID_JOYPAD_X: frontend_menu(); break;
    case RETRO_DEVICE_ID_JOYPAD_Y: frontend_fps_toggle(); break;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: frontend_screenshot(); break;
    case RETRO_DEVICE_ID_JOYPAD_UP: if (slot < 999) slot++; frontend_event(BF_EV_SLOT, slot); break;
    case RETRO_DEVICE_ID_JOYPAD_DOWN: if (slot > 0) slot--; frontend_event(BF_EV_SLOT, slot); break;
    case BF_HK_ALONE: frontend_menu(); break;  // F pressed and let go alone: the menu, as in BananaDS
    default: break;
    }
}

static void load_jobs(void)
{
    hotkeys_on = true;
    char hk[16] = "";
    if (bf_conf_file_get("/data/banana/settings.conf", "ra_hotkeys", hk, sizeof hk) && !strcmp(hk, "off")) hotkeys_on = false;
    // Settings > Other systems > Buttons (PR #151): L2, R2, L3, R3 alone. Defaults: R2 fast-forward, the rest the game's
    static const struct { const char* key; int id; const char* def; } B[] = {
        { "ra_btn_l2", RETRO_DEVICE_ID_JOYPAD_L2, "game" }, { "ra_btn_r2", RETRO_DEVICE_ID_JOYPAD_R2, "ff" },
        { "ra_btn_l3", RETRO_DEVICE_ID_JOYPAD_L3, "game" }, { "ra_btn_r3", RETRO_DEVICE_ID_JOYPAD_R3, "game" } };
    for (unsigned i = 0; i < 4; i++) {
        char v[32] = "";
        if (!bf_conf_file_get("/data/banana/settings.conf", B[i].key, v, sizeof v) &&
            !bf_conf_file_get("/etc/banana/settings.conf", B[i].key, v, sizeof v))
            snprintf(v, sizeof v, "%s", B[i].def);
        const char* env = getenv(B[i].key);  // (tests)
        if (env) snprintf(v, sizeof v, "%s", env);
        if (!strcmp(v, "ff") || !strcmp(v, "save") || !strcmp(v, "load") || !strcmp(v, "menu") || !strcmp(v, "pause") ||
            !strcmp(v, "screenshot") || !strcmp(v, "none")) {
            snprintf(jobs[B[i].id], sizeof jobs[0], "%s", v);
            job_mask |= 1u << B[i].id;
        }
    }
    bf_input_set_job_mask(job_mask);
}

// ---- signals -----------------------------------------------------------------------------------------------------

static void on_signal(int s)
{
    sig_quit = 1;
    if (s == SIGUSR1) sig_state = 1;  // bananad's Power off after sleeping: the automatic state, whatever the setting
}

// ---- the loop ----------------------------------------------------------------------------------------------------

static void usage(void)
{
    fprintf(stderr, "bananafront %s: bananafront [options] <core.so> <content> (see main.c)\n", BF_VERSION);
}

static bool parse_args(int argc, char** argv)
{
    static const struct option o[] = {
        { "system", 1, 0, 's' }, { "saves", 1, 0, 'S' }, { "save-dir", 1, 0, 'D' }, { "states", 1, 0, 'T' },
        { "shots", 1, 0, 'P' }, { "bios", 1, 0, 'B' }, { "config", 1, 0, 'C' }, { "screen", 1, 0, 'n' },
        { "scale", 1, 0, 'z' }, { "audio-device", 1, 0, 'a' }, { "audio-latency", 1, 0, 'l' },
        { "resume-load", 0, 0, 'r' }, { "cpugov", 0, 0, 'g' }, { "lowlat", 0, 0, 'L' }, { "shader", 1, 0, 'H' }, { "border", 1, 0, 'O' }, { "border-rect", 1, 0, 'Q' }, { "resume-save", 0, 0, 'R' }, { "frames", 1, 0, 'f' }, { "stats", 1, 0, 'x' },
        { "help", 0, 0, 'h' }, { 0, 0, 0, 0 } };
    int c;
    while ((c = getopt_long(argc, argv, "", o, NULL)) != -1) {
        switch (c) {
        case 's': A.system = optarg; break;
        case 'S': A.saves = optarg; break;
        case 'D': A.save_dir = optarg; break;
        case 'T': A.states = optarg; break;
        case 'P': A.shots = optarg; break;
        case 'B': A.bios = optarg; break;
        case 'C': A.config = optarg; break;
        case 'n': A.screen = !strcmp(optarg, "bottom"); break;
        case 'z': A.scale = !strcmp(optarg, "integer") ? BF_SCALE_INTEGER : !strcmp(optarg, "stretch") ? BF_SCALE_STRETCH : BF_SCALE_FIT; break;
        case 'a': A.audio_dev = optarg; break;
        case 'l': A.audio_ms = atoi(optarg); break;
        case 'r': A.resume_load = true; break;
        case 'g': A.cpugov = true; break;
        case 'L': A.lowlat = true; break;
        case 'H': A.shader = optarg; break;
        case 'O': A.border = optarg; break;
        case 'Q': sscanf(optarg, "%d,%d,%d,%d", &A.brect[0], &A.brect[1], &A.brect[2], &A.brect[3]); break;
        case 'R': A.resume_save = true; break;
        case 'f': A.frames = atol(optarg); break;
        case 'x': A.stats = optarg; break;
        default: usage(); return false;
        }
    }
    if (argc - optind != 2) { usage(); return false; }
    A.core = argv[optind];
    A.rom = argv[optind + 1];
    if (A.audio_ms < 16) A.audio_ms = 16;
    return true;
}

int main(int argc, char** argv)
{
    if (!parse_args(argc, argv)) return 2;
    const double t_start = bf_now_ms();
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (!bf_core_open(A.core)) return 1;
    bf_paths_init(&g_paths, A.rom, g_core.sys.library_name, A.saves, A.states, A.shots, A.bios, A.save_dir);
    bf_env_system_dir = g_paths.system_dir;
    bf_env_save_dir = g_paths.save_dir;
    bf_mkdirs(g_paths.save_dir);
    bf_core_options_load(A.config, g_paths.game);
    if (!bf_core_load_game(A.rom)) { bf_core_close(); return 1; }
    bf_mmap_refresh();
    sram_load();

    const char* top = getenv("BANANA_TOP");
    bf_video_opts vo = { .screen = A.screen, .top_conn = top ? top : "DSI-2", .scale = A.scale };
    if (!bf_video_open(&vo)) { sram_save(true); bf_core_close(); return 1; }
    if (A.shader && *A.shader) bf_video_shader(A.shader);
    const double refresh = bf_video_refresh();
    // one core frame per refresh when the rates are within 2 % (59.73-60.10 Hz games on a 60.005 Hz panel), the
    // sound scaled to match; else (a 50 Hz game) frames are paced by time and some refreshes repeat one
    const double fps = bf_core_fps();
    const bool locked = fabs(fps - refresh) / refresh < 0.02;
    if (!bf_audio_open(A.audio_dev, A.audio_ms)) bf_log("audio: none, the game runs silent");
    bf_audio_set_rate(g_core.av.timing.sample_rate, fps, refresh, locked);
    bf_input_open(on_button);
    if (getenv("BANANAFRONT_LOWLAT") && atoi(getenv("BANANAFRONT_LOWLAT"))) A.lowlat = true;
    if (A.cpugov || (getenv("BANANAFRONT_CPUGOV") && atoi(getenv("BANANAFRONT_CPUGOV")))) bf_cpugov_start(g_paths.game, refresh);
    load_jobs();
    frontend_init(A.system, A.rom);
    if (A.border && A.brect[2] > 0) frontend_border(A.border, A.brect[0], A.brect[1], A.brect[2], A.brect[3]);

    // the command pipe: stdin when it's a pipe/FIFO (run-bananafront: < $BANANA_RA_CMD)
    if (isatty(0) == 0) {
        cmd_fd = 0;
        fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    }
    reply_out = stdout;

    if (A.resume_load) bf_state_load(-1);
    bf_log("running: %.3f fps game on a %.3f Hz panel (%s), started in %.0f ms", fps, refresh,
           locked ? "one frame a refresh" : "time-paced", bf_now_ms() - t_start);

    S.t0 = bf_now_ms();
    S.cpu0 = cpu_ms();
    double next_stats = S.t0 + 10000, next_sram = S.t0 + 10000;
    double acc = 0;  // time-paced: frames owed
    double ll_budget = 1000.0 / refresh;  // --lowlat: how long before the next refresh a frame starts
    bool want_frame = true;
    while (!quit && !sig_quit) {
        // wait: for the flip (the next refresh), input, commands
        struct pollfd pf[24];
        int n = 0;
        const int vfd = bf_video_fd();
        if (vfd >= 0) pf[n++] = (struct pollfd){ vfd, POLLIN, 0 };
        int ifds[16];
        const int ni = bf_input_fds(ifds, 16);
        for (int i = 0; i < ni; i++) pf[n++] = (struct pollfd){ ifds[i], POLLIN, 0 };
        const int cpos = n;
        if (cmd_fd >= 0) pf[n++] = (struct pollfd){ cmd_fd, POLLIN, 0 };
        const bool can_run = !bf_video_flip_pending() && want_frame && !paused;
        // low latency (--lowlat): the frame starts as late as it can and still make the next refresh, so the
        // buttons it reads are that much fresher (RetroArch's frame delay, set by itself from the frames' times)
        int timeout = can_run ? 0 : (paused ? 50 : 100);
        if (can_run && A.lowlat && !ff) {
            const double wake = bf_video_last_flip_ms() + 1000.0 / refresh - ll_budget;
            const double now = bf_now_ms();
            if (wake > now + 0.5) timeout = (int)(wake - now);
        }
        int r = poll(pf, n, timeout);
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            if (vfd >= 0 && (pf[0].revents & POLLIN)) {
                bf_video_events();
                const double t = bf_video_last_flip_ms();
                if (S.flip_prev > 0 && t - S.flip_prev < 100) { S.flip_sum += t - S.flip_prev; S.flip_n++; }
                S.flip_prev = t;
                S.vblanks++;
                want_frame = true;
            }
            for (int i = (vfd >= 0); i < cpos; i++)
                if (pf[i].revents & POLLIN) bf_input_read(pf[i].fd);
            if (cmd_fd >= 0 && cpos < n && (pf[cpos].revents & (POLLIN | POLLHUP))) bf_commands_poll();
        }
        if (bf_menu_run_pending()) want_frame = true;  // (the menu took the flip events while it was open)
        if (paused) { frontend_paused(); continue; }
        if (bf_video_flip_pending() || !want_frame) continue;
        if (A.lowlat && !ff && bf_now_ms() < bf_video_last_flip_ms() + 1000.0 / refresh - ll_budget - 0.5) continue;
        const double input_at = bf_now_ms();

        // the frame(s) for this refresh
        int nrun = 1;
        if (!locked) {
            acc += fps / refresh;
            nrun = (int)acc;
            acc -= nrun;
        }
        if (ff && ra_hardcore()) ff = false;  // (hardcore: no fast-forward)
        bf_env_fastforward = ff;
        bf_audio_mute(ff);
        frame_new = frame_dupe = false;
        const double t0 = bf_now_ms();
        if (ff) {
            // as many frames as fit in most of a refresh (the last one shown)
            const double budget = 1000.0 / refresh * 0.8;
            nrun = 0;
            do { g_core.run(); frontend_core_frame(); nrun++; } while (bf_now_ms() - t0 < budget && nrun < 16);
        } else
            for (int i = 0; i < nrun; i++) { g_core.run(); frontend_core_frame(); }
        const double t1 = bf_now_ms();
        frames_run += nrun;
        S.frames += nrun;
        if (S.n < NSAMP && nrun) S.run_ms[S.n++] = (t1 - t0) / nrun;
        const bool late = t1 - bf_video_last_flip_ms() > 1000.0 / refresh;
        if (late) S.late++;
        // the input's age when the picture starts to show: from the read to the next refresh
        if (bf_video_last_flip_ms() > 0) {
            S.lat_sum += bf_video_last_flip_ms() + 1000.0 / refresh * (late ? 2 : 1) - input_at;
            S.lat_n++;
        }
        if (A.lowlat && !ff) {
            // the budget: the slowest recent frame and 3 ms for the commit to land before the panel latches it;
            // a late frame adds 2 ms at once, and it shrinks back slowly
            if (late) ll_budget += 2.0;
            const double want = (t1 - t0) * 1.3 + 3.0;
            if (want > ll_budget) ll_budget = want;
            else ll_budget -= (ll_budget - want) * 0.01;
            const double cap = 1000.0 / refresh - 1.0;
            if (ll_budget > cap) ll_budget = cap;
        }
        if (!ff) bf_cpugov_frame(t1 - t0, late);
        bf_cpugov_tick();
        const double tf = bf_now_ms();
        bf_audio_flush();
        frontend_frame();
        if (g_core.av_changed) {
            g_core.av_changed = false;
            bf_audio_set_rate(g_core.av.timing.sample_rate, bf_core_fps(), refresh, locked);
        }
        if (!frame_new) S.dupes++;
        // the flip event asks for the next frame; a commit that failed (a lease taken back?) waits a refresh instead
        const double tc = bf_now_ms();
        want_frame = !bf_video_commit(!frame_new);
        S.commit_ms += bf_now_ms() - tc;
        S.fe_ms += bf_now_ms() - tf;
        if (want_frame) usleep((useconds_t)(1e6 / refresh));

        if (t1 >= next_sram) { sram_save(false); next_sram = t1 + 10000; }
        if (t1 >= next_stats) { stats_line(false); next_stats = t1 + 10000; }
        if (A.frames && (long)frames_run >= A.frames) quit = true;
    }
    stats_line(true);
    if ((A.resume_save && !quit_nosave) || sig_state) bf_state_save(-1);
    sram_save(true);
    // (the writes still on their threads finish first: up to 10 s, never cut short by our own exit)
    for (int i = 0; i < 1000 && __atomic_load_n(&writes_in_flight, __ATOMIC_ACQUIRE) > 0; i++) usleep(10000);
    frontend_quit();
    bf_cpugov_stop();
    bf_input_close();
    bf_audio_close();
    bf_video_close();
    bf_core_close();
    bf_log("quit after %llu frames", (unsigned long long)frames_run);
    return 0;
}
