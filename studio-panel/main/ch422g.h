#pragma once

#include "driver/i2c_master.h"

// I2C-Bus initialisieren + CH422G Output Enable setzen.
// Gibt den Bus-Handle zurück, den touch.cpp für esp_lcd_new_panel_io_i2c_v2 benötigt.
i2c_master_bus_handle_t ch422g_init();

// Backlight + alle Resets freigeben (0x1E ans Data-Register)
void ch422g_backlight_on();

// I2C Bus-Scan: gibt alle antwortenden Adressen per ESP_LOGI aus
void ch422g_i2c_scan(i2c_master_bus_handle_t bus);

// GT911 Reset-Sequenz: wählt I2C-Adresse 0x5D
// GPIO4 wird als Output für die Sequenz genutzt, danach wieder als Input freigegeben.
void ch422g_touch_reset();
