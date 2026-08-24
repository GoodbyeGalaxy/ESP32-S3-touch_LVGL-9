#pragma once

#include "lvgl.h"

// Callback type: called with direction (+1 = right/forward, -1 = left/back)
using SwipeCallback = void(*)(int direction, void *user_data);

// Attach swipe detection to an lv_obj (typically the screen root).
// min_px: minimum horizontal displacement to trigger (default 80).
// Swipe callbacks run inside the LVGL task — safe to call lv_* directly.
void touch_nav_attach(lv_obj_t *obj, SwipeCallback cb, void *user_data, int min_px = 80);

// Remove all swipe handlers attached to obj (call before screen delete).
void touch_nav_detach(lv_obj_t *obj);
