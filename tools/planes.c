// planes.c: lists the DRM planes (id, type, CRTCs, formats, zpos) and CRTCs, read-only (no master needed).
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 1;
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    drmModeRes* r = drmModeGetResources(fd);
    for (int i = 0; r && i < r->count_crtcs; i++) {
        drmModeCrtc* c = drmModeGetCrtc(fd, r->crtcs[i]);
        printf("crtc %u idx %d fb %u mode %s\n", c->crtc_id, i, c->buffer_id, c->mode_valid ? c->mode.name : "-");
        drmModeFreeCrtc(c);
    }
    drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
    for (unsigned j = 0; pr && j < pr->count_planes; j++) {
        drmModePlane* p = drmModeGetPlane(fd, pr->planes[j]);
        drmModeObjectProperties* ps = drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
        printf("plane %u crtcs 0x%x fb %u", p->plane_id, p->possible_crtcs, p->fb_id);
        for (unsigned i = 0; ps && i < ps->count_props; i++) {
            drmModePropertyRes* pp = drmModeGetProperty(fd, ps->props[i]);
            if (pp && (!strcmp(pp->name, "type") || !strcmp(pp->name, "zpos") || !strcmp(pp->name, "NAME")))
                printf(" %s=%llu", pp->name, (unsigned long long)ps->prop_values[i]);
            drmModeFreeProperty(pp);
        }
        printf(" fmts:");
        for (unsigned f = 0; f < p->count_formats && f < 12; f++) printf(" %.4s", (char*)&p->formats[f]);
        printf("\n");
        drmModeFreePlane(p);
    }
    return 0;
}
