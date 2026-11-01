// ra.c: RetroAchievements in BananaFront (rcheevos rc_client, MIT): BananaDS's ra.c (itself SuperDrastic's) on any
// libretro core's memory.
// - The account: BANANAFRONT_RA_USER + BANANAFRONT_RA_TOKEN (RetroArch's token for the same account, from the setup's
//   accounts.conf) and/or BANANAFRONT_RA_PASSWORD (run-bananafront reads accounts.conf). A token BananaFront gets
//   itself is kept in $BANANAFRONT_DATA/ra.token (mode 600), never in RetroArch's or Banana's files.
// - Memory: the core's memory maps flattened into RA's address space by rcheevos' own rc_libretro (what RetroArch
//   uses), read at the frame's end on the main thread.
// - The game: identified by rcheevos' hash for the system, from the content in memory (a zip's member) or its file.
// - banana-ui's game panel today reads RetroArch's log for "[RCHEEVOS]" lines: the same lines go to
//   /run/banana/retroarch.log (hash, game id + title, each unlock), so the panel shows achievements with no change.
// - The companion's RA block (COMPANION-SPEC.md 4.1): state, counts, points, rich presence (once a second), the
//   closest measured achievements, an event ring; /run/banana/ra-game.json at load and after each unlock.
// - BANANA_RA_PROGRESS: "<unlocked> <total>" for the library (docs/EMULATORS.md).
// - HTTP: libcurl, dlopen'd, a worker thread per request. Softcore unless BANANAFRONT_RA_HARDCORE=1.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bf.h"
#include "ra.h"
#include "third_party/bcomp/bcomp.h"
#include "third_party/rcheevos/include/rc_client.h"
#include "third_party/rcheevos/include/rc_consoles.h"
#include "third_party/rcheevos/src/rc_libretro.h"

#define WARNING_ACH(a) ((a)->id >= 101000001u)

static rc_client_t* rc;
static rc_libretro_memory_regions_t mem;
static bool mem_ok;
static char TOKEN_FILE[600], BADGE_DIR[600];
static char user[128], pass[256], token[256], user_agent[256];
static volatile int game_loaded, logged_in, load_started, failed;
static uint32_t console;
static bool hardcore;
static FILE* ralog_f;  // /run/banana/retroarch.log (the game panel's reader)
static bcomp_producer* comp;
static double next_rp;

static void ralog(const char* fmt, ...)
{
    char b[700];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    bf_log("%s", b);
}
// a line in RetroArch's own words, for banana-ui's game panel
static void rclog(const char* fmt, ...)
{
    if (!ralog_f) return;
    va_list a;
    va_start(a, fmt);
    fputs("[INFO] [RCHEEVOS] ", ralog_f);
    vfprintf(ralog_f, fmt, a);
    fputc('\n', ralog_f);
    fflush(ralog_f);
    va_end(a);
}

// ---- pop-ups: queued here, drawn by the frontend's overlay ---------------------------------------------------------
#define NPOP 8
typedef struct { char l1[128], l2[160], badge[600]; uint32_t accent; int ms; } bf_popup_t;
static bf_popup_t pq[NPOP];
static int pq_head, pq_n;
static pthread_mutex_t pq_mu = PTHREAD_MUTEX_INITIALIZER;
static void popup_push(const char* l1, const char* l2, const char* badge, uint32_t accent, int ms)
{
    pthread_mutex_lock(&pq_mu);
    if (pq_n == NPOP) { pq_head = (pq_head + 1) % NPOP; pq_n--; }
    bf_popup_t* p = &pq[(pq_head + pq_n++) % NPOP];
    snprintf(p->l1, sizeof p->l1, "%s", l1 ? l1 : "");
    snprintf(p->l2, sizeof p->l2, "%s", l2 ? l2 : "");
    snprintf(p->badge, sizeof p->badge, "%s", badge ? badge : "");
    p->accent = accent;
    p->ms = ms;
    pthread_mutex_unlock(&pq_mu);
}
int ra_popup_take(char* l1, int n1, char* l2, int n2, char* badge, int nb, uint32_t* accent, int* ms)
{
    pthread_mutex_lock(&pq_mu);
    int got = pq_n > 0;
    if (got) {
        bf_popup_t* p = &pq[pq_head];
        snprintf(l1, n1, "%s", p->l1);
        snprintf(l2, n2, "%s", p->l2);
        snprintf(badge, nb, "%s", p->badge);
        *accent = p->accent;
        *ms = p->ms;
        pq_head = (pq_head + 1) % NPOP;
        pq_n--;
    }
    pthread_mutex_unlock(&pq_mu);
    return got;
}
void bf_popup_push(const char* l1, const char* l2, uint32_t accent, int ms) { popup_push(l1, l2, NULL, accent, ms); }

// ---- HTTP via libcurl (dlopen) -------------------------------------------------------------------------------------
static void* curl_lib;
static void* (*c_init)(void);
static int (*c_setopt)(void*, int, ...);
static int (*c_perform)(void*);
static int (*c_getinfo)(void*, int, ...);
static void (*c_cleanup)(void*);
static void* (*c_slist_append)(void*, const char*);
static void (*c_slist_free)(void*);
enum { CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_POSTFIELDS = 10015, CURLOPT_USERAGENT = 10018,
       CURLOPT_HTTPHEADER = 10023, CURLOPT_WRITEFUNCTION = 20011, CURLOPT_TIMEOUT = 13, CURLOPT_NOSIGNAL = 99,
       CURLOPT_FOLLOWLOCATION = 52, CURLINFO_RESPONSE_CODE = 0x200002 };

static int curl_load(void)
{
    if (curl_lib) return 1;
    curl_lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!curl_lib) return 0;
    c_init = (void* (*)(void))dlsym(curl_lib, "curl_easy_init");
    c_setopt = (int (*)(void*, int, ...))dlsym(curl_lib, "curl_easy_setopt");
    c_perform = (int (*)(void*))dlsym(curl_lib, "curl_easy_perform");
    c_getinfo = (int (*)(void*, int, ...))dlsym(curl_lib, "curl_easy_getinfo");
    c_cleanup = (void (*)(void*))dlsym(curl_lib, "curl_easy_cleanup");
    c_slist_append = (void* (*)(void*, const char*))dlsym(curl_lib, "curl_slist_append");
    c_slist_free = (void (*)(void*))dlsym(curl_lib, "curl_slist_free_all");
    return c_init && c_setopt && c_perform && c_getinfo && c_cleanup && c_slist_append && c_slist_free;
}

typedef struct { char* data; size_t len; } membuf;
static size_t on_body(char* p, size_t sz, size_t n, void* u)
{
    membuf* m = u;
    size_t add = sz * n;
    char* d = realloc(m->data, m->len + add + 1);
    if (!d) return 0;
    memcpy(d + m->len, p, add);
    m->len += add;
    d[m->len] = 0;
    m->data = d;
    return add;
}

typedef struct { char *url, *post, *ctype; rc_client_server_callback_t cb; void* cbdata; } http_req;

// the offline cache (COMPANION-SPEC.md 4.1): the answers a game start needs (patch = the set, startsession = the
// unlocks, achievementsets/gameid = the hash's game), kept per request; served when the server can't be reached
static const char* CACHE_DIR = "/data/banana/ra-cache";
static void cache_name(const char* post, char* out, size_t n)
{
    // the request's own words minus the account's token (r=patch&u=x&t=TOKEN&m=HASH...)
    unsigned long h = 5381;
    const char* r = strstr(post ? post : "", "r=");
    char kind[24] = "x";
    if (r) sscanf(r + 2, "%23[a-z]", kind);
    for (const char* s = post; s && *s; s++) {
        if (s[0] == 't' && s[1] == '=' && (s == post || s[-1] == '&')) { while (*s && *s != '&') s++; if (!*s) break; }
        h = h * 33 + (unsigned char)*s;
    }
    snprintf(out, n, "%s/%s.%08lx.json", CACHE_DIR, kind, h & 0xffffffffUL);
}
static bool cacheable(const char* post)
{
    return post && (strstr(post, "r=patch") || strstr(post, "r=achievementsets") || strstr(post, "r=gameid") ||
                    strstr(post, "r=startsession"));
}

static void* http_thread(void* a)
{
    http_req* r = a;
    membuf body = { 0, 0 };
    long status = 0;
    void* h = c_init();
    void* hdrs = 0;
    if (h) {
        c_setopt(h, CURLOPT_URL, r->url);
        c_setopt(h, CURLOPT_USERAGENT, user_agent);
        c_setopt(h, CURLOPT_NOSIGNAL, 1L);
        c_setopt(h, CURLOPT_TIMEOUT, 30L);
        c_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        c_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
        c_setopt(h, CURLOPT_WRITEDATA, &body);
        if (r->post) {
            c_setopt(h, CURLOPT_POSTFIELDS, r->post);
            if (r->ctype) {
                char ct[128];
                snprintf(ct, sizeof ct, "Content-Type: %s", r->ctype);
                hdrs = c_slist_append(0, ct);
                c_setopt(h, CURLOPT_HTTPHEADER, hdrs);
            }
        }
        if (c_perform(h) == 0) c_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        c_cleanup(h);
        if (hdrs) c_slist_free(hdrs);
    }
    char cf[700];
    if (cacheable(r->post)) {
        cache_name(r->post, cf, sizeof cf);
        if (status == 200 && body.len && strstr(body.data, "\"Success\":true")) {
            bf_mkdirs(CACHE_DIR);
            bf_write_file(cf, body.data, body.len);
        } else if (status == 0 || status >= 500) {
            size_t n = 0;
            char* c = bf_read_file(cf, &n);
            if (c) {
                free(body.data);
                body.data = realloc(c, n + 1);
                body.data[n] = 0;
                body.len = n;
                status = 200;
                ralog("[ra] offline: the cached answer %s", cf);
            }
        }
    }
    rc_api_server_response_t resp;
    resp.body = body.data ? body.data : "";
    resp.body_length = body.len;
    resp.http_status_code = status ? (int)status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    r->cb(&resp, r->cbdata);
    free(body.data);
    free(r->url);
    free(r->post);
    free(r->ctype);
    free(r);
    return 0;
}

static void detach(void* (*fn)(void*), void* arg, const char* name)
{
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, fn, arg)) fn(arg);
    else pthread_setname_np(t, name);
    pthread_attr_destroy(&at);
}

static void server_call(const rc_api_request_t* req, rc_client_server_callback_t cb, void* cbdata, rc_client_t* c)
{
    (void)c;
    if (!curl_load()) {
        rc_api_server_response_t resp = { "", 0, RC_API_SERVER_RESPONSE_CLIENT_ERROR };
        cb(&resp, cbdata);
        return;
    }
    http_req* r = calloc(1, sizeof *r);
    r->url = strdup(req->url);
    r->post = req->post_data ? strdup(req->post_data) : 0;
    r->ctype = req->content_type ? strdup(req->content_type) : 0;
    r->cb = cb;
    r->cbdata = cbdata;
    detach(http_thread, r, "bf-ra-http");
}

// ---- memory --------------------------------------------------------------------------------------------------------
static void core_memory_info(uint32_t id, rc_libretro_core_memory_info_t* info)
{
    info->data = g_core.get_memory_data(id);
    info->size = g_core.get_memory_size(id);
}

static void memory_setup(void)
{
    if (mem_ok) rc_libretro_memory_destroy(&mem);
    struct retro_memory_map mm = { g_core.mmap, g_core.mmap_n };
    mem_ok = rc_libretro_memory_init(&mem, g_core.mmap_n ? &mm : NULL, core_memory_info, console) != 0;
    ralog("[ra] memory: %u regions, %zu bytes%s", mem.count, mem.total_size, mem_ok ? "" : " (none)");
}

static uint32_t read_memory(uint32_t addr, uint8_t* buf, uint32_t n, rc_client_t* c)
{
    (void)c;
    return mem_ok ? rc_libretro_memory_read(&mem, addr, buf, n) : 0;
}

// ---- images: badges, cached as files -------------------------------------------------------------------------------
static int fetch_file(const char* url, const char* path)
{
    if (access(path, R_OK) == 0) return 1;
    if (!*url || !curl_load()) return 0;
    membuf body = { 0, 0 };
    long status = 0;
    void* h = c_init();
    if (!h) return 0;
    c_setopt(h, CURLOPT_URL, url);
    c_setopt(h, CURLOPT_USERAGENT, user_agent);
    c_setopt(h, CURLOPT_NOSIGNAL, 1L);
    c_setopt(h, CURLOPT_TIMEOUT, 10L);
    c_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    c_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
    c_setopt(h, CURLOPT_WRITEDATA, &body);
    if (c_perform(h) == 0) c_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    c_cleanup(h);
    int ok = status == 200 && body.len > 8 && !memcmp(body.data, "\x89PNG", 4) && bf_write_file(path, body.data, body.len);
    free(body.data);
    return ok;
}
static void badge_file(const rc_client_achievement_t* a, char* path, size_t n)
{
    snprintf(path, n, "%s/%s.png", BADGE_DIR, a->badge_name);
}
static int badge_fetch(const rc_client_achievement_t* a, char* path, size_t n)
{
    char url[512] = "";
    badge_file(a, path, n);
    rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof url);
    return fetch_file(url, path);
}

// ---- what the panel and the library read ---------------------------------------------------------------------------
static void json_str(FILE* f, const char* s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static void write_game_json(void)
{
    const rc_client_game_t* g = rc_client_get_game_info(rc);
    if (!g) return;
    const char* path = "/run/banana/ra-game.json";
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(rc, &s);
    fprintf(f, "{\"game_id\":%u,\"title\":", g->id);
    json_str(f, g->title);
    fprintf(f, ",\"icon\":\"badges/game-%s.png\",\"hardcore\":%s,\"points\":[%u,%u],\"achievements\":[",
            g->badge_name ? g->badge_name : "0", hardcore ? "true" : "false", s.points_unlocked, s.points_core);
    rc_client_achievement_list_t* l = rc_client_create_achievement_list(rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    int first = 1;
    for (uint32_t b = 0; l && b < l->num_buckets; b++)
        for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
            const rc_client_achievement_t* a = l->buckets[b].achievements[i];
            if (WARNING_ACH(a)) continue;
            fprintf(f, "%s{\"id\":%u,\"title\":", first ? "" : ",", a->id);
            json_str(f, a->title);
            fputs(",\"desc\":", f);
            json_str(f, a->description);
            fprintf(f, ",\"points\":%u,\"badge\":\"badges/%s.png\",\"unlocked\":%d,\"unlock_time\":%lld,\"measured\":",
                    a->points, a->badge_name,
                    (a->unlocked & RC_CLIENT_ACHIEVEMENT_UNLOCKED_HARDCORE) ? 2 : a->unlocked ? 1 : 0,
                    (long long)a->unlock_time);
            json_str(f, a->measured_progress);
            fprintf(f, ",\"percent\":%d}", (int)a->measured_percent);
            first = 0;
        }
    if (l) rc_client_destroy_achievement_list(l);
    fputs("]}\n", f);
    fclose(f);
    rename(tmp, path);
}

static void write_progress(void)
{
    const char* p = getenv("BANANA_RA_PROGRESS");
    if (!p || !*p || !rc) return;
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(rc, &s);
    if (!s.num_core_achievements) return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%u %u\n", s.num_unlocked_achievements, s.num_core_achievements);
    fclose(f);
    rename(tmp, p);
}

// the RA block of the companion export: counts, rich presence, the closest measured achievements
static void block_update(int state)
{
    if (!comp || !comp->shm) return;
    bcomp_ra_begin(comp);
    bcomp_ra* r = &comp->shm->ra;
    if (state) r->state = (uint32_t)state;
    r->hardcore = hardcore;
    const rc_client_game_t* g = game_loaded ? rc_client_get_game_info(rc) : NULL;
    if (g) {
        r->game_id = g->id;
        rc_client_user_game_summary_t s;
        rc_client_get_user_game_summary(rc, &s);
        r->unlocked = s.num_unlocked_achievements;
        r->total = s.num_core_achievements;
        r->points_got = s.points_unlocked;
        r->points_total = s.points_core;
        rc_client_get_rich_presence_message(rc, r->rp, sizeof r->rp);
        // the closest locked achievements with measured progress
        memset(r->near_, 0, sizeof r->near_);
        rc_client_achievement_list_t* l = rc_client_create_achievement_list(
            rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
        int n = 0;
        for (uint32_t b = 0; l && b < l->num_buckets; b++)
            for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
                const rc_client_achievement_t* a = l->buckets[b].achievements[i];
                if (a->unlocked || !a->measured_progress[0] || WARNING_ACH(a)) continue;
                int k = n < BCOMP_RA_NEAR ? n++ : BCOMP_RA_NEAR;  // sorted by percent, highest first
                while (k > 0 && a->measured_percent > r->near_[k - 1].percent) {
                    if (k < BCOMP_RA_NEAR) r->near_[k] = r->near_[k - 1];
                    k--;
                }
                if (k >= BCOMP_RA_NEAR) continue;
                r->near_[k].id = a->id;
                r->near_[k].percent = (uint32_t)a->measured_percent;
                snprintf(r->near_[k].measured, sizeof r->near_[k].measured, "%s", a->measured_progress);
                snprintf(r->near_[k].title, sizeof r->near_[k].title, "%s", a->title);
            }
        if (l) rc_client_destroy_achievement_list(l);
    }
    bcomp_ra_end(comp);
}

static void block_event(uint32_t type, uint32_t id, const char* value, const char* title)
{
    if (!comp || !comp->shm) return;
    bcomp_ra_begin(comp);
    bcomp_ra_push(comp, type, id, value, title);
    bcomp_ra_end(comp);
}

// ---- events, login, game load --------------------------------------------------------------------------------------
typedef struct { char l1[128], l2[160], badge[600]; const rc_client_achievement_t* a; } unlock_msg;
static void* unlock_thread(void* a)
{
    unlock_msg* m = a;
    int have = badge_fetch(m->a, m->badge, sizeof m->badge);
    popup_push(m->l1, m->l2, have ? m->badge : 0, 0xffd84a, 5000);
    free(m);
    return 0;
}

static void on_event(const rc_client_event_t* e, rc_client_t* c)
{
    (void)c;
    char v[32];
    switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
        if (WARNING_ACH(e->achievement)) { ralog("[ra] warning from the server: %s", e->achievement->title); break; }
        ralog("[ra] unlocked: %s (%u)", e->achievement->title, e->achievement->points);
        rclog("Awarding achievement %u: %s", e->achievement->id, e->achievement->title);
        write_progress();
        write_game_json();
        snprintf(v, sizeof v, "%u", e->achievement->points);
        block_event(1, e->achievement->id, v, e->achievement->title);
        block_update(0);
        unlock_msg* m = calloc(1, sizeof *m);
        if (!m) break;
        snprintf(m->l1, sizeof m->l1, "Achievement unlocked \xc2\xb7 %u points", e->achievement->points);
        snprintf(m->l2, sizeof m->l2, "%s", e->achievement->title);
        m->a = e->achievement;
        detach(unlock_thread, m, "bf-ra");
        break;
    }
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        popup_push("All achievements unlocked!", rc_client_get_game_info(rc)->title, 0, 0xffd84a, 6000);
        block_event(2, rc_client_get_game_info(rc)->id, "", rc_client_get_game_info(rc)->title);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_STARTED: block_event(3, e->leaderboard->id, "", e->leaderboard->title); break;
    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
        popup_push("Leaderboard submitted", e->leaderboard->title, 0, 0x6ab0ff, 3000);
        block_event(4, e->leaderboard->id, e->leaderboard->tracker_value, e->leaderboard->title);
        break;
    case RC_CLIENT_EVENT_LEADERBOARD_FAILED: block_event(5, e->leaderboard->id, "", e->leaderboard->title); break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW: block_event(6, e->leaderboard_tracker->id, e->leaderboard_tracker->display, ""); break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE: block_event(7, e->leaderboard_tracker->id, e->leaderboard_tracker->display, ""); break;
    case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE: block_event(8, e->leaderboard_tracker->id, "", ""); break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        ralog("[ra] server error: %s", e->server_error->error_message);
        block_event(9, 0, "", e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        popup_push("RetroAchievements", "Offline: unlocks will be sent later", 0, 0xff7a4a, 3000);
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        popup_push("RetroAchievements", "Back online, unlocks sent", 0, 0x6ad07a, 3000);
        break;
    }
}

static void* prefetch_thread(void* a)
{
    char* icon_url = a;
    bf_mkdirs(BADGE_DIR);
    char icon[700];
    const rc_client_game_t* g = rc_client_get_game_info(rc);
    snprintf(icon, sizeof icon, "%s/game-%s.png", BADGE_DIR, g && g->badge_name ? g->badge_name : "0");
    fetch_file(icon_url, icon);
    free(icon_url);
    rc_client_achievement_list_t* l = rc_client_create_achievement_list(rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    int got = 0, n = 0;
    for (uint32_t b = 0; l && b < l->num_buckets; b++)
        for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
            char path[700];
            n++;
            got += badge_fetch(l->buckets[b].achievements[i], path, sizeof path);
        }
    if (l) rc_client_destroy_achievement_list(l);
    ralog("[ra] badges: %d of %d cached in %s", got, n, BADGE_DIR);
    return 0;
}

static void on_load(int result, const char* err, rc_client_t* c, void* u)
{
    (void)u;
    if (result != RC_OK) {
        ralog("[ra] game load failed: %s", err ? err : "?");
        failed = 1;
        if (result == RC_NO_GAME_LOADED) {
            rclog("Unknown game");
            block_update(5);
            popup_push("RetroAchievements", "This game isn't known to RetroAchievements", 0, 0xff7a4a, 4000);
        } else {
            block_update(6);
            popup_push("RetroAchievements", err ? err : "Couldn't load the game", 0, 0xff7a4a, 4000);
        }
        return;
    }
    const rc_client_game_t* g = rc_client_get_game_info(c);
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(c, &s);
    char l2[160];
    if (s.num_core_achievements)
        snprintf(l2, sizeof l2, "%u of %u achievements unlocked", s.num_unlocked_achievements, s.num_core_achievements);
    else snprintf(l2, sizeof l2, "No achievements for this game yet");
    ralog("[ra] game %u '%s': %s", g->id, g->title, l2);
    if (g->hash) rclog("Generated hash %s", g->hash);
    rclog("Identified game: %u \"%s\"", g->id, g->title);
    extern void frontend_ra_game(unsigned id, const char* title);
    frontend_ra_game(g->id, g->title);
    game_loaded = 1;
    write_progress();
    write_game_json();
    block_update(s.num_core_achievements ? 3 : 4);
    popup_push(g->title, l2, 0, 0x6ab0ff, 5000);
    char* url = calloc(1, 512);
    if (url) { rc_client_game_get_image_url(g, url, 512); detach(prefetch_thread, url, "bf-ra"); }
}

static void save_token(void)
{
    const rc_client_user_t* u = rc_client_get_user_info(rc);
    if (!u || !u->token) return;
    char b[512];
    int n = snprintf(b, sizeof b, "%s\n%s\n", u->username, u->token);
    if (bf_write_file(TOKEN_FILE, b, (size_t)n)) chmod(TOKEN_FILE, 0600);
}

static void logged_in_popup(void)
{
    const rc_client_user_t* u = rc_client_get_user_info(rc);
    char l2[160];
    if (u && u->score) snprintf(l2, sizeof l2, "Logged in as %s \xc2\xb7 %u points", u->display_name ? u->display_name : user, u->score);
    else snprintf(l2, sizeof l2, "Logged in as %s", u && u->display_name ? u->display_name : user);
    popup_push("RetroAchievements", l2, 0, 0x6ad07a, 3000);
}

static void on_login_password(int result, const char* err, rc_client_t* c, void* u);
static void on_login_token(int result, const char* err, rc_client_t* c, void* u)
{
    (void)c; (void)u;
    if (result == RC_OK) {
        ralog("[ra] logged in with a token as %s", user);
        logged_in_popup();
        logged_in = 1;
        return;
    }
    ralog("[ra] token login failed (%s)%s", err ? err : "?", *pass ? ", trying the password" : "");
    if (*pass) rc_client_begin_login_with_password(rc, user, pass, on_login_password, 0);
    else {
        rclog("Login failed: %s", err ? err : "?");
        block_update(7);
        failed = 1;
        popup_push("RetroAchievements", "Login expired: enter your password in Settings", 0, 0xff7a4a, 5000);
    }
}
static void on_login_password(int result, const char* err, rc_client_t* c, void* u)
{
    (void)c; (void)u;
    if (result != RC_OK) {
        ralog("[ra] login failed: %s", err ? err : "?");
        rclog("Login failed: %s", err ? err : "?");
        block_update(7);
        failed = 1;
        popup_push("RetroAchievements login failed", err ? err : "Check the account in Settings", 0, 0xff7a4a, 5000);
        return;
    }
    ralog("[ra] logged in as %s", user);
    save_token();
    logged_in_popup();
    logged_in = 1;
}

static void on_rc_log(const char* msg, const rc_client_t* c) { (void)c; ralog("[rc] %s", msg); }

uint32_t ra_console_of(const char* s)
{
    static const struct { const char* id; uint32_t c; } T[] = {
        { "gba", RC_CONSOLE_GAMEBOY_ADVANCE }, { "gb", RC_CONSOLE_GAMEBOY }, { "gbc", RC_CONSOLE_GAMEBOY_COLOR },
        { "nes", RC_CONSOLE_NINTENDO }, { "snes", RC_CONSOLE_SUPER_NINTENDO }, { "megadrive", RC_CONSOLE_MEGA_DRIVE },
        { "sms", RC_CONSOLE_MASTER_SYSTEM }, { "gamegear", RC_CONSOLE_GAME_GEAR }, { "psx", RC_CONSOLE_PLAYSTATION },
        { "n64", RC_CONSOLE_NINTENDO_64 }, { "pcengine", RC_CONSOLE_PC_ENGINE }, { "ngp", RC_CONSOLE_NEOGEO_POCKET },
        { "atari2600", RC_CONSOLE_ATARI_2600 }, { "lynx", RC_CONSOLE_ATARI_LYNX } };
    for (unsigned i = 0; s && i < sizeof T / sizeof *T; i++)
        if (!strcmp(T[i].id, s)) return T[i].c;
    return 0;
}

bool ra_init(const char* system, void* companion)
{
    comp = companion;
    const char* eu = getenv("BANANAFRONT_RA_USER");
    snprintf(user, sizeof user, "%s", eu ? eu : "");
    const char* ep = getenv("BANANAFRONT_RA_PASSWORD");
    snprintf(pass, sizeof pass, "%s", ep ? ep : "");
    const char* et = getenv("BANANAFRONT_RA_TOKEN");
    snprintf(token, sizeof token, "%s", et ? et : "");
    // (the secrets out of the environment: nothing the core or a child would see)
    unsetenv("BANANAFRONT_RA_PASSWORD");
    unsetenv("BANANAFRONT_RA_TOKEN");
    console = ra_console_of(system);
    if (!*user) { ralog("[ra] off: no account"); block_update(0); return false; }
    if (!console) { ralog("[ra] off: no RetroAchievements for system '%s'", system ? system : ""); return false; }
    const char* data = getenv("BANANAFRONT_DATA");
    if (!data || !*data) data = "/data/bananafront";
    bf_mkdirs(data);
    snprintf(TOKEN_FILE, sizeof TOKEN_FILE, "%s/ra.token", data);
    snprintf(BADGE_DIR, sizeof BADGE_DIR, "%s/badges", CACHE_DIR);
    hardcore = getenv("BANANAFRONT_RA_HARDCORE") && !strcmp(getenv("BANANAFRONT_RA_HARDCORE"), "1");
    // the game panel's reader: RetroArch's log path (banana-ui unlinks it before the game starts)
    const char* lp = getenv("BANANAFRONT_RA_LOG");
    ralog_f = fopen(lp ? lp : "/run/banana/retroarch.log", "a");
    rc = rc_client_create(read_memory, server_call);
    rc_client_enable_logging(rc, RC_CLIENT_LOG_LEVEL_WARN, on_rc_log);
    rc_client_set_event_handler(rc, on_event);
    rc_client_set_hardcore_enabled(rc, hardcore);
    char clause[128] = "";
    rc_client_get_user_agent_clause(rc, clause, sizeof clause);
    snprintf(user_agent, sizeof user_agent, "BananaFront/%s (BananaOS) %s", BF_VERSION, clause);
    memory_setup();
    block_update(1);
    // our own token first (from a password login here), then RetroArch's for the same account, then the password
    char tu[128] = "", tok[256] = "";
    FILE* tf = fopen(TOKEN_FILE, "r");
    if (tf) {
        if (fgets(tu, sizeof tu, tf) && fgets(tok, sizeof tok, tf)) {
            tu[strcspn(tu, "\r\n")] = 0;
            tok[strcspn(tok, "\r\n")] = 0;
        }
        fclose(tf);
    }
    if (!*tok || strcasecmp(tu, user)) snprintf(tok, sizeof tok, "%s", token);
    if (*tok) rc_client_begin_login_with_token(rc, user, tok, on_login_token, 0);
    else if (*pass) rc_client_begin_login_with_password(rc, user, pass, on_login_password, 0);
    else { popup_push("RetroAchievements", "No password: enter it in Settings", 0, 0xff7a4a, 4000); return false; }
    return true;
}

void ra_memory_changed(void) { if (rc) memory_setup(); }

void ra_frame(void)
{
    if (!rc) return;
    if (logged_in && !load_started) {
        load_started = 1;
        block_update(2);
        // the content the core runs: in memory (a zip's member) or its file
        if (g_core.rom_data)
            rc_client_begin_identify_and_load_game(rc, console, g_core.rom_path, g_core.rom_data, g_core.rom_size, on_load, 0);
        else rc_client_begin_identify_and_load_game(rc, console, g_core.rom_path, NULL, 0, on_load, 0);
    }
    if (game_loaded) {
        rc_client_do_frame(rc);
        const double t = bf_now_ms();
        if (t >= next_rp) { next_rp = t + 1000; block_update(0); }
    } else rc_client_idle(rc);
}

void ra_idle(void) { if (rc) rc_client_idle(rc); }
void ra_reset(void) { if (rc && game_loaded) rc_client_reset(rc); }
bool ra_hardcore(void) { return rc && hardcore && game_loaded; }

int ra_progress(unsigned* unlocked, unsigned* total)
{
    if (!rc || !game_loaded) return 0;
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(rc, &s);
    *unlocked = s.num_unlocked_achievements;
    *total = s.num_core_achievements;
    return 1;
}

int ra_recent(ra_recent_t* out, int max)
{
    if (!rc || !game_loaded || max <= 0) return 0;
    rc_client_achievement_list_t* l = rc_client_create_achievement_list(rc, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                                                                        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    int n = 0;
    for (uint32_t b = 0; l && b < l->num_buckets; b++)
        for (uint32_t i = 0; i < l->buckets[b].num_achievements; i++) {
            const rc_client_achievement_t* a = l->buckets[b].achievements[i];
            if (!a->unlocked || WARNING_ACH(a)) continue;
            int k = n < max ? n++ : max;
            while (k > 0 && (long long)a->unlock_time > out[k - 1].when) {
                if (k < max) out[k] = out[k - 1];
                k--;
            }
            if (k >= max) continue;
            ra_recent_t* r = &out[k];
            snprintf(r->title, sizeof r->title, "%s", a->title ? a->title : "");
            snprintf(r->desc, sizeof r->desc, "%s", a->description ? a->description : "");
            badge_file(a, r->badge, sizeof r->badge);
            r->points = a->points;
            r->when = (long long)a->unlock_time;
        }
    if (l) rc_client_destroy_achievement_list(l);
    return n;
}

// states carry rcheevos' own state (RetroArch's "ACHV" block does the same); not written yet: a load resets instead
void ra_state_loaded(void) { if (rc && game_loaded) rc_client_reset(rc); }

void ra_quit(void)
{
    if (!rc) return;
    rc_client_destroy(rc);  // (waits for nothing: pending award requests are lost if the network is slow)
    rc = NULL;
    if (mem_ok) rc_libretro_memory_destroy(&mem);
    if (ralog_f) fclose(ralog_f);
    unlink("/run/banana/ra-game.json");
}
