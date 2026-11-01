// bf.h: BananaFront, the BananaOS frontend that hosts libretro cores (RetroArch's job, nothing more).
// Shared declarations of the modules; each .c says what it does at its top.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libretro.h"

#ifndef BF_VERSION
#define BF_VERSION "0.1-dev"
#endif

// ---- log.c -------------------------------------------------------------------------------------------------------
void bf_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
double bf_now_ms(void);  // CLOCK_MONOTONIC

// ---- conf.c: key = value files (RetroArch's .cfg/.opt with quotes, Banana's settings.conf without) ---------------
typedef struct bf_kv { char* key; char* val; } bf_kv;
typedef struct { bf_kv* v; int n, cap; } bf_conf;
bool bf_conf_load(bf_conf* c, const char* path);       // adds/overrides; false: no file
const char* bf_conf_get(const bf_conf* c, const char* key);  // the last value, or NULL
void bf_conf_set(bf_conf* c, const char* key, const char* val);
void bf_conf_free(bf_conf* c);
// a single key from a file (the last line that sets it), into out; false: none
bool bf_conf_file_get(const char* path, const char* key, char* out, size_t n);

// ---- paths.c: where saves, states, screenshots go (RetroArch's names, so today's files keep working) ----------
typedef struct {
    char game[512];        // the content's base name: the ROM's file name without its extension (RetroArch's rule)
    char save_dir[1024];   // e.g. /data/saves/Gambatte (/data/saves/gba for every GBA core)
    char state_dir[1024];  // e.g. /data/states/gpSP
    char shot_dir[1024];   // /data/screenshots
    char system_dir[1024]; // /data/bios
} bf_paths;
// saves_root/<library_name> unless shared_save_dir is set (run-retroarch's /data/saves/gba)
void bf_paths_init(bf_paths* p, const char* rom, const char* library_name, const char* saves_root,
                   const char* states_root, const char* shots_root, const char* system_dir, const char* shared_save_dir);
// "<save_dir>/<game><ext>", ext ".srm" or ".rtc"
void bf_path_save(const bf_paths* p, const char* ext, char* out, size_t n);
// RetroArch's state names: slot 0 ".state", n ".state<n>", -1 the automatic ".state.auto"
void bf_path_state(const bf_paths* p, int slot, char* out, size_t n);
void bf_game_base(const char* rom, char* out, size_t n);
bool bf_mkdirs(const char* dir);  // mkdir -p
// write a whole file atomically (tmp + fsync + rename); false on any error, the old file kept
bool bf_write_file(const char* path, const void* data, size_t size);
// read a whole file (malloc'd, *size set); NULL on error
void* bf_read_file(const char* path, size_t* size);

// ---- resample.c: cubic resampler, int16 stereo, the ratio changeable every call (dynamic rate control) --------
typedef struct {
    double pos;        // fractional read position in the history
    int16_t hist[4][2];  // the last 4 input frames
    int primed;
} bf_resampler;
void bf_resampler_reset(bf_resampler* r);
// in frames -> out frames at ratio = out_rate / in_rate; returns frames written (<= out_max)
size_t bf_resample(bf_resampler* r, const int16_t* in, size_t in_frames, int16_t* out, size_t out_max, double ratio);
// RetroArch's dynamic rate control: the ratio nudged by how full the device buffer is (fill 0..1, 0.5 = target)
double bf_drc_ratio(double base_ratio, double fill, double max_delta);

// ---- statefile.c: RetroArch's savestate container (RASTATE blocks, optional RZIP compression) ----------------
// wrap core data into RetroArch's format (compressed: #RZIPv1#), malloc'd result; NULL on error
void* bf_state_pack(const void* core, size_t core_size, bool compress, size_t* out_size);
// a state file's bytes (RZIP or not, RASTATE or raw core data) -> the core's data, malloc'd; NULL if malformed
void* bf_state_unpack(const void* file, size_t file_size, size_t* core_size);

// ---- core.c: the libretro core -----------------------------------------------------------------------------------
typedef struct {
    void* dl;
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info*);
    void (*get_system_av_info)(struct retro_system_av_info*);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void*, size_t);
    bool (*unserialize)(const void*, size_t);
    bool (*load_game)(const struct retro_game_info*);
    void (*unload_game)(void);
    void* (*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
    struct retro_system_info sys;
    struct retro_system_av_info av;
    enum retro_pixel_format fmt;
    unsigned rotation;
    struct retro_memory_descriptor* mmap;  // SET_MEMORY_MAPS, copied
    unsigned mmap_n;
    bool achievements;    // SET_SUPPORT_ACHIEVEMENTS
    bool av_changed;      // SET_SYSTEM_AV_INFO / SET_GEOMETRY since the frontend last looked
    bool options_changed; // GET_VARIABLE_UPDATE answers true once
    void* rom_data;       // the content in memory (need_fullpath false), kept for the core's lifetime
    size_t rom_size;
    char rom_path[1024];  // the file the core loaded (a zip's member is unpacked first when the core can't read zips)
} bf_core;

extern bf_core g_core;
bool bf_core_open(const char* so_path);
// core options: defaults from the core, then the .opt files (RetroArch's: config/<lib>/<lib>.opt, then the game's)
void bf_core_options_load(const char* config_dir, const char* game);
bool bf_core_load_game(const char* rom);
void bf_core_close(void);
double bf_core_fps(void);

// ---- frontend callbacks the core calls (main.c / the drivers) -----------------------------------------------------
void bf_video_frame(const void* data, unsigned w, unsigned h, size_t pitch);
size_t bf_audio_batch(const int16_t* data, size_t frames);
void bf_input_poll(void);
int16_t bf_input_state(unsigned port, unsigned device, unsigned index, unsigned id);
