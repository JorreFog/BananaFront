// unzip.c: just enough of the zip format for ROM archives: the central directory, stored and deflated members (zlib).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#include "bf.h"
#include "unzip.h"

static uint32_t g32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t g16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

static int ext_ok(const char* name, const char* exts)
{
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    if (!exts || !*exts) return strcasecmp(dot + 1, "zip") != 0;
    char l[512];
    snprintf(l, sizeof l, "%s", exts);
    char* save = NULL;
    for (char* t = strtok_r(l, "|", &save); t; t = strtok_r(NULL, "|", &save))
        if (!strcasecmp(t, dot + 1)) return 1;
    return 0;
}

int bf_unzip_method;  // the last bf_unzip_first's: the matching member's method when it wasn't stored/deflate, else -1

void* bf_unzip_first(const char* zip, const char* exts, char* name, size_t name_n, size_t* size)
{
    bf_unzip_method = -1;
    size_t n = 0;
    uint8_t* z = bf_read_file(zip, &n);
    if (!z || n < 22) { free(z); return NULL; }
    // the end of central directory record: within the last 64 KiB + 22
    long eocd = -1;
    for (long i = (long)n - 22; i >= 0 && i >= (long)n - 65557; i--)
        if (g32(z + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(z); return NULL; }
    uint32_t cd = g32(z + eocd + 16), count = g16(z + eocd + 10);
    void* out = NULL;
    for (uint32_t k = 0, at = cd; k < count && at + 46 <= n && g32(z + at) == 0x02014b50; k++) {
        uint16_t method = g16(z + at + 10), nlen = g16(z + at + 28), xlen = g16(z + at + 30), clen = g16(z + at + 32);
        uint32_t csize = g32(z + at + 20), usize = g32(z + at + 24), loc = g32(z + at + 42);
        char nm[512];
        snprintf(nm, sizeof nm, "%.*s", nlen < 511 ? nlen : 511, (const char*)z + at + 46);
        at += 46 + nlen + xlen + clen;
        if (!usize || nm[strlen(nm) - 1] == '/' || !ext_ok(nm, exts)) continue;
        if (loc + 30 > n || g32(z + loc) != 0x04034b50) break;
        const uint8_t* data = z + loc + 30 + g16(z + loc + 26) + g16(z + loc + 28);
        if (data + csize > z + n) break;
        uint8_t* o = malloc(usize);
        if (!o) break;
        if (method == 0 && csize == usize) memcpy(o, data, usize);
        else if (method == 8) {
            z_stream s = { 0 };
            s.next_in = (Bytef*)data;
            s.avail_in = csize;
            s.next_out = o;
            s.avail_out = usize;
            if (inflateInit2(&s, -MAX_WBITS) != Z_OK || inflate(&s, Z_FINISH) != Z_STREAM_END || s.total_out != usize) {
                inflateEnd(&s);
                free(o);
                break;
            }
            inflateEnd(&s);
        } else { bf_unzip_method = method; free(o); break; }  // (bzip2 12, LZMA 14, Deflate64 9...: 7zz's job)
        snprintf(name, name_n, "%s", nm);
        *size = usize;
        out = o;
        break;
    }
    free(z);
    return out;
}
