// memmap.h: the core's memory maps resolved as RetroArch resolves them (memmap.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libretro.h"

enum { BF_MM_NO_MAP = 1, BF_MM_NO_DESC, BF_MM_NO_DATA };

bool bf_mmap_preprocess(struct retro_memory_descriptor* first, unsigned count);
const struct retro_memory_descriptor* bf_mmap_find(const struct retro_memory_descriptor* d, unsigned n,
                                                   unsigned address, size_t* offset);
void bf_mmap_refresh(void);  // after the game loaded / the core set new maps
bool bf_mmap_has_maps(void);
// the byte at a guest address and how many follow it in the same block; NULL + err when not mapped
uint8_t* bf_mmap_ptr(unsigned address, unsigned* max_bytes, int* err);
const char* bf_mmap_err(int err);
