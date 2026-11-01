// kms.c: the game on one panel through KMS, no compositor and no GPU.
// - The panel: banana-ui's DRM lease when it lends one (BANANA_DRM_LEASE_FD: that panel's connector, CRTC and primary
//   plane; banana-ui keeps the other panel for the game panel), else DRM master on card0 and the panel bf_video_opts
//   names (as RetroArch's video_monitor_index). The mode stays as it is (the CRTC's own, else the panel's preferred).
// - The core's picture goes into a DRM dumb buffer of the core's size and pixel format (RGB565 / XRGB8888; 0RGB1555
//   converted to RGB565), and the display plane scales it to the panel (VOP2 does; checked once with a TEST_ONLY
//   commit). Where the plane can't scale, the CPU scales into a panel-sized buffer (nearest, centred).
// - One atomic commit per frame, nonblocking, with a flip event: main runs the next core frame when the flip is done
//   (one frame per refresh, the lowest latency KMS gives without racing the scanout).
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "bf.h"
#include "video.h"

#define NBUF 3
typedef struct {
    uint32_t handle, fb, pitch, w, h, fmt;
    void* map;
    size_t size;
} buf_t;

int shader_init(int fd, const char* name);
int shader_draw(uint32_t sh, int sw, int shh, int sp, uint64_t sgen, uint32_t dh, int dw, int dhh, int dp, uint64_t dgen, int finish);
extern int shader_fast;
extern uint32_t shader_src_fourcc;
int shader_fence(void);
void shader_flush(void);
// BANANAFRONT_SHADER_SYNC=implicit (an A/B): no fence of our own, the kernel's implicit sync on the shared buffer
static int implicit_sync = -1;
void dsflip_log(const char* fmt, ...)
{
    char b[600];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    bf_log("%s", b);
}

static struct {
    buf_t out[NBUF];   // the shader's output: the picture's place on the panel, shown 1:1
    int out_shown, out_pending;
    bool shader;
    char shader_name[64];
    uint64_t gen;      // bumped whenever buffers are made again (the shader caches its imports by these numbers)
    uint32_t ov, ov_fb, ov_crtc, ov_sx, ov_sy, ov_sw, ov_sh, ov_cx, ov_cy, ov_cw, ov_ch;  // the overlay plane
    buf_t ovb[4];
    int ov_draw, ov_rows, ov_pub, ov_scan, ov_prev;
    bool ov_on, ov_was;
    buf_t canvas[2];   // the menu: panel-sized XRGB8888, on the primary plane while the menu is open
    int canvas_shown;
    int fd;
    bool leased;
    uint32_t conn, crtc, plane, mode_blob;
    drmModeModeInfo mode;
    bool need_modeset;
    uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch, p_conn_crtc, p_mode, p_active, p_infence;
    buf_t b[NBUF];
    int shown, pending, drawing;
    bool hw;           // the plane scales
    enum bf_scale scale;
    unsigned src_w, src_h;  // the picture in the drawing buffer
    float aspect;
    int core_fmt;
    uint8_t* cpu_src;  // !hw: the core's picture, scaled into the buffer at commit
    int cpu_pitch;
    double last_flip;
} K = { .fd = -1, .shown = -1, .pending = -1, .drawing = -1, .canvas_shown = -1, .out_shown = -1, .out_pending = -1,
        .ov_pub = -1, .ov_scan = -1, .ov_prev = -1 };

static uint32_t prop(uint32_t obj, uint32_t type, const char* name)
{
    uint32_t id = 0;
    drmModeObjectProperties* ps = drmModeObjectGetProperties(K.fd, obj, type);
    for (uint32_t i = 0; ps && i < ps->count_props && !id; i++) {
        drmModePropertyRes* p = drmModeGetProperty(K.fd, ps->props[i]);
        if (p && !strcmp(p->name, name)) id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(ps);
    return id;
}

static uint64_t propval(uint32_t obj, uint32_t type, const char* name)
{
    uint64_t v = 0;
    drmModeObjectProperties* ps = drmModeObjectGetProperties(K.fd, obj, type);
    for (uint32_t i = 0; ps && i < ps->count_props; i++) {
        drmModePropertyRes* p = drmModeGetProperty(K.fd, ps->props[i]);
        if (p && !strcmp(p->name, name)) v = ps->prop_values[i];
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(ps);
    return v;
}

static void freebuf(buf_t* b)
{
    if (b->map) munmap(b->map, b->size);
    if (b->fb) drmModeRmFB(K.fd, b->fb);
    if (b->handle) {
        struct drm_mode_destroy_dumb d = { .handle = b->handle };
        drmIoctl(K.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    }
    memset(b, 0, sizeof *b);
}

static bool mkbuf(buf_t* b, uint32_t w, uint32_t h, uint32_t fmt)
{
    memset(b, 0, sizeof *b);
    struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = fmt == DRM_FORMAT_RGB565 ? 16 : 32 };
    if (drmIoctl(K.fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return false;
    b->handle = c.handle;
    b->pitch = c.pitch;
    b->size = c.size;
    b->w = w;
    b->h = h;
    b->fmt = fmt;
    uint32_t hs[4] = { c.handle }, ps[4] = { c.pitch }, os[4] = { 0 };
    if (drmModeAddFB2(K.fd, w, h, fmt, hs, ps, os, &b->fb, 0)) { freebuf(b); return false; }
    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(K.fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { freebuf(b); return false; }
    b->map = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, K.fd, m.offset);
    if (b->map == MAP_FAILED) { b->map = NULL; freebuf(b); return false; }
    memset(b->map, 0, c.size);
    return true;
}

static int fixed[4];  // the screen border's hole: the picture goes exactly there
void bf_video_fixed_rect(int x, int y, int w, int h) { fixed[0] = x; fixed[1] = y; fixed[2] = w; fixed[3] = h; }

// where the picture goes on the panel
static void dest_rect(unsigned sw, unsigned sh, int* x, int* y, int* w, int* h)
{
    if (fixed[2] > 0 && fixed[3] > 0) { *x = fixed[0]; *y = fixed[1]; *w = fixed[2]; *h = fixed[3]; return; }
    const int W = K.mode.hdisplay, H = K.mode.vdisplay;
    const double a = K.aspect > 0 ? K.aspect : (double)sw / sh;
    if (K.scale == BF_SCALE_STRETCH) { *x = *y = 0; *w = W; *h = H; return; }
    if (K.scale == BF_SCALE_INTEGER) {
        int k = (int)(W / sw) < (int)(H / sh) ? (int)(W / sw) : (int)(H / sh);
        if (k < 1) k = 1;
        *w = sw * k;
        *h = sh * k;
    } else if (W / a <= H) {
        *w = W;
        *h = (int)(W / a + 0.5);
    } else {
        *h = H;
        *w = (int)(H * a + 0.5);
    }
    *w &= ~1;
    *h &= ~1;
    if (*w > W) *w = W;
    if (*h > H) *h = H;
    *x = (W - *w) / 2;
    *y = (H - *h) / 2;
}

static void add_plane(drmModeAtomicReq* r, const buf_t* b, unsigned sw, unsigned sh)
{
    int x, y, w, h;
    if (K.hw) dest_rect(sw, sh, &x, &y, &w, &h);
    else { x = y = 0; w = K.mode.hdisplay; h = K.mode.vdisplay; sw = b->w; sh = b->h; }
    drmModeAtomicAddProperty(r, K.plane, K.p_fb, b->fb);
    drmModeAtomicAddProperty(r, K.plane, K.p_crtc, K.crtc);
    drmModeAtomicAddProperty(r, K.plane, K.p_sx, 0);
    drmModeAtomicAddProperty(r, K.plane, K.p_sy, 0);
    drmModeAtomicAddProperty(r, K.plane, K.p_sw, (uint64_t)sw << 16);
    drmModeAtomicAddProperty(r, K.plane, K.p_sh, (uint64_t)sh << 16);
    drmModeAtomicAddProperty(r, K.plane, K.p_cx, x);
    drmModeAtomicAddProperty(r, K.plane, K.p_cy, y);
    drmModeAtomicAddProperty(r, K.plane, K.p_cw, w);
    drmModeAtomicAddProperty(r, K.plane, K.p_ch, h);
    if (K.need_modeset) {
        drmModeAtomicAddProperty(r, K.conn, K.p_conn_crtc, K.crtc);
        drmModeAtomicAddProperty(r, K.crtc, K.p_mode, K.mode_blob);
        drmModeAtomicAddProperty(r, K.crtc, K.p_active, 1);
    }
}

static bool pick_panel(const bf_video_opts* o)
{
    drmModeRes* res = drmModeGetResources(K.fd);
    if (!res) { bf_log("kms: no resources: %s", strerror(errno)); return false; }
    // the connected connectors; with a lease there is exactly one
    uint32_t conns[8];
    char names[8][32];
    int n = 0;
    for (int i = 0; i < res->count_connectors && n < 8; i++) {
        drmModeConnector* c = drmModeGetConnector(K.fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) {
            conns[n] = c->connector_id;
            const char* tn = drmModeGetConnectorTypeName(c->connector_type);
            snprintf(names[n], 32, "%s-%u", tn ? tn : "?", c->connector_type_id);
            n++;
        }
        drmModeFreeConnector(c);
    }
    if (!n) { drmModeFreeResources(res); bf_log("kms: no connected panel"); return false; }
    int pick = 0;
    if (!K.leased && n > 1) {
        // the top panel is the one named top_conn; the bottom the other
        int top = 0;
        for (int i = 0; i < n; i++)
            if (o->top_conn && !strcmp(names[i], o->top_conn)) top = i;
        pick = o->screen == 0 ? top : (top == 0 ? 1 : 0);
    }
    K.conn = conns[pick];
    drmModeConnector* c = drmModeGetConnector(K.fd, K.conn);
    // its CRTC: the one driving it now, else the first its encoders allow
    uint32_t crtc = 0;
    if (c->encoder_id) {
        drmModeEncoder* e = drmModeGetEncoder(K.fd, c->encoder_id);
        if (e) crtc = e->crtc_id;
        drmModeFreeEncoder(e);
    }
    if (!crtc && res->count_crtcs == 1) crtc = res->crtcs[0];
    for (int i = 0; i < c->count_encoders && !crtc; i++) {
        drmModeEncoder* e = drmModeGetEncoder(K.fd, c->encoders[i]);
        for (int j = 0; e && j < res->count_crtcs && !crtc; j++)
            if (e->possible_crtcs & (1u << j)) crtc = res->crtcs[j];
        drmModeFreeEncoder(e);
    }
    K.crtc = crtc;
    drmModeCrtc* cr = crtc ? drmModeGetCrtc(K.fd, crtc) : NULL;
    if (cr && cr->mode_valid) K.mode = cr->mode;
    else {
        K.mode = c->modes[0];
        for (int i = 0; i < c->count_modes; i++)
            if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) { K.mode = c->modes[i]; break; }
        K.need_modeset = true;
    }
    drmModeFreeCrtc(cr);
    bf_log("kms: %s panel %s, %ux%u %.3f Hz%s", K.leased ? "leased" : "master", names[pick], K.mode.hdisplay,
           K.mode.vdisplay, bf_video_refresh(), K.need_modeset ? " (mode set)" : "");
    drmModeFreeConnector(c);
    // the CRTC's index, for the plane's possible_crtcs
    int idx = -1;
    for (int j = 0; j < res->count_crtcs; j++)
        if (res->crtcs[j] == crtc) idx = j;
    drmModeFreeResources(res);
    if (!crtc || idx < 0) { bf_log("kms: no CRTC for the panel"); return false; }
    drmModePlaneRes* pr = drmModeGetPlaneResources(K.fd);
    for (uint32_t j = 0; pr && j < pr->count_planes && !K.plane; j++) {
        drmModePlane* pl = drmModeGetPlane(K.fd, pr->planes[j]);
        if (pl && (pl->possible_crtcs & (1u << idx)) &&
            propval(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type") == DRM_PLANE_TYPE_PRIMARY)
            K.plane = pl->plane_id;
        drmModeFreePlane(pl);
    }
    // an overlay plane for pop-ups over the game: the one in the lease, else a free one (master)
    for (uint32_t j = 0; pr && j < pr->count_planes && !K.ov; j++) {
        drmModePlane* pl = drmModeGetPlane(K.fd, pr->planes[j]);
        if (pl && (pl->possible_crtcs & (1u << idx)) && !pl->fb_id &&
            propval(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type") == DRM_PLANE_TYPE_OVERLAY)
            K.ov = pl->plane_id;
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pr);
    if (!K.plane) { bf_log("kms: no primary plane"); return false; }
    if (K.ov) {
        const uint32_t P = DRM_MODE_OBJECT_PLANE;
        K.ov_fb = prop(K.ov, P, "FB_ID"); K.ov_crtc = prop(K.ov, P, "CRTC_ID");
        K.ov_sx = prop(K.ov, P, "SRC_X"); K.ov_sy = prop(K.ov, P, "SRC_Y"); K.ov_sw = prop(K.ov, P, "SRC_W");
        K.ov_sh = prop(K.ov, P, "SRC_H"); K.ov_cx = prop(K.ov, P, "CRTC_X"); K.ov_cy = prop(K.ov, P, "CRTC_Y");
        K.ov_cw = prop(K.ov, P, "CRTC_W"); K.ov_ch = prop(K.ov, P, "CRTC_H");
    }
    bf_log("kms: overlay plane %s", K.ov ? "there (pop-ups over the game)" : "none (pop-ups on banana-ui's panel only)");
    return true;
}

bool bf_video_open(const bf_video_opts* o)
{
    K.scale = o->scale;
    const char* l = getenv("BANANA_DRM_LEASE_FD");
    if (l && *l) {
        K.fd = fcntl(atoi(l), F_DUPFD_CLOEXEC, 3);
        K.leased = K.fd >= 0;
        if (K.fd < 0) bf_log("kms: the lease fd %s: %s", l, strerror(errno));
        close(atoi(l));  // (our copy is enough; the original isn't passed on to anything we start)
        unsetenv("BANANA_DRM_LEASE_FD");
    }
    if (K.fd < 0) K.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (K.fd < 0) { bf_log("kms: /dev/dri/card0: %s", strerror(errno)); return false; }
    drmSetClientCap(K.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (drmSetClientCap(K.fd, DRM_CLIENT_CAP_ATOMIC, 1)) { bf_log("kms: no atomic modesetting"); return false; }
    if (!pick_panel(o)) return false;
    const uint32_t P = DRM_MODE_OBJECT_PLANE;
    K.p_fb = prop(K.plane, P, "FB_ID");
    K.p_crtc = prop(K.plane, P, "CRTC_ID");
    K.p_sx = prop(K.plane, P, "SRC_X");
    K.p_sy = prop(K.plane, P, "SRC_Y");
    K.p_sw = prop(K.plane, P, "SRC_W");
    K.p_sh = prop(K.plane, P, "SRC_H");
    K.p_cx = prop(K.plane, P, "CRTC_X");
    K.p_cy = prop(K.plane, P, "CRTC_Y");
    K.p_cw = prop(K.plane, P, "CRTC_W");
    K.p_ch = prop(K.plane, P, "CRTC_H");
    K.p_infence = prop(K.plane, P, "IN_FENCE_FD");
    K.p_conn_crtc = prop(K.conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    K.p_mode = prop(K.crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    K.p_active = prop(K.crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    if (drmModeCreatePropertyBlob(K.fd, &K.mode, sizeof K.mode, &K.mode_blob)) K.mode_blob = 0;
    // a rotated plane (left by something else) upright, as banana-ui does
    uint32_t rot = prop(K.plane, P, "rotation");
    if (rot && propval(K.plane, P, "rotation") != DRM_MODE_ROTATE_0)
        drmModeObjectSetProperty(K.fd, K.plane, P, rot, DRM_MODE_ROTATE_0);
    return true;
}

static void add_overlay(drmModeAtomicReq* r);

static uint32_t drm_fmt(int core_fmt)
{
    return core_fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_RGB565;
}

// the shader (Settings > Other systems > Shader): GL comes up here, once; false: none (the plane scales as without)
bool bf_video_shader(const char* name)
{
    if (!name || !*name || K.fd < 0) return false;
    shader_fast = 0;  // (its fast twins are DS-specific)
    // (the shader's environment: the image's folder unless the launcher named one)
    if (!getenv("DSFLIP_SHADER_DIR")) setenv("DSFLIP_SHADER_DIR", "/usr/lib/superdrastic/shaders", 0);
    K.shader = shader_init(K.fd, name) != 0;
    snprintf(K.shader_name, sizeof K.shader_name, "%s", name);
    bf_log("kms: shader %s: %s", name, K.shader ? "on" : "unavailable, the plane scales instead");
    return K.shader;
}
bool bf_video_wants_xrgb(void) { return false; }  // (the shader imports RGB565 as it is)
void bf_video_shader_off(void) { K.shader = false; K.shader_name[0] = 0; }
const char* bf_video_shader_name(void) { return K.shader ? K.shader_name : ""; }
void bf_video_set_scale(enum bf_scale s) { K.scale = s; }
enum bf_scale bf_video_get_scale(void) { return K.scale; }

// (re)make the buffers for a picture of w x h, and find out whether the plane scales it
static bool ensure_bufs(unsigned w, unsigned h, int core_fmt)
{
    const uint32_t f = drm_fmt(core_fmt);
    if (K.b[0].fb && K.b[0].fmt == f && ((K.hw && K.b[0].w >= w && K.b[0].h >= h) || (!K.hw && K.core_fmt == core_fmt)))
        return true;
    // the flip pending must finish before its buffer goes
    while (K.pending >= 0) bf_video_events();
    for (int i = 0; i < NBUF; i++) freebuf(&K.b[i]);
    K.shown = K.drawing = -1;
    K.core_fmt = core_fmt;
    // the largest picture the core can make (its max geometry), so a resolution change needs no new buffers
    unsigned bw = w > g_core.av.geometry.max_width ? w : g_core.av.geometry.max_width;
    unsigned bh = h > g_core.av.geometry.max_height ? h : g_core.av.geometry.max_height;
    if (bw > 2048 || bh > 2048) { bw = w; bh = h; }
    K.gen += 0x1000;
    K.hw = mkbuf(&K.b[0], bw, bh, f);
    if (K.hw && K.shader) {
        // (the shader's output is shown 1:1: the game's buffer is only the shader's input)
    } else if (K.hw && !getenv("BANANAFRONT_CPU_SCALE")) {
        drmModeAtomicReq* r = drmModeAtomicAlloc();
        add_plane(r, &K.b[0], w, h);
        K.hw = drmModeAtomicCommit(K.fd, r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL) == 0;
        drmModeAtomicFree(r);
        if (!K.hw) bf_log("kms: the plane doesn't scale %ux%u: the CPU does", w, h);
    } else K.hw = false;
    freebuf(&K.b[0]);
    if (!K.hw) { bw = K.mode.hdisplay; bh = K.mode.vdisplay; }
    for (int i = 0; i < NBUF; i++)
        if (!mkbuf(&K.b[i], bw, bh, f)) { bf_log("kms: no scanout buffers (%ux%u): %s", bw, bh, strerror(errno)); return false; }
    bf_log("kms: %s buffers %ux%u %s", K.hw ? "plane-scaled" : "panel-sized", bw, bh, f == DRM_FORMAT_RGB565 ? "RGB565" : "XRGB8888");
    return true;
}

static bool ensure_out(unsigned sw, unsigned sh)
{
    int x, y, w, h;
    const bool hw = K.hw;
    K.hw = true;
    dest_rect(sw, sh, &x, &y, &w, &h);
    K.hw = hw;
    if (K.out[0].fb && (int)K.out[0].w == w && (int)K.out[0].h == h) return true;
    while (K.pending >= 0) bf_video_events();
    for (int i = 0; i < NBUF; i++) freebuf(&K.out[i]);
    K.out_shown = K.out_pending = -1;
    K.gen += 0x1000;
    for (int i = 0; i < NBUF; i++)
        if (!mkbuf(&K.out[i], (uint32_t)w, (uint32_t)h, DRM_FORMAT_XRGB8888)) { bf_log("kms: no shader output buffers"); K.shader = false; return false; }
    bf_log("kms: shader output %dx%d at %d,%d", w, h, x, y);
    return true;
}

void* bf_video_begin(unsigned w, unsigned h, int fmt, float aspect, int* pitch)
{
    if (!ensure_bufs(w, h, fmt)) return NULL;
    K.aspect = aspect;
    K.src_w = w;
    K.src_h = h;
    if (!K.hw) {
        // the core's picture kept here, scaled into the scanout buffer at commit
        const int bpp = fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
        static size_t cap;
        if ((size_t)w * h * bpp > cap) { cap = (size_t)w * h * bpp; K.cpu_src = realloc(K.cpu_src, cap); }
        K.cpu_pitch = w * bpp;
        *pitch = K.cpu_pitch;
        return K.cpu_src;
    }
    // a buffer that's neither on the panel nor waiting to be
    int k = 0;
    while (k == K.shown || k == K.pending) k++;
    K.drawing = k;
    *pitch = K.b[k].pitch;
    return K.b[k].map;
}

static void cpu_scale(buf_t* b)
{
    int x, y, w, h;
    K.hw = true;  // (dest_rect computes the fitted rectangle)
    dest_rect(K.src_w, K.src_h, &x, &y, &w, &h);
    K.hw = false;
    const int bpp = b->fmt == DRM_FORMAT_RGB565 ? 2 : 4;
    uint8_t* dst = b->map;
    for (int dy = 0; dy < h; dy++) {
        const unsigned sy = (unsigned)((uint64_t)dy * K.src_h / h);
        uint8_t* drow = dst + (size_t)(y + dy) * b->pitch + (size_t)x * bpp;
        const uint8_t* srow = K.cpu_src + (size_t)sy * K.cpu_pitch;
        if (bpp == 2)
            for (int dx = 0; dx < w; dx++) ((uint16_t*)drow)[dx] = ((const uint16_t*)srow)[(uint64_t)dx * K.src_w / w];
        else
            for (int dx = 0; dx < w; dx++) ((uint32_t*)drow)[dx] = ((const uint32_t*)srow)[(uint64_t)dx * K.src_w / w];
    }
}

static void add_plane_out(drmModeAtomicReq* r, const buf_t* b)
{
    int x, y, w, h;
    const bool hw = K.hw;
    K.hw = true;
    dest_rect(K.src_w, K.src_h, &x, &y, &w, &h);
    K.hw = hw;
    drmModeAtomicAddProperty(r, K.plane, K.p_fb, b->fb);
    drmModeAtomicAddProperty(r, K.plane, K.p_crtc, K.crtc);
    drmModeAtomicAddProperty(r, K.plane, K.p_sx, 0);
    drmModeAtomicAddProperty(r, K.plane, K.p_sy, 0);
    drmModeAtomicAddProperty(r, K.plane, K.p_sw, (uint64_t)b->w << 16);
    drmModeAtomicAddProperty(r, K.plane, K.p_sh, (uint64_t)b->h << 16);
    drmModeAtomicAddProperty(r, K.plane, K.p_cx, x);
    drmModeAtomicAddProperty(r, K.plane, K.p_cy, y);
    drmModeAtomicAddProperty(r, K.plane, K.p_cw, b->w);
    drmModeAtomicAddProperty(r, K.plane, K.p_ch, b->h);
}

bool bf_video_commit(bool dupe)
{
    if (K.fd < 0 || K.pending >= 0) return false;
    int k = dupe ? K.shown : K.drawing;
    if (k < 0) return false;
    if (!dupe && !K.hw) cpu_scale(&K.b[k]);
    if (implicit_sync < 0) implicit_sync = getenv("BANANAFRONT_SHADER_SYNC") && !strcmp(getenv("BANANAFRONT_SHADER_SYNC"), "implicit");
    drmModeAtomicReq* r = drmModeAtomicAlloc();
    int fence = -1;
    if (K.shader && ensure_out(K.src_w, K.src_h)) {
        // the shader pass: the game's buffer -> an output buffer the size of its place on the panel (GPU, waited for:
        // the commit must not show a half-drawn picture)
        int o = dupe && K.out_shown >= 0 ? K.out_shown : -1;
        if (o < 0) {
            o = 0;
            while (o == K.out_shown || o == K.out_pending) o++;
            const buf_t* sb = &K.b[k];
            shader_src_fourcc = sb->fmt;
            // (no glFinish: the commit carries the GPU's fence and the display waits for it, not this thread)
            if (shader_draw(sb->handle, (int)K.src_w, (int)K.src_h, (int)sb->pitch, K.gen + (uint64_t)k + 1, K.out[o].handle,
                            (int)K.out[o].w, (int)K.out[o].h, (int)K.out[o].pitch, K.gen + (uint64_t)o + 101, !K.p_infence && !implicit_sync)) {
                bf_log("kms: the shader failed: off");
                K.shader = false;
            } else if (K.p_infence && !implicit_sync) fence = shader_fence();
            else if (implicit_sync) shader_flush();  // (the display waits on the buffer's own fences: dma-buf implicit sync)
        }
        K.out_pending = o;
        if (K.shader) {
            add_plane_out(r, &K.out[o]);
            if (fence >= 0) drmModeAtomicAddProperty(r, K.plane, K.p_infence, (uint64_t)fence);
        } else add_plane(r, &K.b[k], K.src_w, K.src_h);
    } else
        add_plane(r, &K.b[k], K.src_w, K.src_h);
    add_overlay(r);
    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    if (K.need_modeset) flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;
    int ret = drmModeAtomicCommit(K.fd, r, flags, NULL);
    drmModeAtomicFree(r);
    if (fence >= 0) close(fence);
    if (ret) {
        static int warned;
        if (warned++ < 5) bf_log("kms: commit failed: %s", strerror(-ret));
        return false;
    }
    K.need_modeset = false;
    K.pending = k;
    K.drawing = -1;
    return true;
}

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void* data)
{
    (void)fd; (void)seq; (void)sec; (void)usec; (void)data;
    K.shown = K.pending;
    K.pending = -1;
    if (K.out_pending >= 0) { K.out_shown = K.out_pending; K.out_pending = -1; }
    K.last_flip = bf_now_ms();
}

void bf_video_events(void)
{
    drmEventContext ev = { .version = 2, .page_flip_handler = on_flip };
    drmHandleEvent(K.fd, &ev);
}

bool bf_video_flip_pending(void) { return K.pending >= 0; }
int bf_video_fd(void) { return K.fd; }
double bf_video_last_flip_ms(void) { return K.last_flip; }

double bf_video_refresh(void)
{
    const drmModeModeInfo* m = &K.mode;
    if (!m->htotal || !m->vtotal) return 60.0;
    return m->clock * 1000.0 / ((double)m->htotal * m->vtotal);
}

int bf_video_size(int* w, int* h)
{
    *w = K.mode.hdisplay;
    *h = K.mode.vdisplay;
    return 0;
}

uint32_t* bf_video_grab(int* w, int* h) { (void)w; (void)h; return NULL; }

// ---- the overlay plane: pop-ups and the FPS counter over the game (ARGB8888, premultiplied), the panel's width, the
// top rows only; in the game's own commit, so it costs nothing while hidden (the plane off)
// (drawn on frontend.c's overlay thread: four buffers - the one committed last, the one before it (on the panel
// until that flip is done), the one published for the next commit, and the one being drawn)
uint32_t* bf_video_overlay(int rows, int* pitch, int* w)
{
    if (K.fd < 0 || !K.ov || rows <= 0) return NULL;
    if (K.ov_rows != rows) {
        // (a new size: only from the main thread's first use, or with the plane off - see frontend.c)
        for (int i = 0; i < 4; i++) freebuf(&K.ovb[i]);
        for (int i = 0; i < 4; i++)
            if (!mkbuf(&K.ovb[i], K.mode.hdisplay, (uint32_t)rows, DRM_FORMAT_ARGB8888)) { K.ov = 0; return NULL; }
        K.ov_rows = rows;
        __atomic_store_n(&K.ov_pub, -1, __ATOMIC_RELEASE);
        K.ov_scan = K.ov_prev = -1;
    }
    const int pub = __atomic_load_n(&K.ov_pub, __ATOMIC_ACQUIRE), scan = __atomic_load_n(&K.ov_scan, __ATOMIC_ACQUIRE);
    const int prev = __atomic_load_n(&K.ov_prev, __ATOMIC_ACQUIRE);
    int k = 0;
    while (k == pub || k == scan || k == prev) k++;
    K.ov_draw = k;
    *pitch = (int)K.ovb[k].pitch;
    *w = K.mode.hdisplay;
    memset(K.ovb[k].map, 0, K.ovb[k].size);
    return K.ovb[k].map;
}
// the buffer just drawn goes up with the next commit (on), or the plane comes down (off)
void bf_video_overlay_show(bool on)
{
    if (on && K.ov && K.ov_rows) __atomic_store_n(&K.ov_pub, K.ov_draw, __ATOMIC_RELEASE);
    __atomic_store_n(&K.ov_on, on && K.ov && K.ov_rows, __ATOMIC_RELEASE);
}
bool bf_video_has_overlay(void) { return K.ov != 0; }

static void add_overlay(drmModeAtomicReq* r)
{
    if (!K.ov) return;
    const int pub = __atomic_load_n(&K.ov_pub, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&K.ov_on, __ATOMIC_ACQUIRE) && pub >= 0) {
        const buf_t* b = &K.ovb[pub];
        drmModeAtomicAddProperty(r, K.ov, K.ov_fb, b->fb);
        drmModeAtomicAddProperty(r, K.ov, K.ov_crtc, K.crtc);
        drmModeAtomicAddProperty(r, K.ov, K.ov_sx, 0);
        drmModeAtomicAddProperty(r, K.ov, K.ov_sy, 0);
        drmModeAtomicAddProperty(r, K.ov, K.ov_sw, (uint64_t)b->w << 16);
        drmModeAtomicAddProperty(r, K.ov, K.ov_sh, (uint64_t)b->h << 16);
        drmModeAtomicAddProperty(r, K.ov, K.ov_cx, 0);
        drmModeAtomicAddProperty(r, K.ov, K.ov_cy, 0);
        drmModeAtomicAddProperty(r, K.ov, K.ov_cw, b->w);
        drmModeAtomicAddProperty(r, K.ov, K.ov_ch, b->h);
        const int was = __atomic_load_n(&K.ov_scan, __ATOMIC_ACQUIRE);
        if (was != pub) { __atomic_store_n(&K.ov_prev, was, __ATOMIC_RELEASE); __atomic_store_n(&K.ov_scan, pub, __ATOMIC_RELEASE); }
        K.ov_was = true;
    } else if (K.ov_was) {
        drmModeAtomicAddProperty(r, K.ov, K.ov_fb, 0);
        drmModeAtomicAddProperty(r, K.ov, K.ov_crtc, 0);
        K.ov_was = false;
    }
}

// ---- the menu's canvas: the primary plane shows a panel-sized XRGB8888 buffer instead of the game while it's open
// (no scaling, no second plane needed: the lease has only the primary); the next game commit puts the game back
int bf_video_canvas(uint32_t** px, int* pitch, int* w, int* h)
{
    if (K.fd < 0) return -1;
    while (K.pending >= 0) bf_video_events();
    for (int i = 0; i < 2; i++)
        if (!K.canvas[i].fb && !mkbuf(&K.canvas[i], K.mode.hdisplay, K.mode.vdisplay, DRM_FORMAT_XRGB8888)) return -1;
    const int k = K.canvas_shown == 0 ? 1 : 0;
    *px = K.canvas[k].map;
    *pitch = (int)K.canvas[k].pitch;
    *w = K.mode.hdisplay;
    *h = K.mode.vdisplay;
    return k;
}

void bf_video_canvas_show(int k)
{
    if (K.fd < 0 || k < 0 || k > 1 || !K.canvas[k].fb) return;
    while (K.pending >= 0) bf_video_events();
    drmModeAtomicReq* r = drmModeAtomicAlloc();
    const buf_t* b = &K.canvas[k];
    drmModeAtomicAddProperty(r, K.plane, K.p_fb, b->fb);
    drmModeAtomicAddProperty(r, K.plane, K.p_crtc, K.crtc);
    drmModeAtomicAddProperty(r, K.plane, K.p_sx, 0);
    drmModeAtomicAddProperty(r, K.plane, K.p_sy, 0);
    drmModeAtomicAddProperty(r, K.plane, K.p_sw, (uint64_t)b->w << 16);
    drmModeAtomicAddProperty(r, K.plane, K.p_sh, (uint64_t)b->h << 16);
    drmModeAtomicAddProperty(r, K.plane, K.p_cx, 0);
    drmModeAtomicAddProperty(r, K.plane, K.p_cy, 0);
    drmModeAtomicAddProperty(r, K.plane, K.p_cw, b->w);
    drmModeAtomicAddProperty(r, K.plane, K.p_ch, b->h);
    if (K.ov_was) {  // (no pop-up over the menu)
        drmModeAtomicAddProperty(r, K.ov, K.ov_fb, 0);
        drmModeAtomicAddProperty(r, K.ov, K.ov_crtc, 0);
        K.ov_was = false;
    }
    // (blocking: the menu draws at its own pace, and the buffer it gets next is the one not on the panel)
    if (drmModeAtomicCommit(K.fd, r, 0, NULL) == 0) K.canvas_shown = k;
    drmModeAtomicFree(r);
}

// the menu closed: the game's last picture back (and its buffers' sizes as before)
void bf_video_canvas_done(void)
{
    if (K.fd < 0) return;
    if (K.shown >= 0) {
        drmModeAtomicReq* r = drmModeAtomicAlloc();
        if (K.shader && K.out_shown >= 0) add_plane_out(r, &K.out[K.out_shown]);
        else add_plane(r, &K.b[K.shown], K.src_w, K.src_h);
        drmModeAtomicCommit(K.fd, r, 0, NULL);
        drmModeAtomicFree(r);
    }
    K.canvas_shown = -1;
}

void bf_video_close(void)
{
    if (K.fd < 0) return;
    while (K.pending >= 0) bf_video_events();
    // (the buffers go with the fd; closing it ends master or hands the lease back to banana-ui, which draws again)
    for (int i = 0; i < NBUF; i++) freebuf(&K.b[i]);
    for (int i = 0; i < 2; i++) freebuf(&K.canvas[i]);
    for (int i = 0; i < NBUF; i++) freebuf(&K.out[i]);
    for (int i = 0; i < 4; i++) freebuf(&K.ovb[i]);
    free(K.cpu_src);
    close(K.fd);
    K.fd = -1;
}
