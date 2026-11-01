// unzip.h: the first member of a .zip whose extension is in a libretro valid_extensions list ("gb|gbc|dmg").
#pragma once
#include <stddef.h>

// the member's bytes (malloc'd) and its name; NULL if none / not a zip / a method other than stored or deflate
void* bf_unzip_first(const char* zip, const char* exts, char* name, size_t name_n, size_t* size);
