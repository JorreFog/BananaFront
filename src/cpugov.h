// cpugov.h: the clock governor (cpugov.c).
#pragma once
#include <stdbool.h>

bool bf_cpugov_start(const char* game, double refresh_hz);  // false: no cpufreq (PC)
void bf_cpugov_frame(double work_ms, bool late);            // each refresh's work, and whether it missed the refresh
void bf_cpugov_tick(void);                                  // once a loop: step-down decisions once a second
void bf_cpugov_boost(int ms);
unsigned bf_cpugov_mhz(void);                               // the clock it set (0: off)
void bf_cpugov_stop(void);
