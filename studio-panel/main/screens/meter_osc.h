#pragma once
#include "lvgl.h"

// Oscilloscope meter — L+R waveform, time domain.
// IN: nothing. OUT: new screen (not loaded).
lv_obj_t *meter_osc_screen_create();
