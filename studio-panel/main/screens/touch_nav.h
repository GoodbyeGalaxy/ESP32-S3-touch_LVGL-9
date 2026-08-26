#pragma once

#include "lvgl.h"

// Callback type: called with direction (+1 = right/forward, -1 = left/back)
using SwipeCallback = void(*)(int direction, void *user_data);

// Long-press callback: called once after threshold, with press position.
using LongPressCallback = void(*)(lv_point_t pos, void *user_data);

// Attach swipe detection to an lv_obj (typically the screen root).
// min_px: minimum horizontal displacement to trigger (default 80).
// Uses LVGL 9 gesture detection under the hood — no CLICKABLE flag is set on obj,
// so child widgets keep receiving their own CLICKED events. Gesture events bubble
// from children up to obj via LV_OBJ_FLAG_GESTURE_BUBBLE (LVGL default).
// Callback runs inside the LVGL task — safe to call lv_* directly.
void touch_nav_attach(lv_obj_t *obj, SwipeCallback cb, void *user_data, int min_px = 80);

// Attach long-press detector to obj (obj must be CLICKABLE — tiles, buttons, etc).
// threshold_ms: how long to hold (default 600ms). long_cb fires once per press
// after threshold. Normal CLICKED still fires on release; callsite should
// track whether long-press already fired if it wants to suppress the click.
// State is auto-freed when obj is deleted.
void touch_nav_attach_long_press(lv_obj_t *obj, LongPressCallback long_cb,
                                 void *user_data, uint32_t threshold_ms = 600);

// Remove all touch_nav handlers (swipe + long-press) attached to obj and free
// their state. Safe to call multiple times. State is also auto-freed on obj delete,
// so calling this before delete is only needed to disable detection early.
void touch_nav_detach(lv_obj_t *obj);
