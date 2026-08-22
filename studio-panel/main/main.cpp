#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
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

    // Framebuffer via draw_bitmap vorbelegen (gleicher Weg wie LVGL-Flush)
    {
        const uint16_t bg = 0xFFFF;  // DIAGNOSE: weiß
        const int strip_h = 40;
        uint16_t *strip = static_cast<uint16_t *>(
            heap_caps_malloc(LCD_H_RES * strip_h * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (strip) {
            for (int i = 0; i < LCD_H_RES * strip_h; i++) strip[i] = bg;
            for (int y = 0; y < LCD_V_RES; y += strip_h) {
                int h = (y + strip_h <= LCD_V_RES) ? strip_h : (LCD_V_RES - y);
                esp_lcd_panel_draw_bitmap(display_get_panel(), 0, y, LCD_H_RES, y + h, strip);
            }
            heap_caps_free(strip);
            ESP_LOGI(TAG, "Framebuffer pre-filled via draw_bitmap");
        } else {
            ESP_LOGE(TAG, "draw_bitmap strip alloc failed");
        }
    }

    touch_init();
    ui_init();

    ESP_LOGI(TAG, "Boot complete.");
}
