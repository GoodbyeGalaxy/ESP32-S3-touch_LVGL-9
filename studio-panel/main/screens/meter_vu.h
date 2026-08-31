#pragma once
#include "lvgl.h"

// Dedicated VU meter screen — two full-size analog VU panels (L+R).
// Uses MeterEngine for VU ballistics + SkinVU for rendering.
// IN: nothing. OUT: new screen (not loaded).
lv_obj_t *meter_vu_screen_create();
