// core.c: the libretro core: loaded with dlopen, its environment calls answered, its options (RetroArch's .opt files
// read, never written: RetroArch stays the fallback with the same settings), the content loaded (a zip's member
// unpacked when the core can't read zips itself).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "bf.h"
#include "unzip.h"

__attribute__((weak)) void bf_mmap_changed(void) {}

bf_core g_core;

// paths the core asks for (set by main before load)
const char* bf_env_system_dir = "/data/bios";
const char* bf_env_save_dir = "/data/saves";
const char* bf_env_username = "Banana";
unsigned bf_env_language = RETRO_LANGUAGE_ENGLISH;
bool bf_env_fastforward = false;

// ---- options -----------------------------------------------------------------------------------------------------
typedef struct {
    char* key;
    char* val;       // current
    char* def;       // the core's default
    char** values;   // allowed values (may be empty: free text)
    int nvalues;
} opt_t;
static opt_t* opts;
static int nopts;

static opt_t* opt_find(const char* key)
{
    for (int i = 0; i < nopts; i++)
        if (!strcmp(opts[i].key, key)) return &opts[i];
    return NULL;
}

static opt_t* opt_add(const char* key)
{
    opt_t* o = opt_find(key);
    if (o) return o;
    opts = realloc(opts, sizeof *opts * (nopts + 1));
    o = &opts[nopts++];
    memset(o, 0, sizeof *o);
    o->key = strdup(key);
    return o;
}

static void opt_values_add(opt_t* o, const char* v)
{
    o->values = realloc(o->values, sizeof *o->values * (o->nvalues + 1));
    o->values[o->nvalues++] = strdup(v);
}

// SET_VARIABLES: "Description; first|second|third", the first is the default
static void opts_from_variables(const struct retro_variable* v)
{
    for (; v && v->key; v++) {
        opt_t* o = opt_add(v->key);
        const char* s = v->value ? strchr(v->value, ';') : NULL;
        if (!s) continue;
        s++;
        while (*s == ' ') s++;
        char* list = strdup(s);
        for (char *save = NULL, *t = strtok_r(list, "|", &save); t; t = strtok_r(NULL, "|", &save)) opt_values_add(o, t);
        free(list);
        if (o->nvalues) { free(o->def); o->def = strdup(o->values[0]); }
    }
}

static void opts_from_defs(const char* key, const struct retro_core_option_value* values, const char* def)
{
    opt_t* o = opt_add(key);
    for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && values[i].value; i++) opt_values_add(o, values[i].value);
    free(o->def);
    o->def = strdup(def ? def : (o->nvalues ? o->values[0] : ""));
}

static void opts_from_v1(const struct retro_core_option_definition* d)
{
    for (; d && d->key; d++) opts_from_defs(d->key, d->values, d->default_value);
}

static void opts_from_v2(const struct retro_core_options_v2* v2)
{
    if (!v2) return;
    for (const struct retro_core_option_v2_definition* d = v2->definitions; d && d->key; d++)
        opts_from_defs(d->key, d->values, d->default_value);
}

static void opts_reset_to_defaults(void)
{
    for (int i = 0; i < nopts; i++)
        if (!opts[i].val && opts[i].def) opts[i].val = strdup(opts[i].def);
}

void bf_core_options_load(const char* config_dir, const char* game)
{
    // RetroArch's per-core file, then the game's own (game_specific_options): the last wins
    const char* lib = g_core.sys.library_name ? g_core.sys.library_name : "core";
    char p[1600];
    bf_conf c = { 0 };
    snprintf(p, sizeof p, "%s/%s/%s.opt", config_dir, lib, lib);
    bool a = bf_conf_load(&c, p);
    snprintf(p, sizeof p, "%s/%s/%s.opt", config_dir, lib, game);
    bool b = bf_conf_load(&c, p);
    // BananaFront's own overrides (e.g. a stress test's), never RetroArch's files
    const char* extra = getenv("BANANAFRONT_OPTIONS");
    bool e = extra && bf_conf_load(&c, extra);
    int applied = 0;
    for (int i = 0; i < c.n; i++) {
        opt_t* o = opt_find(c.v[i].key);
        if (!o) continue;  // (an option this core version doesn't have)
        free(o->val);
        o->val = strdup(c.v[i].val);
        applied++;
    }
    if (a || b || e) bf_log("core options: %d from %s%s%s", applied, a ? "the core's .opt " : "", b ? "the game's .opt " : "",
                            e ? "BANANAFRONT_OPTIONS" : "");
    bf_conf_free(&c);
    g_core.options_changed = true;
}

const char* bf_core_option(const char* key)
{
    opt_t* o = opt_find(key);
    return o ? (o->val ? o->val : o->def) : NULL;
}

void bf_core_option_set(const char* key, const char* val)
{
    opt_t* o = opt_find(key);
    if (!o) return;
    free(o->val);
    o->val = strdup(val);
    g_core.options_changed = true;
}

// ---- environment ---------------------------------------------------------------------------------------------------

static void core_log(enum retro_log_level level, const char* fmt, ...)
{
    if (level < RETRO_LOG_INFO && !getenv("BANANAFRONT_DEBUG")) return;
    char b[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    static const char* L[] = { "debug", "info", "warn", "error" };
    bf_log("[core %s] %s", L[level & 3], b);
}

static retro_time_t perf_usec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (retro_time_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static retro_perf_tick_t perf_counter(void) { return (retro_perf_tick_t)perf_usec(); }
static uint64_t perf_cpu_features(void) { return RETRO_SIMD_NEON | RETRO_SIMD_ASIMD; }
static void perf_log(void) {}
static void perf_register(struct retro_perf_counter* c) { c->registered = true; }
static void perf_start(struct retro_perf_counter* c) { c->start = perf_counter(); }
static void perf_stop(struct retro_perf_counter* c) { c->total += perf_counter() - c->start; c->call_cnt++; }

static void copy_mmap(const struct retro_memory_map* m)
{
    free(g_core.mmap);
    g_core.mmap = NULL;
    g_core.mmap_n = 0;
    if (!m || !m->num_descriptors) return;
    g_core.mmap = malloc(sizeof *g_core.mmap * m->num_descriptors);
    memcpy(g_core.mmap, m->descriptors, sizeof *g_core.mmap * m->num_descriptors);
    g_core.mmap_n = m->num_descriptors;
}

// hooks other modules provide (weak: a test build may leave them out)
__attribute__((weak)) bool bf_hw_render_request(struct retro_hw_render_callback* cb) { (void)cb; return false; }

static bool environment(unsigned cmd, void* data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = bf_env_system_dir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data = bf_env_save_dir; return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY: *(const char**)data = bf_env_system_dir; return true;
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH: return false;
    case RETRO_ENVIRONMENT_GET_USERNAME: *(const char**)data = bf_env_username; return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *(unsigned*)data = bf_env_language; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(const enum retro_pixel_format*)data;
        if (f > RETRO_PIXEL_FORMAT_RGB565) return false;
        g_core.fmt = f;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_ROTATION: g_core.rotation = *(const unsigned*)data & 3; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
        *(uint64_t*)data = (1 << RETRO_DEVICE_JOYPAD) | (1 << RETRO_DEVICE_ANALOG);
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
        struct retro_perf_callback* p = data;
        p->get_time_usec = perf_usec;
        p->get_cpu_features = perf_cpu_features;
        p->get_perf_counter = perf_counter;
        p->perf_register = perf_register;
        p->perf_start = perf_start;
        p->perf_stop = perf_stop;
        p->perf_log = perf_log;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES: opts_from_variables(data); opts_reset_to_defaults(); return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned*)data = 2; return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: opts_from_v1(data); opts_reset_to_defaults(); return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
        opts_from_v1(((const struct retro_core_options_intl*)data)->us);
        opts_reset_to_defaults();
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: opts_from_v2(data); opts_reset_to_defaults(); return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
        opts_from_v2(((const struct retro_core_options_v2_intl*)data)->us);
        opts_reset_to_defaults();
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable* v = data;
        v->value = v->key ? bf_core_option(v->key) : NULL;
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLE: {
        const struct retro_variable* v = data;
        if (!v) return true;  // (a query: supported)
        if (v->key && v->value) bf_core_option_set(v->key, v->value);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool*)data = g_core.options_changed;
        g_core.options_changed = false;
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: return true;
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL: return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: return true;
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO: return true;
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE: return true;
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS: g_core.achievements = *(const bool*)data; return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: copy_mmap(data); bf_mmap_changed(); return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        g_core.av.geometry = *(const struct retro_game_geometry*)data;
        g_core.av_changed = true;
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        g_core.av = *(const struct retro_system_av_info*)data;
        g_core.av_changed = true;
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *(int*)data = 3; return true;  // video and audio wanted
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING: *(bool*)data = bf_env_fastforward; return true;
    case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE: *(float*)data = 60.0f; return true;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION: *(unsigned*)data = 1; return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: bf_log("[core message] %s", ((const struct retro_message*)data)->msg); return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
        const struct retro_message_ext* m = data;
        if (m->target != RETRO_MESSAGE_TARGET_OSD) bf_log("[core message] %s", m->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
        // GLES3 contexts (N64/Dreamcast cores) come later: hw.c registers bf_hw_render_request then
        return bf_hw_render_request(data);
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: return false;
    case RETRO_ENVIRONMENT_GET_JIT_CAPABLE: *(bool*)data = true; return true;
    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT: *(int*)data = RETRO_SAVESTATE_CONTEXT_NORMAL; return true;
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY: return true;
    case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE: return false;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: return false;
    case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: return false;
    case RETRO_ENVIRONMENT_GET_LED_INTERFACE: return false;
    case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER: return false;
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT: return false;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: return true;
    case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: return false;
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: return false;
    case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
        struct retro_throttle_state* t = data;
        t->mode = bf_env_fastforward ? RETRO_THROTTLE_FAST_FORWARD : RETRO_THROTTLE_VSYNC;
        t->rate = (float)bf_core_fps();
        return true;
    }
    default:
        // (a command sent with the experimental bit that this frontend knows without it, as RetroArch answers it)
        if (cmd & RETRO_ENVIRONMENT_EXPERIMENTAL) return environment(cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL, data);
        if (getenv("BANANAFRONT_DEBUG")) bf_log("environment %u not answered", cmd);
        return false;
    }
}

static void audio_sample(int16_t l, int16_t r)
{
    int16_t s[2] = { l, r };
    bf_audio_batch(s, 1);
}

// ---- loading -----------------------------------------------------------------------------------------------------

#define SYM(n) do { *(void**)&g_core.n = dlsym(g_core.dl, "retro_" #n); if (!g_core.n) { bf_log("core: no retro_" #n); return false; } } while (0)

bool bf_core_open(const char* so)
{
    memset(&g_core, 0, sizeof g_core);
    g_core.fmt = RETRO_PIXEL_FORMAT_0RGB1555;
    g_core.dl = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!g_core.dl) { bf_log("core: %s", dlerror()); return false; }
    SYM(init); SYM(deinit); SYM(api_version); SYM(get_system_info); SYM(get_system_av_info); SYM(set_environment);
    SYM(set_video_refresh); SYM(set_audio_sample); SYM(set_audio_sample_batch); SYM(set_input_poll);
    SYM(set_input_state); SYM(set_controller_port_device); SYM(reset); SYM(run); SYM(serialize_size); SYM(serialize);
    SYM(unserialize); SYM(load_game); SYM(unload_game); SYM(get_memory_data); SYM(get_memory_size);
    if (g_core.api_version() != RETRO_API_VERSION) { bf_log("core: API version %u", g_core.api_version()); return false; }
    g_core.get_system_info(&g_core.sys);
    g_core.set_environment(environment);
    g_core.set_video_refresh((retro_video_refresh_t)bf_video_frame);
    g_core.set_audio_sample(audio_sample);
    g_core.set_audio_sample_batch(bf_audio_batch);
    g_core.set_input_poll(bf_input_poll);
    g_core.set_input_state(bf_input_state);
    g_core.init();
    bf_log("core %s %s (%s)", g_core.sys.library_name, g_core.sys.library_version, so);
    return true;
}

static bool ext_in(const char* name, const char* list)
{
    const char* dot = strrchr(name, '.');
    if (!dot || !list) return false;
    char l[512];
    snprintf(l, sizeof l, "%s", list);
    for (char *save = NULL, *t = strtok_r(l, "|", &save); t; t = strtok_r(NULL, "|", &save))
        if (!strcasecmp(t, dot + 1)) return true;
    return false;
}

static char tmp_rom[1100];

bool bf_core_load_game(const char* rom)
{
    struct retro_game_info gi = { 0 };
    snprintf(g_core.rom_path, sizeof g_core.rom_path, "%s", rom);
    const char* exts = g_core.sys.valid_extensions;
    const bool zip = strlen(rom) > 4 && !strcasecmp(rom + strlen(rom) - 4, ".zip");
    if (zip && !ext_in(rom, exts)) {
        // the first member with an extension the core takes, as RetroArch picks it
        char member[512];
        size_t n = 0;
        void* d = bf_unzip_first(rom, exts, member, sizeof member, &n);
        if (!d) { bf_log("no %s file in %s", exts ? exts : "", rom); return false; }
        bf_log("content: %s from the zip (%zu bytes)", member, n);
        if (g_core.sys.need_fullpath) {
            // (RetroArch's cache_directory is /tmp too)
            const char* slash = strrchr(member, '/');
            snprintf(tmp_rom, sizeof tmp_rom, "/tmp/bananafront-%d-%s", (int)getpid(), slash ? slash + 1 : member);
            if (!bf_write_file(tmp_rom, d, n)) { free(d); bf_log("can't unpack to %s", tmp_rom); return false; }
            free(d);
            snprintf(g_core.rom_path, sizeof g_core.rom_path, "%s", tmp_rom);
        } else {
            g_core.rom_data = d;
            g_core.rom_size = n;
            // a path with the member's name: cores look at its extension
            const char* slash = strrchr(member, '/');
            char dir[1024];
            snprintf(dir, sizeof dir, "%s", rom);
            char* ds = strrchr(dir, '/');
            if (ds) *ds = 0;
            snprintf(g_core.rom_path, sizeof g_core.rom_path, "%s/%s", dir, slash ? slash + 1 : member);
        }
    } else if (!g_core.sys.need_fullpath) {
        g_core.rom_data = bf_read_file(rom, &g_core.rom_size);
        if (!g_core.rom_data) { bf_log("can't read %s", rom); return false; }
    }
    gi.path = g_core.rom_path;
    gi.data = g_core.rom_data;
    gi.size = g_core.rom_size;
    if (!g_core.load_game(&gi)) { bf_log("the core didn't load %s", rom); return false; }
    g_core.get_system_av_info(&g_core.av);
    g_core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    bf_log("game: %ux%u (max %ux%u, aspect %.3f), %.3f fps, %.0f Hz sound, pixel format %d", g_core.av.geometry.base_width,
           g_core.av.geometry.base_height, g_core.av.geometry.max_width, g_core.av.geometry.max_height,
           g_core.av.geometry.aspect_ratio, g_core.av.timing.fps, g_core.av.timing.sample_rate, g_core.fmt);
    return true;
}

void bf_core_close(void)
{
    if (!g_core.dl) return;
    g_core.unload_game();
    g_core.deinit();
    // (no dlclose: some cores' static destructors run after their deinit and expect the library still mapped)
    free(g_core.rom_data);
    free(g_core.mmap);
    if (tmp_rom[0]) unlink(tmp_rom);
}

double bf_core_fps(void) { return g_core.av.timing.fps > 1 ? g_core.av.timing.fps : 60.0; }
