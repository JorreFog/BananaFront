// util.c: the log, key = value files, paths (RetroArch's names for saves and states), whole-file I/O.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bf.h"

double bf_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

void bf_log(const char* fmt, ...)
{
    char b[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    size_t n = strlen(b);
    while (n && b[n - 1] == '\n') b[--n] = 0;
    fprintf(stderr, "bananafront: %s\n", b);
}

// ---- key = value -----------------------------------------------------------------------------------------------

static char* trim(char* s)
{
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

// one line "key = value" / "key = \"value\"" / "key=value": false if it isn't one
static bool parse_line(char* ln, char** k, char** v)
{
    char* s = trim(ln);
    if (!*s || *s == '#' || *s == ';' || *s == '[') return false;
    char* eq = strchr(s, '=');
    if (!eq) return false;
    *eq = 0;
    *k = trim(s);
    char* val = trim(eq + 1);
    size_t n = strlen(val);
    if (n >= 2 && val[0] == '"' && val[n - 1] == '"') { val[n - 1] = 0; val++; }
    *v = val;
    return **k != 0;
}

void bf_conf_set(bf_conf* c, const char* key, const char* val)
{
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->v[i].key, key)) { free(c->v[i].val); c->v[i].val = strdup(val); return; }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->v = realloc(c->v, sizeof *c->v * c->cap);
    }
    c->v[c->n].key = strdup(key);
    c->v[c->n].val = strdup(val);
    c->n++;
}

bool bf_conf_load(bf_conf* c, const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char ln[4096], *k, *v;
    while (fgets(ln, sizeof ln, f))
        if (parse_line(ln, &k, &v)) bf_conf_set(c, k, v);
    fclose(f);
    return true;
}

const char* bf_conf_get(const bf_conf* c, const char* key)
{
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->v[i].key, key)) return c->v[i].val;
    return NULL;
}

void bf_conf_free(bf_conf* c)
{
    for (int i = 0; i < c->n; i++) { free(c->v[i].key); free(c->v[i].val); }
    free(c->v);
    memset(c, 0, sizeof *c);
}

bool bf_conf_file_get(const char* path, const char* key, char* out, size_t n)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char ln[4096], *k, *v;
    bool found = false;
    while (fgets(ln, sizeof ln, f))
        if (parse_line(ln, &k, &v) && !strcmp(k, key)) { snprintf(out, n, "%s", v); found = true; }
    fclose(f);
    return found;
}

bool bf_system_setting(const char* data, const char* etc, const char* key, const char* system, char* out, size_t n)
{
    char k[128];
    if (system && *system) {
        snprintf(k, sizeof k, "%s.%s", key, system);
        if (bf_conf_file_get(data, k, out, n) || bf_conf_file_get(etc, k, out, n)) return true;
    }
    // (then the plain key: a system with no page of its own, PICO-8, keeps what a card from before #151 said)
    return bf_conf_file_get(data, key, out, n) || bf_conf_file_get(etc, key, out, n);
}

int bf_swap_ab(int id)
{
    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_A: return RETRO_DEVICE_ID_JOYPAD_B;
    case RETRO_DEVICE_ID_JOYPAD_B: return RETRO_DEVICE_ID_JOYPAD_A;
    case RETRO_DEVICE_ID_JOYPAD_X: return RETRO_DEVICE_ID_JOYPAD_Y;
    case RETRO_DEVICE_ID_JOYPAD_Y: return RETRO_DEVICE_ID_JOYPAD_X;
    default: return id;
    }
}

uint32_t bf_stick_dpad(int x, int y)
{
    const int t = 16384;  // (half a tilt: RetroArch's input_axis_threshold 0.5)
    uint32_t b = 0;
    if (x >= t) b |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (x <= -t) b |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (y >= t) b |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (y <= -t) b |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    return b;
}

// ---- paths -----------------------------------------------------------------------------------------------------

// RetroArch (runloop_path_set_basename): the content path's file name without its last extension; a zip's own name
// (it is given the archive's path), not its member's
void bf_game_base(const char* rom, char* out, size_t n)
{
    const char* b = strrchr(rom, '/');
    b = b ? b + 1 : rom;
    snprintf(out, n, "%s", b);
    char* dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

void bf_paths_init(bf_paths* p, const char* rom, const char* library_name, const char* saves_root,
                   const char* states_root, const char* shots_root, const char* system_dir, const char* shared_save_dir)
{
    memset(p, 0, sizeof *p);
    bf_game_base(rom, p->game, sizeof p->game);
    // (sort_savefiles_enable / sort_savestates_enable: a folder per core, named by its library_name)
    if (shared_save_dir && *shared_save_dir) snprintf(p->save_dir, sizeof p->save_dir, "%s", shared_save_dir);
    else snprintf(p->save_dir, sizeof p->save_dir, "%s/%s", saves_root, library_name);
    snprintf(p->state_dir, sizeof p->state_dir, "%s/%s", states_root, library_name);
    snprintf(p->shot_dir, sizeof p->shot_dir, "%s", shots_root);
    snprintf(p->system_dir, sizeof p->system_dir, "%s", system_dir);
}

void bf_path_save(const bf_paths* p, const char* ext, char* out, size_t n)
{
    snprintf(out, n, "%s/%s%s", p->save_dir, p->game, ext);
}

void bf_path_state(const bf_paths* p, int slot, char* out, size_t n)
{
    if (slot == -2) snprintf(out, n, "%s/%s.state.undo", p->state_dir, p->game);  // (the menu's undo of a load)
    else if (slot < 0) snprintf(out, n, "%s/%s.state.auto", p->state_dir, p->game);
    else if (slot == 0) snprintf(out, n, "%s/%s.state", p->state_dir, p->game);
    else snprintf(out, n, "%s/%s.state%d", p->state_dir, p->game, slot);
}

bool bf_mkdirs(const char* dir)
{
    char b[1024];
    snprintf(b, sizeof b, "%s", dir);
    for (char* s = b + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(b, 0755) && errno != EEXIST) return false;
            *s = '/';
        }
    return mkdir(b, 0755) == 0 || errno == EEXIST;
}

bool bf_write_file(const char* path, const void* data, size_t size)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.bf-new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const char* s = data;
    size_t left = size;
    while (left) {
        ssize_t w = write(fd, s, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); unlink(tmp); return false; }
        s += w;
        left -= (size_t)w;
    }
    if (fsync(fd) || close(fd)) { unlink(tmp); return false; }
    if (rename(tmp, path)) { unlink(tmp); return false; }
    return true;
}

void* bf_read_file(const char* path, size_t* size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 0) { close(fd); return NULL; }
    size_t n = (size_t)st.st_size;
    char* b = malloc(n ? n : 1);
    size_t got = 0;
    while (b && got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (!b || got != n) { free(b); return NULL; }
    *size = n;
    return b;
}
