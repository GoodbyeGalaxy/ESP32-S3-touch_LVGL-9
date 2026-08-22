#include "display.h"
static esp_lcd_panel_handle_t s_panel = nullptr;
void display_init() {}
esp_lcd_panel_handle_t display_get_panel() { return s_panel; }
