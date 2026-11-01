// archive.c: ROMs in archives the zip reader (unzip.c) can't open - a .7z, or a zip whose member is compressed with
// bzip2, LZMA, Deflate64 and the like (7-Zip's "Ultra", some Windows tools) - unpacked with 7-Zip's 7zz, which BananaOS
// ships for Files. RetroArch opens a .7z itself (and refuses those zips as BananaFront did): QA's fresh 0.5.1 card had
// an Emerald and a Dragon Quest VI that opened in neither.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bf.h"
#include "unzip.h"

extern char** environ;
static char cache_dir[300];

static const char* sevenzip(void)
{
    const char* e = getenv("BANANAFRONT_7ZZ");  // (tests)
    if (e && *e) return e;
    static const char* c[] = { "/usr/bin/7zz", "/usr/libexec/banana/7zz", "/usr/bin/7z", "/usr/bin/7za" };
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) if (access(c[i], X_OK) == 0) return c[i];
    return NULL;
}

static int ext_in(const char* name, const char* exts)
{
    const char* dot = strrchr(name, '.');
    if (!dot || !exts || !*exts) return 0;
    char l[512];
    snprintf(l, sizeof l, "%s", exts);
    char* save = NULL;
    for (char* t = strtok_r(l, "|", &save); t; t = strtok_r(NULL, "|", &save))
        if (!strcasecmp(t, dot + 1)) return 1;
    return 0;
}

// runs 7zz with argv (no shell: any name works); its stdout into buf (NUL-terminated) when buf is given
static int run7z(char* const argv[], char* buf, size_t n)
{
    int pfd[2] = { -1, -1 };
    if (buf && pipe(pfd) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (buf) { posix_spawn_file_actions_adddup2(&fa, pfd[1], 1); posix_spawn_file_actions_addclose(&fa, pfd[0]); }
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int e = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (buf) close(pfd[1]);
    if (e != 0) { if (buf) close(pfd[0]); return -1; }
    size_t got = 0;
    if (buf) {
        ssize_t r;
        while ((r = read(pfd[0], buf + got, n - 1 - got)) > 0) { got += (size_t)r; if (got >= n - 1) break; }
        buf[got] = 0;
        // (the rest drained so 7zz doesn't block on a full pipe)
        char sink[4096];
        while (read(pfd[0], sink, sizeof sink) > 0) {}
        close(pfd[0]);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static long long mem_available(void)
{
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char l[128];
    long long kb = -1;
    while (fgets(l, sizeof l, f)) if (sscanf(l, "MemAvailable: %lld kB", &kb) == 1) break;
    fclose(f);
    return kb < 0 ? -1 : kb * 1024;
}

bool bf_7z_extract(const char* archive, const char* exts, char* out, size_t out_n)
{
    const char* z = sevenzip();
    if (!z) { bf_log("archive: %s needs 7-Zip (7zz), which isn't installed", archive); return false; }
    // the listing (-slt: one "Key = value" a line, a blank line between members)
    static char list[1 << 20];
    char* la[] = { (char*)z, "l", "-slt", "-ba", "--", (char*)archive, NULL };
    if (run7z(la, list, sizeof list) != 0) { bf_log("archive: 7zz couldn't read %s", archive); return false; }
    char member[1024] = "", method[64] = "", path[1024] = "", meth[64] = "";
    long long size = -1, sz = -1;
    int dir = 0;
    for (char *save = NULL, *l = strtok_r(list, "\n", &save);; l = strtok_r(NULL, "\n", &save)) {
        // (a member ends at the next "Path =" or the end)
        if (!l || !strncmp(l, "Path = ", 7)) {
            if (path[0] && !dir && sz > 0 && ext_in(path, exts)) {
                snprintf(member, sizeof member, "%s", path);
                snprintf(method, sizeof method, "%s", meth);
                size = sz;
                break;
            }
            if (!l) break;
            snprintf(path, sizeof path, "%s", l + 7);
            path[strcspn(path, "\r")] = 0;
            meth[0] = 0; sz = -1; dir = 0;
        } else if (!strncmp(l, "Size = ", 7)) sz = atoll(l + 7);
        else if (!strncmp(l, "Method = ", 9)) { snprintf(meth, sizeof meth, "%s", l + 9); meth[strcspn(meth, "\r")] = 0; }
        else if (!strncmp(l, "Folder = +", 10) || !strncmp(l, "Attributes = D", 14)) dir = 1;
    }
    if (!member[0]) { bf_log("archive: no %s file in %s", exts ? exts : "", archive); return false; }
    // unpacked into /run (memory), and the core reads it again: twice its size, and 64 MiB to spare
    const long long avail = mem_available();
    if (avail > 0 && size * 2 + (64ll << 20) > avail) {
        bf_log("archive: %s is %lld MB unpacked, too big for the free memory (%lld MB)", member, size >> 20, avail >> 20);
        return false;
    }
    const char* cd = getenv("BANANAFRONT_ROM_CACHE");  // (tests)
    if (!cd || !*cd) cd = "/run/banana/rom-cache";
    // (what a BananaFront that didn't exit cleanly left: its pid's folder, gone with that process)
    DIR* rc = opendir(cd);
    if (rc) {
        struct dirent* e;
        char pp[64];
        while ((e = readdir(rc))) {
            const int p = atoi(e->d_name);
            if (p <= 0 || p == (int)getpid()) continue;
            snprintf(pp, sizeof pp, "/proc/%d", p);
            if (access(pp, F_OK) == 0) continue;
            snprintf(cache_dir, sizeof cache_dir, "%s/%s", cd, e->d_name);
            bf_archive_cleanup();
        }
        closedir(rc);
    }
    snprintf(cache_dir, sizeof cache_dir, "%s/%d", cd, (int)getpid());
    bf_mkdirs(cache_dir);
    char odir[320];
    snprintf(odir, sizeof odir, "-o%s", cache_dir);
    char* ea[] = { (char*)z, "e", "-y", "-bso0", "-bsp0", odir, "--", (char*)archive, member, NULL };
    if (run7z(ea, NULL, 0) != 0) { bf_log("archive: 7zz couldn't unpack %s from %s", member, archive); bf_archive_cleanup(); return false; }
    const char* base = strrchr(member, '/');
    if (!base) base = strrchr(member, '\\');
    snprintf(out, out_n, "%s/%s", cache_dir, base ? base + 1 : member);
    struct stat st;
    if (stat(out, &st) != 0 || st.st_size != size) {
        bf_log("archive: 7zz's %s isn't there or not whole", out);
        bf_archive_cleanup();
        return false;
    }
    bf_log("archive: unpacked %s from %s (%s) with 7zz", member, archive, method[0] ? method : "?");
    return true;
}

void bf_archive_cleanup(void)
{
    if (!cache_dir[0]) return;
    DIR* d = opendir(cache_dir);
    if (d) {
        struct dirent* e;
        char p[600];
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { snprintf(p, sizeof p, "%s/%s", cache_dir, e->d_name); unlink(p); }
        closedir(d);
    }
    rmdir(cache_dir);
    cache_dir[0] = 0;
}
