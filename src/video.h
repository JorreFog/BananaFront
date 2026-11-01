// video.h: the game panel. kms.c (the handheld: DRM lease or master, dumb buffers the display plane scales) or
// video_null.c (PC tests: frames counted and checksummed, paced by a timer).
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum bf_scale { BF_SCALE_FIT, BF_SCALE_INTEGER, BF_SCALE_STRETCH };

typedef struct {
    int screen;             // without a lease: 0 the top panel, 1 the bottom
    const char* top_conn;   // the top panel's connector name (BANANA_TOP: DSI-2 on the RG DS)
    enum bf_scale scale;
} bf_video_opts;

bool bf_video_open(const bf_video_opts* o);
// the core's next picture: a buffer of w x h in the core's format (fmt: RETRO_PIXEL_FORMAT_*) to write into
// (pitch in bytes); NULL on error. aspect: the picture's display aspect (w/h when 0)
void* bf_video_begin(unsigned w, unsigned h, int fmt, float aspect, int* pitch);
// show the buffer from bf_video_begin (dupe: the previous picture again) at the next refresh; false if it failed
bool bf_video_commit(bool dupe);
bool bf_video_flip_pending(void);
int bf_video_fd(void);           // poll() it for flip events (-1: none, use bf_video_wait)
void bf_video_events(void);      // after the fd is readable: the flip done
double bf_video_refresh(void);   // Hz
int bf_video_size(int* w, int* h);  // the panel
// the last frame shown, as XRGB8888 (for thumbnails, screenshots): malloc'd, w x h; NULL if none
uint32_t* bf_video_grab(int* w, int* h);
double bf_video_last_flip_ms(void);  // when the last flip happened (bf_now_ms clock)
// the in-game menu's canvas (a panel-sized XRGB8888 buffer on the panel instead of the game; -1 none)
int bf_video_canvas(uint32_t** px, int* pitch, int* w, int* h);
void bf_video_canvas_show(int k);
void bf_video_canvas_done(void);  // the game's picture back
void bf_video_close(void);
