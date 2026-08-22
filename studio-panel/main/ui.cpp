#include "ui.h"
#include "board.h"
#include "display.h"
#include "touch.h"
#include "ch422g.h"
#include "screens/home.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "ui";
static lv_display_t *s_disp = nullptr;

void ui_init()
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority     = 4,
        .task_stack        = 16384,
        .task_affinity     = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms   = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = nullptr,
        .panel_handle   = display_get_panel(),
        .control_handle = nullptr,
        .buffer_size    = LCD_H_RES * 40,   // kleiner Puffer, stabiler
        .double_buffer  = false,
        .trans_size     = 0,
        .hres           = LCD_H_RES,
        .vres           = LCD_V_RES,
        .monochrome     = false,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
#if LVGL_VERSION_MAJOR >= 9
        .color_format  = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma     = false,
            .buff_spiram  = true,
            .sw_rotate    = false,
            .full_refresh = false,
            .direct_mode  = false,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode       = false,
            .avoid_tearing = false,
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_rgb failed");
        return;
    }
    lv_display_set_default(s_disp);
    lv_display_set_bg_color(s_disp, lv_color_hex(0x0A0A0A));
    lv_display_set_bg_opa(s_disp, LV_OPA_COVER);

    // Touch nur registrieren wenn erfolgreich initialisiert
    if (touch_get_handle() != nullptr) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = s_disp,
            .handle = touch_get_handle(),
        };
        lvgl_port_add_touch(&touch_cfg);
    } else {
        ESP_LOGW(TAG, "Touch not available, skipping registration");
    }

    ESP_LOGI(TAG, "Acquiring LVGL lock...");
    bool locked = lvgl_port_lock(5000);
    ESP_LOGI(TAG, "Lock acquired: %d", locked);
    if (locked) {
        ESP_LOGI(TAG, "Creating home screen...");
        home_screen_create();
        ESP_LOGI(TAG, "Home screen created");
        lvgl_port_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock");
    }

    ch422g_set(CH422G_LCD_RST | CH422G_LCD_BL | CH422G_TOUCH_RST);
    ESP_LOGI(TAG, "LVGL ready");
}
