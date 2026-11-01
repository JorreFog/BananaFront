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

static struct {
    int fd;
    bool leased;
    uint32_t conn, crtc, plane, mode_blob;
    drmModeModeInfo mode;
    bool need_modeset;
    uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch, p_conn_crtc, p_mode, p_active;
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
} K = { .fd = -1, .shown = -1, .pending = -1, .drawing = -1 };

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

// where the picture goes on the panel
static void dest_rect(unsigned sw, unsigned sh, int* x, int* y, int* w, int* h)
{
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
    drmModeFreePlaneResources(pr);
    if (!K.plane) { bf_log("kms: no primary plane"); return false; }
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

static uint32_t drm_fmt(int core_fmt) { return core_fmt == RETRO_PIXEL_FORMAT_XRGB8888 ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_RGB565; }

// (re)make the buffers for a picture of w x h, and find out whether the plane scales it
static bool ensure_bufs(unsigned w, unsigned h, int core_fmt)
{
    const uint32_t f = drm_fmt(core_fmt);
    if (K.b[0].fb && K.b[0].fmt == f && ((K.hw && K.b[0].w >= w && K.b[0].h >= h) || (!K.hw && K.core_fmt == core_fmt)))
        return true;
    // the flip pending must finish before its buffer goes
    while (K.pending >= 0) {
        drmEventContext ev = { .version = 2 };
        (void)ev;
        bf_video_events();
    }
    for (int i = 0; i < NBUF; i++) freebuf(&K.b[i]);
    K.shown = K.drawing = -1;
    K.core_fmt = core_fmt;
    // the largest picture the core can make (its max geometry), so a resolution change needs no new buffers
    unsigned bw = w > g_core.av.geometry.max_width ? w : g_core.av.geometry.max_width;
    unsigned bh = h > g_core.av.geometry.max_height ? h : g_core.av.geometry.max_height;
    if (bw > 2048 || bh > 2048) { bw = w; bh = h; }
    K.hw = mkbuf(&K.b[0], bw, bh, f);
    if (K.hw && !getenv("BANANAFRONT_CPU_SCALE")) {
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

bool bf_video_commit(bool dupe)
{
    if (K.fd < 0 || K.pending >= 0) return false;
    int k = dupe ? K.shown : K.drawing;
    if (k < 0) return false;
    if (!dupe && !K.hw) cpu_scale(&K.b[k]);
    drmModeAtomicReq* r = drmModeAtomicAlloc();
    add_plane(r, &K.b[k], K.src_w, K.src_h);
    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    if (K.need_modeset) flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;
    int ret = drmModeAtomicCommit(K.fd, r, flags, NULL);
    drmModeAtomicFree(r);
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

void bf_video_close(void)
{
    if (K.fd < 0) return;
    while (K.pending >= 0) bf_video_events();
    // (the buffers go with the fd; closing it ends master or hands the lease back to banana-ui, which draws again)
    for (int i = 0; i < NBUF; i++) freebuf(&K.b[i]);
    free(K.cpu_src);
    close(K.fd);
    K.fd = -1;
}
