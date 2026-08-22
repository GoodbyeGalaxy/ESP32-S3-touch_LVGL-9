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
    touch_init();
    ui_init();

    ESP_LOGI(TAG, "Boot complete.");
}
