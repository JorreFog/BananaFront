// memmap.c: the core's memory maps (RETRO_ENVIRONMENT_SET_MEMORY_MAPS) as RetroArch reads them: the descriptors
// preprocessed (select/len/disconnect filled in) and an address resolved to a pointer, exactly as RetroArch 1.22's
// runloop.c mmap_preprocess_descriptors and command.c command_memory_get_descriptor do (GPL-3.0, RetroArch team), so
// READ_CORE_MEMORY answers the companions the same, and rcheevos and the companion export read the same bytes.
// A core without maps: SYSTEM_RAM at address 0 (RetroArch's READ_CORE_MEMORY says "no memory map" there).
#include <string.h>

#include "bf.h"
#include "memmap.h"

static size_t add_bits_down(size_t n)
{
    n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
    if (sizeof(size_t) > 4) n |= n >> 16 >> 16;
    return n;
}
static size_t inflate_(size_t addr, size_t mask)
{
    while (mask) {
        size_t tmp = (mask - 1) & ~mask;
        addr = ((addr & ~tmp) << 1) | (addr & tmp);
        mask = mask & (mask - 1);
    }
    return addr;
}
static size_t reduce(size_t addr, size_t mask)
{
    while (mask) {
        size_t tmp = (mask - 1) & ~mask;
        addr = (addr & tmp) | ((addr >> 1) & ~tmp);
        mask = (mask & (mask - 1)) >> 1;
    }
    return addr;
}
static size_t highest_bit(size_t n) { n = add_bits_down(n); return n ^ (n >> 1); }

bool bf_mmap_preprocess(struct retro_memory_descriptor* first, unsigned count)
{
    size_t top_addr = 1;
    struct retro_memory_descriptor* end = first + count;
    for (struct retro_memory_descriptor* d = first; d < end; d++)
        top_addr |= d->select ? d->select : d->start + d->len - 1;
    top_addr = add_bits_down(top_addr);
    for (struct retro_memory_descriptor* d = first; d < end; d++) {
        if (d->select == 0) {
            if (d->len == 0 || (d->len & (d->len - 1)) != 0) return false;
            d->select = top_addr & ~inflate_(add_bits_down(d->len - 1), d->disconnect);
        }
        if (d->len == 0) d->len = add_bits_down(reduce(top_addr & ~d->select, d->disconnect)) + 1;
        if (d->start & ~d->select) return false;
        size_t highest_reachable = inflate_(d->len - 1, d->disconnect);
        while (highest_bit(top_addr & ~d->select & ~d->disconnect) > highest_bit(highest_reachable))
            d->disconnect |= highest_bit(top_addr & ~d->select & ~d->disconnect);
    }
    return true;
}

const struct retro_memory_descriptor* bf_mmap_find(const struct retro_memory_descriptor* d, unsigned n,
                                                   unsigned address, size_t* offset)
{
    for (const struct retro_memory_descriptor* end = d + n; d < end; d++) {
        if (d->select == 0) {
            if (address >= d->start && address < d->start + d->len) { *offset = address - d->start; return d; }
        } else if (((d->start ^ address) & d->select) == 0) {
            unsigned off = address - (unsigned)d->start;
            unsigned mask = (unsigned)d->disconnect;
            while (mask) {
                unsigned tmp = (mask - 1) & ~mask;
                off = (off & tmp) | ((off >> 1) & ~tmp);
                mask = (mask & (mask - 1)) >> 1;
            }
            if (off >= d->len) continue;  // (RetroArch reads past it; a companion must not)
            *offset = off;
            return d;
        }
    }
    return NULL;
}

// the preprocessed copy of the core's maps (g_core.mmap is the core's own, kept as given)
static struct retro_memory_descriptor* pp;
static unsigned pp_n;
static bool pp_ok;

void bf_mmap_refresh(void)
{
    pp = g_core.mmap;  // (core.c copied them; preprocessing in place is what RetroArch does to its copy)
    pp_n = g_core.mmap_n;
    // (RetroArch ignores a failure here too: the descriptors after the first it can't fill keep select 0 and are
    // matched by their start and length, so the same addresses answer)
    if (pp_n && !bf_mmap_preprocess(pp, pp_n))
        bf_log("memory maps: one of the core's %u descriptors has no select and a length that isn't a power of two "
               "(read by start and length, as RetroArch does)", pp_n);
    pp_ok = pp_n > 0;
}

bool bf_mmap_has_maps(void) { return pp_ok; }

uint8_t* bf_mmap_ptr(unsigned address, unsigned* max_bytes, int* err)
{
    *max_bytes = 0;
    if (pp_ok) {
        size_t off = 0;
        const struct retro_memory_descriptor* d = bf_mmap_find(pp, pp_n, address, &off);
        if (!d) { *err = BF_MM_NO_DESC; return NULL; }
        if (!d->ptr) { *err = BF_MM_NO_DATA; return NULL; }
        *max_bytes = (unsigned)(d->len - off);
        *err = 0;
        return (uint8_t*)d->ptr + d->offset + off;
    }
    // no maps: SYSTEM_RAM from 0
    uint8_t* ram = g_core.get_memory_data ? g_core.get_memory_data(RETRO_MEMORY_SYSTEM_RAM) : NULL;
    size_t sz = g_core.get_memory_size ? g_core.get_memory_size(RETRO_MEMORY_SYSTEM_RAM) : 0;
    if (!ram || address >= sz) { *err = BF_MM_NO_MAP; return NULL; }
    *max_bytes = (unsigned)(sz - address);
    *err = 0;
    return ram + address;
}

const char* bf_mmap_err(int err)
{
    switch (err) {
    case BF_MM_NO_MAP: return "no memory map defined";
    case BF_MM_NO_DESC: return "no descriptor for address";
    case BF_MM_NO_DATA: return "no data for descriptor";
    default: return "";
    }
}

// the core sent new maps (after the game loaded): RetroArch preprocesses them again too
__attribute__((weak)) void frontend_mmap_changed(void) {}
void bf_mmap_changed(void)
{
    if (!pp) return;  // (before the game loaded: main refreshes after the load)
    bf_mmap_refresh();
    frontend_mmap_changed();
}
