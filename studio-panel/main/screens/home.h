#pragma once
#include "lvgl.h"

// Erstellt den Home-Screen und gibt ihn zurück.
// Aufrufer lädt ihn mit lv_screen_load() oder lv_screen_load_anim().
lv_obj_t *home_screen_create();

// Convenience: Erstellt + lädt sofort (ohne Animation, für Boot)
void home_screen_load();
