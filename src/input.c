// input.c: the handheld's pad (and any USB/Bluetooth pad or keyboard) from evdev, read, never grabbed: bananad reads
// the same devices for the lid, power and volume keys. Mapped as RetroArch's autoconfig profile for the built-in pad
// (retrogame_joypad.cfg) maps it, so a game plays the same: Nintendo layout (A east, B south), L3/R3 the thumb
// buttons, F (BTN_MODE) the hotkey button: while F is held the other buttons go to the hotkeys, not the game.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "bf.h"
#include "input.h"

#define MAXDEV 16
typedef struct {
    int fd;
    char name[128];
    struct input_absinfo ax[ABS_CNT > 0x40 ? 0x40 : ABS_CNT];
    bool has_abs[0x40];
    int port;  // the player: the handheld's own pad (and keyboards) 0, other pads 1-3 in the order they were found
} dev_t_;
static dev_t_ devs[MAXDEV];
static int ndevs;

#define NPORT 4
static uint32_t pbuttons[NPORT];   // RETRO_DEVICE_ID_JOYPAD_* bits per player, as the pads have them
static uint32_t game_buttons[NPORT];  // what the game sees (player 1 minus what F and the jobs hold back)
static int16_t paxes[NPORT][4];    // left x/y, right x/y (-32768..32767)
#define buttons pbuttons[0]
static int cur_port;               // (the device whose events are being read)
static int hat_x, hat_y;
static bool hotkey_down, hotkey_used;
static bf_input_hotkey_fn on_hotkey;
static uint32_t job_mask;
// the menu's mode: every key (and the hat as d-pad keys) queued raw instead of going to the game and the hotkeys
static bool raw_mode;
static struct { int code, value; } rawq[64];
static int raw_head, raw_n;
static void raw_push(int code, int value)
{
    if (raw_n == 64) { raw_head = (raw_head + 1) % 64; raw_n--; }
    rawq[(raw_head + raw_n++) % 64].code = code;
    rawq[(raw_head + raw_n - 1) % 64].value = value;
}
void bf_input_raw(bool on)
{
    raw_mode = on;
    raw_n = 0;
    if (!on) { memset(pbuttons, 0, sizeof pbuttons); hotkey_down = false; }  // (what was held in the menu doesn't carry into the game)
}
bool bf_input_raw_take(int* code, int* value)
{
    if (!raw_n) return false;
    *code = rawq[raw_head].code;
    *value = rawq[raw_head].value;
    raw_head = (raw_head + 1) % 64;
    raw_n--;
    return true;
}

void bf_input_set_job_mask(uint32_t mask) { job_mask = mask; }

#define BIT(a, i) (((a)[(i) / (8 * sizeof(long))] >> ((i) % (8 * sizeof(long)))) & 1)

static int map_key(int code)
{
    switch (code) {
    case BTN_EAST: case KEY_X: return RETRO_DEVICE_ID_JOYPAD_A;
    case BTN_SOUTH: case KEY_Z: return RETRO_DEVICE_ID_JOYPAD_B;
    case BTN_NORTH: case KEY_S: return RETRO_DEVICE_ID_JOYPAD_X;
    case BTN_WEST: case KEY_A: return RETRO_DEVICE_ID_JOYPAD_Y;
    case BTN_TL: case KEY_Q: return RETRO_DEVICE_ID_JOYPAD_L;
    case BTN_TR: case KEY_W: return RETRO_DEVICE_ID_JOYPAD_R;
    case BTN_TL2: return RETRO_DEVICE_ID_JOYPAD_L2;
    case BTN_TR2: return RETRO_DEVICE_ID_JOYPAD_R2;
    case BTN_THUMBL: return RETRO_DEVICE_ID_JOYPAD_L3;
    case BTN_THUMBR: return RETRO_DEVICE_ID_JOYPAD_R3;
    case BTN_START: case KEY_ENTER: return RETRO_DEVICE_ID_JOYPAD_START;
    case BTN_SELECT: case KEY_RIGHTSHIFT: return RETRO_DEVICE_ID_JOYPAD_SELECT;
    case BTN_DPAD_UP: case KEY_UP: return RETRO_DEVICE_ID_JOYPAD_UP;
    case BTN_DPAD_DOWN: case KEY_DOWN: return RETRO_DEVICE_ID_JOYPAD_DOWN;
    case BTN_DPAD_LEFT: case KEY_LEFT: return RETRO_DEVICE_ID_JOYPAD_LEFT;
    case BTN_DPAD_RIGHT: case KEY_RIGHT: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
    default: return -1;
    }
}
static bool is_hotkey(int code) { return code == BTN_MODE || code == KEY_F1; }

static void open_dev(const char* path)
{
    if (ndevs >= MAXDEV) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 }, abs[ABS_MAX / (8 * sizeof(long)) + 1] = { 0 };
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    // a touchscreen is the panel's (banana-ui reads the game panel's own); a pad or keyboard has these
    const bool touch = BIT(abs, ABS_MT_POSITION_X) || BIT(keys, BTN_TOUCH);
    const bool pad = BIT(keys, BTN_SOUTH) || BIT(keys, BTN_DPAD_UP) || BIT(abs, ABS_HAT0X) || BIT(keys, KEY_UP) ||
                     BIT(keys, KEY_VOLUMEUP);
    if (touch || !pad) { close(fd); return; }
    dev_t_* d = &devs[ndevs++];
    memset(d, 0, sizeof *d);
    d->fd = fd;
    ioctl(fd, EVIOCGNAME(sizeof d->name), d->name);
    // players: the built-in pad first; another pad (USB, Bluetooth) the next free player; keyboards, the volume keys:
    // player 1
    const bool joypad = BIT(keys, BTN_SOUTH) || BIT(keys, BTN_DPAD_UP) || BIT(abs, ABS_HAT0X);
    d->port = 0;
    if (joypad && !strstr(d->name, "retrogame")) {
        bool used[NPORT] = { false };
        for (int i = 0; i < ndevs - 1; i++) used[devs[i].port] = true;
        bool builtin = false;
        for (int i = 0; i < ndevs - 1; i++) if (strstr(devs[i].name, "retrogame")) builtin = true;
        // (with no built-in pad found yet, e.g. on a PC, the first pad is player 1)
        for (int p = builtin || used[0] ? 1 : 0; p < NPORT; p++) if (!used[p] || p == NPORT - 1) { d->port = p; break; }
    }
    for (int a = 0; a < 0x40; a++)
        if (BIT(abs, a) && ioctl(fd, EVIOCGABS(a), &d->ax[a]) == 0) d->has_abs[a] = true;
    bf_log("input: %s (%s)", d->name, path);
}

// pads that come and go (USB, Bluetooth) while a game runs: /dev/input watched with inotify; a new event node is
// opened on the next read (its permissions settle a moment after it appears, so it's retried a few times)
static int ino = -1;
static char pending_dev[4][64];
static int npending, pending_tries;

void bf_input_open(bf_input_hotkey_fn fn)
{
    on_hotkey = fn;
    if (!getenv("BANANAFRONT_INPUT")) {
        ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ino >= 0 && inotify_add_watch(ino, "/dev/input", IN_CREATE) < 0) { close(ino); ino = -1; }
    }
    const char* only = getenv("BANANAFRONT_INPUT");  // (tests: one device)
    if (only) { open_dev(only); return; }
    DIR* dir = opendir("/dev/input");
    if (!dir) return;
    struct dirent* e;
    char names[64][32];
    int n = 0;
    while ((e = readdir(dir)) && n < 64)
        if (!strncmp(e->d_name, "event", 5)) snprintf(names[n++], 32, "%s", e->d_name);
    closedir(dir);
    for (int i = 0; i < n; i++) {
        char p[64];
        snprintf(p, sizeof p, "/dev/input/%s", names[i]);
        open_dev(p);
    }
}

int bf_input_fds(int* fds, int max)
{
    int n = 0;
    if (ino >= 0 && n < max) fds[n++] = ino;
    for (int i = 0; i < ndevs && n < max; i++) fds[n++] = devs[i].fd;
    return n;
}

static int16_t scale_axis(const struct input_absinfo* a, int v)
{
    const int lo = a->minimum, hi = a->maximum;
    if (hi <= lo) return 0;
    const double c = (lo + hi) / 2.0, h = (hi - lo) / 2.0;
    double x = (v - c) / h;
    const double dz = a->flat > 0 ? a->flat / h : 0.0;
    if (x > -dz && x < dz) x = 0;
    if (x > 1) x = 1;
    if (x < -1) x = -1;
    return (int16_t)(x * 32767);
}

static void set_button(int id, bool down)
{
    if (id < 0) return;
    if (down) pbuttons[cur_port] |= 1u << id;
    else pbuttons[cur_port] &= ~(1u << id);
}

static void key_event(int code, int value)
{
    if (value == 2) return;  // (autorepeat)
    if (raw_mode) { raw_push(code, value); return; }
    const bool down = value != 0;
    if (is_hotkey(code)) {
        hotkey_down = down;
        if (down) hotkey_used = false;
        else if (!hotkey_used && on_hotkey) on_hotkey(BF_HK_ALONE, true, true);  // F pressed and let go alone
        return;
    }
    if (code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN) {
        if (hotkey_down && down && on_hotkey) { hotkey_used = true; on_hotkey(code == KEY_VOLUMEUP ? BF_HK_VOLUP : BF_HK_VOLDOWN, true, true); }
        return;
    }
    const int id = map_key(code);
    if (id < 0) return;
    set_button(id, down);
    if (!on_hotkey || cur_port) return;  // (the hotkeys and jobs are player 1's)
    if (hotkey_down && down) {
        hotkey_used = true;
        on_hotkey(id, true, true);
    } else if (down && (job_mask >> id & 1)) on_hotkey(id, true, false);
    else if (!down) on_hotkey(id, false, hotkey_down);
}

static void hotplug_read(void)
{
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t r;
    while ((r = read(ino, buf, sizeof buf)) > 0)
        for (char* p = buf; p < buf + r;) {
            struct inotify_event* e = (struct inotify_event*)p;
            if (e->len && !strncmp(e->name, "event", 5) && npending < 4) {
                snprintf(pending_dev[npending++], sizeof pending_dev[0], "/dev/input/%s", e->name);
                pending_tries = 0;
            }
            p += sizeof *e + e->len;
        }
}

// (called between frames by main: a new pad is opened once it can be)
void bf_input_hotplug_tick(void)
{
    if (!npending) return;
    for (int i = 0; i < npending;) {
        const int before = ndevs;
        int fd = open(pending_dev[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            close(fd);
            open_dev(pending_dev[i]);
            if (ndevs > before) bf_log("input: connected while playing");
            memmove(pending_dev[i], pending_dev[i + 1], sizeof pending_dev[0] * (size_t)(npending - i - 1));
            npending--;
        } else i++;
    }
    if (npending && ++pending_tries > 300) npending = 0;  // (~5 s of tries: not a device we may read)
}

void bf_input_read(int fd)
{
    if (fd == ino) { hotplug_read(); return; }
    for (int i = 0; i < ndevs; i++) {
        if (devs[i].fd != fd) continue;
        dev_t_* d = &devs[i];
        cur_port = d->port;
        struct input_event ev[64];
        ssize_t r;
        while ((r = read(fd, ev, sizeof ev)) > 0) {
            for (size_t k = 0; k < (size_t)r / sizeof *ev; k++) {
                const struct input_event* e = &ev[k];
                if (e->type == EV_KEY) key_event(e->code, e->value);
                else if (e->type == EV_ABS && e->code < 0x40 && d->has_abs[e->code]) {
                    if (raw_mode && (e->code == ABS_HAT0X || e->code == ABS_HAT0Y)) {
                        int* h = e->code == ABS_HAT0X ? &hat_x : &hat_y;
                        const int neg = e->code == ABS_HAT0X ? BTN_DPAD_LEFT : BTN_DPAD_UP, pos = e->code == ABS_HAT0X ? BTN_DPAD_RIGHT : BTN_DPAD_DOWN;
                        if (*h < 0) raw_push(neg, 0);
                        if (*h > 0) raw_push(pos, 0);
                        if (e->value < 0) raw_push(neg, 1);
                        if (e->value > 0) raw_push(pos, 1);
                        *h = e->value;
                    } else if (e->code == ABS_HAT0X || e->code == ABS_HAT0Y) {
                        int* h = e->code == ABS_HAT0X ? &hat_x : &hat_y;
                        const int neg = e->code == ABS_HAT0X ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_UP;
                        const int pos = e->code == ABS_HAT0X ? RETRO_DEVICE_ID_JOYPAD_RIGHT : RETRO_DEVICE_ID_JOYPAD_DOWN;
                        *h = e->value;
                        set_button(neg, e->value < 0);
                        set_button(pos, e->value > 0);
                        if (hotkey_down && e->value && on_hotkey) { hotkey_used = true; on_hotkey(e->value < 0 ? neg : pos, true, true); }
                    } else {
                        const int slot = e->code == ABS_X ? 0 : e->code == ABS_Y ? 1 : e->code == ABS_RX ? 2 : e->code == ABS_RY ? 3 : -1;
                        if (slot >= 0) paxes[d->port][slot] = scale_axis(&d->ax[e->code], e->value);
                    }
                }
            }
        }
        if (r < 0 && errno == ENODEV) {  // unplugged
            close(fd);
            devs[i] = devs[--ndevs];
        }
        return;
    }
}

void bf_input_poll(void)
{
    // while F is held, the game sees no buttons (F + B must not also press B in the game)
    game_buttons[0] = hotkey_down ? 0 : pbuttons[0] & ~job_mask;
    for (int p = 1; p < NPORT; p++) game_buttons[p] = pbuttons[p];
}

int16_t bf_input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port >= NPORT) return 0;
    switch (device) {
    case RETRO_DEVICE_JOYPAD:
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)game_buttons[port];
        return id < 16 ? (game_buttons[port] >> id) & 1 : 0;
    case RETRO_DEVICE_ANALOG:
        if (hotkey_down && port == 0) return 0;
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT && id < 2) return paxes[port][id];
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT && id < 2) return paxes[port][2 + id];
        return 0;
    default: return 0;
    }
}

uint32_t bf_input_buttons(void) { return buttons; }
bool bf_input_hotkey_held(void) { return hotkey_down; }

void bf_input_close(void)
{
    if (ino >= 0) { close(ino); ino = -1; }
    for (int i = 0; i < ndevs; i++) close(devs[i].fd);
    ndevs = 0;
}
