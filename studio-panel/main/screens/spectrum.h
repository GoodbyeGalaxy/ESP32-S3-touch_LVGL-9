#pragma once
#include "lvgl.h"

// Creates the spectrum screen (entry point, View 1 = bars).
// Returns new screen object; caller does NOT call lv_screen_load — done internally.
lv_obj_t *spectrum_screen_create();
