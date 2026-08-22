#include "ui.h"
#include "board.h"
#include "display.h"
#include "touch.h"
#include "screens/home.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "ui";
static lv_display_t *s_disp = nullptr;

void ui_init()
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority     = 4,
        .task_stack_size   = 8192,
        .task_affinity     = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms   = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = nullptr,
        .panel_handle  = display_get_panel(),
        .buffer_size   = LCD_H_RES * LCD_V_RES,
        .double_buffer = true,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma     = false,
            .buff_spiram  = true,
            .sw_rotate    = false,
            .full_refresh = false,
            .direct_mode  = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    // Touch-Eingabe bei LVGL registrieren
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = s_disp,
        .handle = touch_get_handle(),
    };
    lvgl_port_add_touch(&touch_cfg);

    lvgl_port_lock(0);
    home_screen_create();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL ready");
}
