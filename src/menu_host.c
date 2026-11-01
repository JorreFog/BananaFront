// menu_host.c: BananaFront's side of the in-game menu (menu.c = BananaDS's menu, one panel): its canvas is a
// panel-sized buffer on the game's panel (kms.c), its keys the pad's raw events (input.c), its sounds go straight to
// ALSA while the game's are stopped. The game, its sound and its clock stand still while it is open; the companion
// export and RetroAchievements are kept alive and the command pipe is read (MENU_TOGGLE / QUIT close it).
#define _GNU_SOURCE
#include <dirent.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "bf.h"
#include "frontend.h"
#include "input.h"
#include "menu.h"
#include "ra.h"
#include "video.h"

static bool menu_open, close_req;
static double next_service;

static int canvas(void* ctx, int panel, uint32_t** px, int* pitch, int* w, int* h)
{
    (void)ctx;
    if (panel == 0) return -1;  // (one panel: the menu's controls half only)
    return bf_video_canvas(px, pitch, w, h);
}
static void show(void* ctx, int panel, int k) { (void)ctx; if (panel == 1) bf_video_canvas_show(k); }
static int grab_top(void* ctx, uint32_t* dst, int pitch, int w, int h) { (void)ctx; (void)dst; (void)pitch; (void)w; (void)h; return 0; }

static int map_key(int code)
{
    switch (code) {
    case BTN_EAST: case KEY_ENTER: return BM_A;
    case BTN_SOUTH: case KEY_BACKSPACE: return BM_B;
    case BTN_WEST: return BM_Y;
    case BTN_NORTH: return BM_X;
    case BTN_TL: return BM_L;
    case BTN_TR: return BM_R;
    case BTN_START: return BM_START;
    case BTN_DPAD_UP: case KEY_UP: return BM_UP;
    case BTN_DPAD_DOWN: case KEY_DOWN: return BM_DOWN;
    case BTN_DPAD_LEFT: case KEY_LEFT: return BM_LEFT;
    case BTN_DPAD_RIGHT: case KEY_RIGHT: return BM_RIGHT;
    case BTN_MODE: case KEY_ESC: return BM_MENU;
    default: return -1;
    }
}

static int poll_ev(void* ctx, bmenu_ev* ev)
{
    (void)ctx;
    // (the menu's loop calls this every few ms: the pads, the pipe, and the companion/RA a few times a second)
    int fds[16];
    const int n = bf_input_fds(fds, 16);
    struct pollfd pf[16];
    for (int i = 0; i < n; i++) pf[i] = (struct pollfd){ fds[i], POLLIN, 0 };
    if (n > 0 && poll(pf, (nfds_t)n, 0) > 0)
        for (int i = 0; i < n; i++)
            if (pf[i].revents & (POLLIN | POLLERR | POLLHUP)) bf_input_read(pf[i].fd);
    const double t = bf_now_ms();
    if (t >= next_service) {
        next_service = t + 250;
        bf_commands_poll();
        frontend_paused();
    }
    if (close_req || bf_quitting()) {
        close_req = false;
        *ev = (bmenu_ev){ .kind = BM_EV_KEY, .key = BM_START, .down = 1 };  // (START closes the menu)
        return 1;
    }
    int code, value;
    while (bf_input_raw_take(&code, &value)) {
        const int k = map_key(code);
        if (k < 0) continue;
        *ev = (bmenu_ev){ .kind = BM_EV_KEY, .key = k, .down = value != 0 };
        return 1;
    }
    return 0;
}

static int battery(void* ctx, int* charging)
{
    (void)ctx;
    *charging = 0;
    DIR* d = opendir("/sys/class/power_supply");
    if (!d) return -1;
    struct dirent* e;
    int pct = -1;
    while ((e = readdir(d)) && pct < 0) {
        char p[300], v[32] = "";
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        FILE* f = fopen(p, "r");
        if (!f) continue;
        if (fgets(v, sizeof v, f) && !strncmp(v, "Battery", 7)) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
            FILE* c = fopen(p, "r");
            if (c) { if (fscanf(c, "%d", &pct) != 1) pct = -1; fclose(c); }
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
            FILE* s = fopen(p, "r");
            char st[32] = "";
            if (s) { if (fgets(st, sizeof st, s)) *charging = !strncmp(st, "Charging", 8) || !strncmp(st, "Full", 4); fclose(s); }
        }
        fclose(f);
    }
    closedir(d);
    return pct;
}

// the menu's slots 1-8 are RetroArch's slots 0-7 (slot 1 = RetroArch's first, ".state": what F + R saves, in both)
static int ra_slot(int menu_slot) { return menu_slot == BM_UNDO_SLOT ? -2 : menu_slot - 1; }
static int save(void* ctx, int slot) { (void)ctx; return bf_state_save(ra_slot(slot)) ? 0 : -1; }
static int load(void* ctx, int slot) { (void)ctx; return bf_state_load(ra_slot(slot)) ? 0 : -1; }
static void state_file(void* ctx, int slot, char* out, int n)
{
    (void)ctx;
    bf_path_state(&g_paths, ra_slot(slot), out, (size_t)n);
}
static void quit(void* ctx) { (void)ctx; bf_quit(false); }
static void quit_nosave(void* ctx) { (void)ctx; bf_quit(true); }
static void restart(void* ctx) { (void)ctx; g_core.reset(); ra_reset(); }
static void mlog(void* ctx, const char* line) { (void)ctx; bf_log("%s", line); }
#include "cpugov.h"
static void boost(void* ctx, int ms) { (void)ctx; bf_cpugov_boost(ms); }

// the Hotkeys page, in BananaDS's words (the same lines, already translated); slot 1 is RetroArch's first slot
static int hotkey(void* ctx, int i, const char** keys, const char** what)
{
    (void)ctx;
    static const char* K[][2] = { { "F / F + X", "This menu" }, { "F + R", "Save state (slot 1)" },
                                  { "F + L", "Load state (slot 1)" }, { "F + R2", "Fast-forward on / off" },
                                  { "F + SELECT", "Screenshot" }, { "F + Y", "FPS counter" }, { "F + START", "Quit game" },
                                  { "VOL + / -", "Volume" }, { "F + VOL + / -", "Brightness" } };
    if (i < 0 || i >= (int)(sizeof K / sizeof *K)) return 0;
    *keys = K[i][0];
    *what = K[i][1];
    return 1;
}

// the menu's sounds: 48 kHz stereo copies (linear resampling: short UI clicks)
typedef struct { int16_t* pcm; size_t frames; } snd_t;
static snd_t snd[8];
static void sound_load(void* ctx, int id, const int16_t* pcm, int frames, int rate, int channels)
{
    (void)ctx;
    if (id < 0 || id >= 8 || !pcm || frames <= 0 || rate <= 0 || channels < 1) return;
    const size_t out = (size_t)((double)frames * BF_AUDIO_RATE / rate);
    int16_t* o = malloc(out * 4);
    if (!o) return;
    for (size_t i = 0; i < out; i++) {
        const double x = (double)i * rate / BF_AUDIO_RATE;
        const size_t a = (size_t)x;
        const size_t b = a + 1 < (size_t)frames ? a + 1 : a;
        const double f = x - a;
        for (int c = 0; c < 2; c++) {
            const int cc = channels > 1 ? c : 0;
            o[i * 2 + c] = (int16_t)(pcm[a * channels + cc] * (1 - f) + pcm[b * channels + cc] * f);
        }
    }
    free(snd[id].pcm);
    snd[id].pcm = o;
    snd[id].frames = out;
}
static void sound_play(void* ctx, int id)
{
    (void)ctx;
    if (id >= 0 && id < 8 && snd[id].pcm) bf_audio_raw(snd[id].pcm, snd[id].frames);
}

// Quick settings: the picture's look and size, for this game (Settings > Other systems keeps the defaults)
static const char* SH_NAME[] = { "", "zfast-lcd", "zfast-crt", "sharp-bilinear" };
static const char* SH_LABEL[] = { "Off", "Fast LCD", "Fast CRT", "Sharp pixels" };
static const char* SC_LABEL[] = { "Keep the shape", "Whole numbers", "Fill the screen" };
static int opt_count(void* ctx) { (void)ctx; return 2; }
static const char* opt_label(void* ctx, int i) { (void)ctx; return i == 0 ? "Shader" : "Picture size"; }
static int opt_values(void* ctx, int i) { (void)ctx; return i == 0 ? 4 : 3; }
static const char* opt_value_name(void* ctx, int i, int v) { (void)ctx; return i == 0 ? SH_LABEL[v & 3] : SC_LABEL[v % 3]; }
static int opt_get(void* ctx, int i)
{
    (void)ctx;
    if (i == 1) return (int)bf_video_get_scale();
    const char* n = bf_video_shader_name();
    for (int k = 1; k < 4; k++) if (!strcmp(n, SH_NAME[k])) return k;
    return 0;
}
static void opt_set(void* ctx, int i, int v)
{
    (void)ctx;
    if (i == 1) { bf_video_set_scale((enum bf_scale)(v % 3)); return; }
    if (v <= 0) bf_video_shader_off();
    else bf_video_shader(SH_NAME[v & 3]);
}

static bool inited, open_req;

// F + X, F alone, MENU_TOGGLE: asked here, opened by main's loop between frames (never from inside the command
// reader or the input reader, which the menu's own loop calls again); asked while open: it closes
void bf_menu_open(void)
{
    if (menu_open) close_req = true;
    else open_req = true;
}

bool bf_menu_run_pending(void)
{
    if (!open_req) return false;
    open_req = false;
    if (!inited) {
        bmenu_host h = { 0 };
        h.rom = bf_rom();
        h.resume_on = bf_resume_on();
        h.canvas = canvas;
        h.show = show;
        h.grab_top = grab_top;
        h.poll = poll_ev;
        h.battery = battery;
        h.save = save;
        h.load = load;
        h.state_file = state_file;
        h.quit = quit;
        h.quit_nosave = quit_nosave;
        h.hotkey = hotkey;
        h.restart = restart;
        h.sound_load = sound_load;
        h.sound_play = sound_play;
        h.boost = boost;
        h.log = mlog;
        h.opt_count = opt_count;
        h.opt_label = opt_label;
        h.opt_values = opt_values;
        h.opt_value_name = opt_value_name;
        h.opt_get = opt_get;
        h.opt_set = opt_set;
        bmenu_init(&h);
        bmenu_session_start();
        inited = true;
    }
    menu_open = true;
    frontend_overlay_pause(true);
    bf_input_raw(true);
    bf_audio_pause(true);
    const int ok = bmenu_run(0);
    bf_audio_pause(false);
    bf_input_raw(false);
    bf_video_canvas_done();
    frontend_menu_closed();
    frontend_overlay_pause(false);
    menu_open = false;
    if (!ok) bf_log("menu: couldn't open (fonts or panel buffers missing)");
    return true;
}
