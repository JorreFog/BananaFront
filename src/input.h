// input.h: evdev pads (input.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// a hotkey: F + a button (its RETRO_DEVICE_ID_JOYPAD_* id), F + a volume key, or F alone (pressed and let go)
enum { BF_HK_ALONE = 100, BF_HK_VOLUP, BF_HK_VOLDOWN };
// a button event the frontend may act on: down with f (F held: a hotkey), down without f for a button in the job
// mask (Settings > Other systems > Buttons: L2/R2/L3/R3 alone), and every release (hold-to-fast-forward)
typedef void (*bf_input_hotkey_fn)(int id, bool down, bool f);

void bf_input_open(bf_input_hotkey_fn fn);
int bf_input_fds(int* fds, int max);
void bf_input_read(int fd);
uint32_t bf_input_buttons(void);  // the pad's buttons now (RETRO_DEVICE_ID_JOYPAD_* bits)
bool bf_input_hotkey_held(void);
// buttons the game doesn't see (their own job instead), RETRO_DEVICE_ID_JOYPAD_* bits
void bf_input_set_job_mask(uint32_t mask);
// player 1's pad as Settings > Other systems > Buttons > the system has it (run-retroarch gives RetroArch the same):
// Swap A/B and X/Y (the game's A on the pad's B ...; the hotkeys stay on the buttons as printed), and the left stick as the d-pad
void bf_input_set_pad(bool swap_ab, bool stick_dpad);
// the menu: raw evdev key events (code, value 1 down / 0 up; the hat as BTN_DPAD_*) queued instead of played
void bf_input_raw(bool on);
bool bf_input_raw_take(int* code, int* value);
void bf_input_hotplug_tick(void);  // between frames: pads that appeared, opened
void bf_input_close(void);
