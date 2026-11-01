// unzip.h: the first member of a .zip whose extension is in a libretro valid_extensions list ("gb|gbc|dmg").
#pragma once
#include <stdbool.h>
#include <stddef.h>

// the member's bytes (malloc'd) and its name; NULL if none / not a zip / a method other than stored or deflate
void* bf_unzip_first(const char* zip, const char* exts, char* name, size_t name_n, size_t* size);
extern int bf_unzip_method;  // after a NULL: the matching member's zip method if it wasn't stored/deflate, else -1

// an archive's first member with an extension in exts, unpacked by 7-Zip (7zz: .7z, and zip methods zlib can't do -
// bzip2, LZMA, Deflate64...) into /run/banana/rom-cache/<pid>/; its path in out. false (and a log line) if none, too big
// for the free memory (tmpfs), or 7zz is missing / failed. bf_archive_cleanup removes what it unpacked.
bool bf_7z_extract(const char* archive, const char* exts, char* out, size_t out_n);
void bf_archive_cleanup(void);
