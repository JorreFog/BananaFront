// frontend.h: what main.c's loop hands to the rest of BananaFront (frontend.c: game-id, the companion export,
// screenshots, state thumbnails, pop-ups; menu.c: the in-game menu; ra.c: RetroAchievements; cpugov.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "bf.h"

enum { BF_EV_STATE_SAVED = 1, BF_EV_STATE_LOADED, BF_EV_STATE_FAILED, BF_EV_STATE_MISSING, BF_EV_SLOT, BF_EV_SHOT };

extern bf_paths g_paths;

void frontend_init(const char* system, const char* rom);  // after the game loaded and the panel opened
void frontend_frame(void);       // after each refresh's core frames (the consistent point: services the companion)
void frontend_quit(void);        // before the core is closed
void frontend_menu(void);        // F + X, MENU_TOGGLE
void frontend_screenshot(void);  // F + SELECT, SCREENSHOT
void frontend_fps_toggle(void);  // F + Y
void frontend_event(int ev, int arg);  // a state saved/loaded, the slot changed... (a pop-up, a log line)

uint32_t* bf_frame_rgb(unsigned* w, unsigned* h);  // main.c: the last picture as XRGB8888, malloc'd
void bf_command(const char* line);                  // main.c: one of RetroArch's command words
