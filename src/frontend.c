// frontend.c: what BananaOS needs around the game.
// - /run/banana/game-id (COMPANION-SPEC.md 2): the game's hashes and paths, written at the start, removed at the end.
// - The companion export (COMPANION-SPEC.md 3, bcomp.c): attached when banana-ui set BANANA_COMPANION; the core's
//   memory maps (or its SYSTEM_RAM at the system's address) as the region table; requests answered once per refresh
//   after the core's frames, the only point where the core's memory is consistent. Not attached: no cost.
// - Screenshots (RetroArch's names: <game>-YYMMDD-HHMMSS.png in /data/screenshots) and state thumbnails
//   (<state>.png, as RetroArch's savestate_thumbnail): the core's last picture at its own size.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "bf.h"
#include "frontend.h"
#include "memmap.h"
#include "ra.h"
#include "third_party/rcheevos/include/rc_hash.h"
#include "third_party/bcomp/bcomp.h"
#include "third_party/bcomp/bcomp_libretro.h"
#include "third_party/rcheevos/src/rhash/md5.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBIW_ZLIB_COMPRESS bf_stbiw_zlib  // zlib's deflate: smaller and faster than stb's own
#include <stddef.h>
static unsigned char* bf_stbiw_zlib(unsigned char* data, int len, int* out_len, int quality);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "third_party/stb/stb_image_write.h"
#pragma GCC diagnostic pop

static unsigned char* bf_stbiw_zlib(unsigned char* data, int len, int* out_len, int quality)
{
    (void)quality;
    uLongf n = compressBound((uLong)len);
    unsigned char* o = STBIW_MALLOC(n);
    if (!o || compress2(o, &n, data, (uLong)len, 6) != Z_OK) { STBIW_FREE(o); return NULL; }
    *out_len = (int)n;
    return o;
}

static const char* sys_id = "";
static bcomp_producer comp;
static bool comp_on;
static uint64_t comp_frames;
static const char* GAME_ID = "/run/banana/game-id";
static time_t started;

// the system's RAM where its companion map places it, for a core that gives no memory maps (systems/<id>.json)
static uint32_t fallback_ram_start(const char* s)
{
    if (!strcmp(s, "gba")) return 0x02000000;
    if (!strcmp(s, "gb") || !strcmp(s, "gbc")) return 0xC000;
    if (!strcmp(s, "snes")) return 0x7E0000;
    if (!strcmp(s, "megadrive") || !strcmp(s, "sms")) return 0xFF0000;
    return 0;
}
static uint32_t fallback_sram_start(const char* s)
{
    if (!strcmp(s, "gba")) return 0x0E000000;
    if (!strcmp(s, "gb") || !strcmp(s, "gbc")) return 0xA000;
    if (!strcmp(s, "nes")) return 0x6000;
    return 0x10000000;
}
static uint32_t mem_size(unsigned id) { return (uint32_t)g_core.get_memory_size(id); }

static char gid_rom[1100], ra_hash_s[40], ra_title_s[256];
static unsigned ra_id;
static char crc_s[16], md5_s[40];
static size_t rom_n;

static void write_game_id(const char* rom);
void frontend_ra_game(unsigned id, const char* title)
{
    ra_id = id;
    snprintf(ra_title_s, sizeof ra_title_s, "%s", title ? title : "");
    write_game_id(NULL);
}

static void hash_content(void)
{
    // the hashes of the content the core runs (a zip's member, not the zip)
    const void* d = g_core.rom_data;
    size_t n = g_core.rom_size;
    void* own = NULL;
    if (!d) { own = bf_read_file(g_core.rom_path, &n); d = own; }
    char* crc = crc_s;
    char* md5s = md5_s;
    rom_n = n;
    if (d) {
        snprintf(crc, sizeof crc_s, "%08lx", crc32(0, d, (uInt)n));
        md5_state_t m;
        md5_byte_t dig[16];
        md5_init(&m);
        md5_append(&m, d, (int)n);
        md5_finish(&m, dig);
        for (int i = 0; i < 16; i++) sprintf(md5s + i * 2, "%02x", dig[i]);
        // rcheevos' hash for the system (header rules differ per system: NES skips iNES, SNES a copier header...)
        extern uint32_t ra_console_of(const char* system);
        uint32_t con = ra_console_of(sys_id);
        if (con && !rc_hash_generate_from_buffer(ra_hash_s, con, d, n)) ra_hash_s[0] = 0;
    }
    free(own);
}

static void write_game_id(const char* rom)
{
    if (rom) snprintf(gid_rom, sizeof gid_rom, "%s", rom);
    rom = gid_rom;
    const size_t n = rom_n;
    const char* crc = crc_s;
    const char* md5s = md5_s;
    const char* member = strcmp(g_core.rom_path, rom) ? strrchr(g_core.rom_path, '/') : NULL;
    char b[4096];
    int o = snprintf(b, sizeof b,
                     "version=1\nsystem=%s\ncore=%s %s\nfrontend=BananaFront %s\npid=%d\nrom=%s\nrom_member=%s\n"
                     "rom_base=%s\nrom_size=%zu\ncrc32=%s\nmd5=%s\nra_hash=%s\nra_game_id=%u\nra_title=%s\nsave_dir=%s\n"
                     "state_dir=%s\nscreenshot_dir=%s\nstarted=%ld\n",
                     sys_id, g_core.sys.library_name, g_core.sys.library_version, BF_VERSION, (int)getpid(), rom,
                     member ? member + 1 : "", g_paths.game, n, crc, md5s, ra_hash_s, ra_id, ra_title_s, g_paths.save_dir,
                     g_paths.state_dir, g_paths.shot_dir, (long)started);
    (void)o;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.tmp", GAME_ID);
    FILE* f = fopen(tmp, "w");
    if (!f) return;  // (no /run/banana on a PC)
    fputs(b, f);
    fclose(f);
    rename(tmp, GAME_ID);
}

static void comp_regions(void)
{
    bcomp_clear_regions(&comp);
    int n = 0;
    if (g_core.mmap_n) {
        struct retro_memory_map mm = { g_core.mmap, g_core.mmap_n };
        n = bcomp_add_retro_map(&comp, &mm);
    }
    if (!n)
        bcomp_add_retro_fallback(&comp, fallback_ram_start(sys_id), fallback_sram_start(sys_id), mem_size,
                                 g_core.get_memory_data);
    bcomp_publish(&comp);
    bf_log("companion: attached, %d regions%s", comp.n, n ? "" : " (the core's RAM, no memory map)");
}

void frontend_init(const char* system, const char* rom)
{
    sys_id = system ? system : "";
    started = time(NULL);
    hash_content();
    write_game_id(rom);
    comp_on = bcomp_attach(&comp, NULL) == 0;
    if (comp_on) {
        char core[128];
        snprintf(core, sizeof core, "%s %s", g_core.sys.library_name, g_core.sys.library_version);
        bcomp_set_info(&comp, sys_id, core, "BananaFront " BF_VERSION);
        comp_regions();
        bcomp_set_state(&comp, 2, (uint32_t)(bf_core_fps() * 100));
    }
    ra_init(sys_id, comp_on ? &comp : NULL);
}

void frontend_core_frame(void) { ra_frame(); }
void frontend_paused(void)
{
    ra_idle();
    if (comp_on) bcomp_service(&comp, comp_frames);
}

void frontend_mmap_changed(void)
{
    if (comp_on) comp_regions();
    ra_memory_changed();
}

void frontend_frame(void)
{
    comp_frames++;
    if (comp_on) bcomp_service(&comp, comp_frames);
}

void frontend_quit(void)
{
    ra_quit();
    if (comp_on) { bcomp_set_state(&comp, 4, 0); bcomp_detach(&comp); }
    unlink(GAME_ID);
}

static bool write_png(const char* path, const uint32_t* px, unsigned w, unsigned h)
{
    // XRGB8888 -> RGB
    unsigned char* rgb = malloc((size_t)w * h * 3);
    if (!rgb) return false;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3] = px[i] >> 16;
        rgb[i * 3 + 1] = px[i] >> 8;
        rgb[i * 3 + 2] = px[i];
    }
    int len = 0;
    unsigned char* png = stbi_write_png_to_mem(rgb, (int)w * 3, (int)w, (int)h, 3, &len);
    free(rgb);
    bool ok = png && bf_write_file(path, png, (size_t)len);
    STBIW_FREE(png);
    return ok;
}

void bf_thumbnail_write(const char* state_path)
{
    unsigned w, h;
    uint32_t* px = bf_frame_rgb(&w, &h);
    if (!px) return;
    char p[1700];
    snprintf(p, sizeof p, "%s.png", state_path);
    write_png(p, px, w, h);
    free(px);
}

void frontend_screenshot(void)
{
    unsigned w, h;
    uint32_t* px = bf_frame_rgb(&w, &h);
    if (!px) return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char p[1700];
    snprintf(p, sizeof p, "%s/%s-%02d%02d%02d-%02d%02d%02d.png", g_paths.shot_dir, g_paths.game, tm.tm_year % 100,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    bf_mkdirs(g_paths.shot_dir);
    const bool ok = write_png(p, px, w, h);
    free(px);
    bf_log("screenshot %s: %s", ok ? "saved" : "NOT saved", p);
    frontend_event(BF_EV_SHOT, ok);
}

__attribute__((weak)) void bf_menu_open(void) { bf_log("menu: not built in"); }
void frontend_menu(void) { bf_menu_open(); }

static bool show_fps;
void frontend_fps_toggle(void) { show_fps = !show_fps; bf_log("fps counter %s", show_fps ? "on" : "off"); }

__attribute__((weak)) void bf_popup(int ev, int arg) { (void)ev; (void)arg; }
void frontend_event(int ev, int arg)
{
    if (ev == BF_EV_SLOT) bf_log("state slot %d", arg);
    bf_popup(ev, arg);
}
