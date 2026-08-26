#pragma once
#include "lvgl.h"

// Creates foot bar on scr at Y=THEME_FOOT_Y, H=THEME_FOOT_H.
// Adds ← Home button on the left.
// IN: scr — parent screen. OUT: right_zone lv_obj_t* for screen-specific actions.
// Screen-specific: add buttons/labels as children of the returned right_zone.
lv_obj_t *foot_create(lv_obj_t *scr);
