#include "ui.h"
#include "board.h"
#include "display.h"
#include "touch.h"
#include "screens/home.h"
#include "screens/metering.h"
#include "screens/statusbar.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui";

void ui_init()
{
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_stack_size = 16384;
    cfg.stack_in_psram  = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg));

    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
            display_get_panel(), NULL,
            LCD_H_RES, LCD_V_RES,
            ESP_LV_ADAPTER_ROTATE_0);
    disp_cfg.tear_avoid_mode   = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
    disp_cfg.profile.use_psram = true;

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "esp_lv_adapter_register_display failed");
        return;
    }

    // Theme deaktivieren: verhindert dass Theme-Padding den Screen-Rand
    // unbedeckt lässt und PSRAM-Grün durchscheint.
    lv_display_set_theme(disp, nullptr);

    if (touch_get_handle() != nullptr) {
        esp_lv_adapter_touch_config_t touch_cfg =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_get_handle());
        esp_lv_adapter_register_touch(&touch_cfg);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        statusbar_init();
        lv_screen_load(metering_screen_create());
        esp_lv_adapter_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock");
    }

    ESP_LOGI(TAG, "LVGL ready");
}
