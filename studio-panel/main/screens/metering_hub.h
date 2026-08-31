#pragma once
#include "lvgl.h"

// IN: nothing. OUT: new tile-picker hub screen (not loaded — caller loads it).
lv_obj_t *metering_hub_screen_create();

// Enter a specific meter fullscreen (0-based index into the hub's meter list).
// Loads with slide-up animation. Used internally by tile taps and external callers.
void metering_hub_enter(int meter_idx);

// Exit current fullscreen meter → return to hub. Called from swipe-down handler.
void metering_hub_exit();

// Cycle to next / previous meter fullscreen (called from swipe left/right).
void metering_hub_next();
void metering_hub_prev();
