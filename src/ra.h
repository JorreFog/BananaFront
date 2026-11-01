// ra.h: RetroAchievements (ra.c, rcheevos rc_client).
#pragma once
#include <stdbool.h>
#include <stdint.h>


// after the game loaded: the account from the environment (run-bananafront); companion: the bcomp producer or NULL
bool ra_init(const char* system, void* companion);
void ra_frame(void);           // after each core frame (main thread)
void ra_idle(void);            // paused (the menu): keeps the server session alive
void ra_reset(void);
void ra_state_loaded(void);
void ra_memory_changed(void);  // the core sent new memory maps
bool ra_hardcore(void);        // hardcore active: no state loads, no fast-forward
int ra_progress(unsigned* unlocked, unsigned* total);
// the in-game menu (menu.c, BananaDS's API): the latest unlocks, newest first, with their cached badges
typedef struct { char title[96], desc[200], badge[600]; unsigned points; long long when; } ra_recent_t;
int ra_recent(ra_recent_t* out, int max);
// the next pop-up to show: 1 and its lines, its badge's file ("" for none), its accent colour and how long, else 0
int ra_popup_take(char* l1, int n1, char* l2, int n2, char* badge, int nb, uint32_t* accent, int* ms);
void bf_popup_push(const char* l1, const char* l2, uint32_t accent, int ms);  // a pop-up of the frontend's own
void ra_quit(void);
