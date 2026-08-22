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
        .task_stack        = 8192,
        .task_affinity     = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms   = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = nullptr,
        .panel_handle   = display_get_panel(),
        .control_handle = nullptr,
        .buffer_size    = LCD_H_RES * LCD_V_RES,
        .double_buffer  = true,
        .trans_size     = 0,
        .hres           = LCD_H_RES,
        .vres           = LCD_V_RES,
        .monochrome     = false,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .rounder_cb = nullptr,
#if LVGL_VERSION_MAJOR >= 9
        .color_format  = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma     = false,
            .buff_spiram  = true,
            .sw_rotate    = false,
            .full_refresh = false,
            .direct_mode  = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode       = true,   // bounce_buffer_size_px ist gesetzt
            .avoid_tearing = true,   // num_fbs=2
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_rgb failed");
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

    // Backlight erst einschalten wenn erster Frame bereit — kein weißer Blitz
    ch422g_set(CH422G_LCD_RST | CH422G_LCD_BL | CH422G_TOUCH_RST);

    ESP_LOGI(TAG, "LVGL ready");
}
