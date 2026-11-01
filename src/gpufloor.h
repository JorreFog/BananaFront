// gpufloor.h: a floor under the GPU's clock while a shader runs (gpufloor.c)
#pragma once
#include <stdbool.h>

bool bf_gpufloor_start(void);   // false: no GPU devfreq (PC)
void bf_gpufloor_missed(void);  // a flip a refresh late with the frame on time
void bf_gpufloor_tick(void);    // once a loop: the step down after a calm while
unsigned bf_gpufloor_mhz(void);
void bf_gpufloor_stop(void);
