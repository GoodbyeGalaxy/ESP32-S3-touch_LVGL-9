#pragma once
#include "lvgl.h"

// M/S Meter — Mid and Side VU-style bars with numeric readout.
// IN: nothing. OUT: new screen (not loaded).
lv_obj_t *meter_ms_screen_create();
