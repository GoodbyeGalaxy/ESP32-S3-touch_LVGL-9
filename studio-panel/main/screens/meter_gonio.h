#pragma once
#include "lvgl.h"

// Lissajous goniometer — plots L vs R samples with persistence fade.
// IN: nothing. OUT: new screen (not loaded).
lv_obj_t *meter_gonio_screen_create();
