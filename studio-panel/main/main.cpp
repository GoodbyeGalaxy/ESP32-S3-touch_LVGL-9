#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
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
    // Backlight + Resets aktivieren
    ch422g_set(CH422G_LCD_RST | CH422G_LCD_BL | CH422G_TOUCH_RST);

    display_init();

    // SMOKE TEST - entfernen nach Verifikation
    {
        void *fb = nullptr;
        esp_lcd_rgb_panel_get_frame_buffer(display_get_panel(), 1, &fb);
        if (fb) {
            uint16_t *buf = static_cast<uint16_t *>(fb);
            for (int i = 0; i < LCD_H_RES * 40; i++) buf[i] = 0xF800;  // RGB565 Rot
        }
    }

    touch_init();
    ui_init();

    ESP_LOGI(TAG, "Boot complete.");
}
