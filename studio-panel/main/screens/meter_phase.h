#pragma once
#include "lvgl.h"

// Phase Correlation meter — large horizontal bar + 60s history.
// IN: nothing. OUT: new screen (not loaded).
lv_obj_t *meter_phase_screen_create();
