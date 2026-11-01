// statefile.c: RetroArch 1.22's savestate files, so a state saved in RetroArch loads here and the other way round.
//   file  = "#RZIPv\1#" u32 chunk_size u64 total_size { u32 zsize, zlib data }*   (savestate_file_compression, on)
//         | the payload as is
//   payload = "RASTATE\1" { char id[4], u32 size, data, zero padding to 8 bytes }* "END " block
//           | the core's raw data (states from older RetroArch)
// The core's data is the "MEM " block. Other blocks (ACHV, RPLY) are skipped on load and not written.
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "bf.h"

static const char RZIP[8] = { '#', 'R', 'Z', 'I', 'P', 'v', 1, '#' };
static const char RAST[8] = { 'R', 'A', 'S', 'T', 'A', 'T', 'E', 1 };
#define RZIP_CHUNK (128 * 1024)

static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t* p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

void* bf_state_pack(const void* core, size_t core_size, bool compress, size_t* out_size)
{
    const size_t pad = (8 - core_size % 8) % 8;
    const size_t psize = 8 + 8 + core_size + pad + 8;
    uint8_t* p = calloc(1, psize);
    if (!p) return NULL;
    memcpy(p, RAST, 8);
    memcpy(p + 8, "MEM ", 4);
    put32(p + 12, (uint32_t)core_size);
    memcpy(p + 16, core, core_size);
    memcpy(p + 16 + core_size + pad, "END ", 4);
    if (!compress) { *out_size = psize; return p; }
    // RZIP: 128 KiB chunks, each deflated on its own (RetroArch's rzipstream)
    const size_t chunks = (psize + RZIP_CHUNK - 1) / RZIP_CHUNK;
    const size_t cap = 20 + chunks * (4 + compressBound(RZIP_CHUNK));
    uint8_t* z = malloc(cap);
    if (!z) { free(p); return NULL; }
    memcpy(z, RZIP, 8);
    put32(z + 8, RZIP_CHUNK);
    put32(z + 12, (uint32_t)psize);
    put32(z + 16, (uint32_t)((uint64_t)psize >> 32));
    size_t o = 20;
    for (size_t at = 0; at < psize; at += RZIP_CHUNK) {
        const size_t n = psize - at < RZIP_CHUNK ? psize - at : RZIP_CHUNK;
        uLongf zn = (uLongf)(cap - o - 4);
        // level 1: a state is written while the game waits; RetroArch reads any level
        if (compress2(z + o + 4, &zn, p + at, n, 1) != Z_OK) { free(p); free(z); return NULL; }
        put32(z + o, (uint32_t)zn);
        o += 4 + zn;
    }
    free(p);
    *out_size = o;
    return z;
}

void* bf_state_unpack(const void* file, size_t file_size, size_t* core_size)
{
    const uint8_t* f = file;
    uint8_t* payload = NULL;
    size_t psize = 0;
    if (file_size >= 20 && !memcmp(f, RZIP, 8)) {
        const uint32_t chunk = get32(f + 8);
        const uint64_t total = get64(f + 12);
        if (!chunk || total > (1ull << 31)) return NULL;
        payload = malloc(total ? total : 1);
        if (!payload) return NULL;
        size_t at = 20;
        while (at + 4 <= file_size && psize < total) {
            const uint32_t zn = get32(f + at);
            at += 4;
            if (zn > file_size - at) { free(payload); return NULL; }
            uLongf n = (uLongf)(total - psize);
            if (uncompress(payload + psize, &n, f + at, zn) != Z_OK) { free(payload); return NULL; }
            psize += n;
            at += zn;
        }
        if (psize != total) { free(payload); return NULL; }
    } else {
        payload = malloc(file_size ? file_size : 1);
        if (!payload) return NULL;
        memcpy(payload, f, file_size);
        psize = file_size;
    }
    if (psize >= 8 && !memcmp(payload, RAST, 8)) {
        size_t at = 8;
        while (at + 8 <= psize) {
            const uint32_t n = get32(payload + at + 4);
            if (n > psize - at - 8) break;
            if (!memcmp(payload + at, "MEM ", 4)) {
                uint8_t* core = malloc(n ? n : 1);
                if (core) memcpy(core, payload + at + 8, n);
                free(payload);
                *core_size = n;
                return core;
            }
            if (!memcmp(payload + at, "END ", 4)) break;
            at += 8 + n + (8 - n % 8) % 8;
        }
        free(payload);
        return NULL;
    }
    *core_size = psize;  // raw core data
    return payload;
}
