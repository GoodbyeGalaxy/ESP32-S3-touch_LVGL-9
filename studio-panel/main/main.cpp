#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_rgb.h"
#include "board.h"
#include "ch422g.h"
#include "display.h"
#include "touch.h"
#include "ui.h"

static const char *TAG = "main";

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Studio Panel booting...");

    ch422g_init();
    // RST-Pins aktivieren; Backlight kommt erst nach LVGL-Init (ui_init)
    ch422g_set(CH422G_LCD_RST | CH422G_TOUCH_RST);

    display_init();

    // Framebuffer mit Hintergrundfarbe vorbelegen bevor LVGL startet
    // verhindert dass der grüne PSRAM-Initialzustand durchscheint
    void *fb = nullptr;
    esp_lcd_rgb_panel_get_frame_buffer(display_get_panel(), 1, &fb);
    if (fb) {
        uint16_t *pixels = static_cast<uint16_t *>(fb);
        const uint16_t bg = 0x0841;  // 0x0A0A0A in RGB565
        for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) pixels[i] = bg;
    }

    touch_init();
    ui_init();

    ESP_LOGI(TAG, "Boot complete.");
}
