#pragma once
#include "esp_lcd_touch.h"
#include "driver/i2c_master.h"

// i2c_bus: geteilter Bus-Handle aus ch422g_init()
void touch_init(i2c_master_bus_handle_t i2c_bus);
esp_lcd_touch_handle_t touch_get_handle();
