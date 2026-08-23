#pragma once
#include "esp_lcd_panel_ops.h"
void display_init(uint8_t num_fbs);
esp_lcd_panel_handle_t display_get_panel();
