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
void frontend_core_frame(void);  // after each retro_run (RetroAchievements evaluate every emulated frame)
void frontend_paused(void);      // while paused or in the menu, a few times a second
void frontend_mmap_changed(void);
void frontend_quit(void);
void frontend_border(const char* png, int x, int y, int w, int h);
void frontend_menu_closed(void);
void frontend_overlay_pause(bool p);        // before the core is closed
void frontend_menu(void);        // F + X, MENU_TOGGLE
void frontend_screenshot(void);
void frontend_panel_grab(void);  // GRAB_PANEL: the game plane as shown (after the shader)  // F + SELECT, SCREENSHOT
void frontend_fps_toggle(void);  // F + Y
void frontend_event(int ev, int arg);  // a state saved/loaded, the slot changed... (a pop-up, a log line)

uint32_t* bf_frame_rgb(unsigned* w, unsigned* h);  // main.c: the last picture as XRGB8888, malloc'd
void bf_command(const char* line);                  // main.c: one of RetroArch's command words
void bf_commands_poll(void);                        // main.c: the command pipe read (the menu's loop calls it)
bool bf_state_save(int slot);                       // main.c: RetroArch's slots; -1 the automatic one, -2 the menu's undo
bool bf_state_load(int slot);
void bf_quit(bool nosave);
bool bf_quitting(void);
bool bf_resume_on(void);
const char* bf_rom(void);
