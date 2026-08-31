#pragma once
#include "lvgl.h"

struct SettingOption {
    const char *label;
    const char *desc = nullptr;  // optional; shown in desc box when this option is selected
};

struct SettingItem {
    const char        *label;
    const SettingOption *options;
    int                 option_count;
    int                *selected; // index into options[]
};

// Creates gear button in right_zone. On tap, opens settings overlay on scr.
// Takes ownership of items[] — will delete[] on button delete.
void settings_btn_create(lv_obj_t *right_zone, lv_obj_t *scr,
                         SettingItem *items, int count);
