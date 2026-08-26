#pragma once
#include "lvgl.h"

// Temporary brightness calibration screen.
// IN: nothing. OUT: new screen lv_obj_t* (not loaded — caller calls lv_screen_load/anim).
lv_obj_t *gradient_test_screen_create();
