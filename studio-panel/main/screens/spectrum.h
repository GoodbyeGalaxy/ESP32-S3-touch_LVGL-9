#pragma once
#include "lvgl.h"

// IN: nothing. OUT: new screen (not loaded — caller calls lv_screen_load/anim).
lv_obj_t *spectrum_screen_create();
