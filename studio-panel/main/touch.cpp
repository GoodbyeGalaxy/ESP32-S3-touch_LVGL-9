#include "touch.h"
static esp_lcd_touch_handle_t s_touch = nullptr;
void touch_init() {}
esp_lcd_touch_handle_t touch_get_handle() { return s_touch; }
