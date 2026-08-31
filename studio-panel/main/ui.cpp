#include "ui.h"
#include "board.h"
#include "display.h"
#include "touch.h"
#include "screens/home.h"
#include "screens/metering.h"
#include "screens/statusbar.h"
#include "screens/nav_controller.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "ui";

static void time_tick_cb(lv_timer_t *)
{
    time_t now;
    time(&now);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year > 70) {  // synced (year > 1970)
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        statusbar_update_time(buf);
    }
}

void ui_init()
{
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_stack_size = 20480;   // 20 KB in DRAM — PSRAM stack competed with canvas DMA
    cfg.stack_in_psram  = false;
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
        nav_init();
        home_screen_load();
        lv_timer_create(time_tick_cb, 1000, nullptr);
        esp_lv_adapter_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock");
    }

    ESP_LOGI(TAG, "LVGL ready");
}
