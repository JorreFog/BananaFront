/* bcomp_libretro.h - publish a libretro core's memory to the companion export (COMPANION-SPEC.md 3.3).
 * Include libretro.h first. Call from the frontend's RETRO_ENVIRONMENT_SET_MEMORY_MAPS handler (and once after
 * retro_load_game with the fallback when the core never sent a map). MIT. */
#ifndef BCOMP_LIBRETRO_H
#define BCOMP_LIBRETRO_H

#include "bcomp.h"
#include <stdio.h>

/* every descriptor with memory and a length becomes a region (mirrors too: a def address in any of them works) */
static inline int bcomp_add_retro_map(bcomp_producer* p, const struct retro_memory_map* mm)
{
	int added = 0;
	for (unsigned i = 0; i < mm->num_descriptors; i++)
	{
		const struct retro_memory_descriptor* d = &mm->descriptors[i];
		if (!d->ptr || !d->len) continue;
		uint32_t fl = 0;
		if (d->flags & RETRO_MEMDESC_CONST) fl |= BCOMP_R_ROM;
		else if (d->flags & RETRO_MEMDESC_SAVE_RAM) fl |= BCOMP_R_SRAM;
		else if (d->flags & RETRO_MEMDESC_VIDEO_RAM) fl |= BCOMP_R_VRAM;
		else fl |= BCOMP_R_RAM;
		if (d->flags & RETRO_MEMDESC_BIGENDIAN) fl |= BCOMP_R_BE;
		char name[16];
		if (d->addrspace && *d->addrspace) snprintf(name, sizeof name, "%s", d->addrspace);
		else snprintf(name, sizeof name, "map%u", i);
		if (bcomp_add_region(p, name, (uint32_t)d->start, (uint32_t)d->len, fl, 0,
				(const uint8_t*)d->ptr + d->offset) != 0) break;
		added++;
	}
	return added;
}

/* no memory map from the core: its SYSTEM_RAM / SAVE_RAM at the system map's addresses (systems/<id>.json) */
static inline void bcomp_add_retro_fallback(bcomp_producer* p, uint32_t ram_start, uint32_t sram_start,
	uint32_t (*get_size)(unsigned), void* (*get_data)(unsigned))
{
	void* ram = get_data(RETRO_MEMORY_SYSTEM_RAM);
	uint32_t n = get_size(RETRO_MEMORY_SYSTEM_RAM);
	if (ram && n) bcomp_add_region(p, "ram", ram_start, n, BCOMP_R_RAM, 0, ram);
	void* sram = get_data(RETRO_MEMORY_SAVE_RAM);
	n = get_size(RETRO_MEMORY_SAVE_RAM);
	if (sram && n) bcomp_add_region(p, "sram", sram_start, n, BCOMP_R_SRAM, 0, sram);
}

#endif
