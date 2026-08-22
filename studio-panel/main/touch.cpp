#include "touch.h"
#include "board.h"
#include "ch422g.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_touch = nullptr;

void touch_init()
{
    // GT911 Reset-Sequenz: TOUCH_RST low → 10 ms → TOUCH_RST high
    // LCD_RST und LCD_BL bleiben gesetzt; nur TOUCH_RST wird kurz low gezogen
    ch422g_set(CH422G_LCD_RST | CH422G_LCD_BL);                      // TOUCH_RST = low
    vTaskDelay(pdMS_TO_TICKS(10));
    ch422g_set(CH422G_LCD_RST | CH422G_LCD_BL | CH422G_TOUCH_RST);   // TOUCH_RST = high

    // I2C Panel-IO für GT911 — I2C-Bus läuft bereits durch ch422g_init()
    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr                    = 0x5D;        // GT911 Standard; bei Fehler: 0x14
    io_cfg.scl_speed_hz                = BSP_I2C_FREQ;
    io_cfg.control_phase_bytes         = 1;
    io_cfg.dc_bit_offset               = 0;
    io_cfg.lcd_cmd_bits                = 16;
    io_cfg.lcd_param_bits              = 8;
    io_cfg.flags.disable_control_phase = 1;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(
        (esp_lcd_i2c_bus_handle_t)BSP_I2C_PORT, &io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max              = LCD_H_RES;
    tp_cfg.y_max              = LCD_V_RES;
    tp_cfg.rst_gpio_num       = GPIO_NUM_NC;   // Reset via CH422G, kein direkter GPIO
    tp_cfg.int_gpio_num       = TOUCH_INT_PIN;
    tp_cfg.levels.reset       = 0;
    tp_cfg.levels.interrupt   = 0;
    tp_cfg.flags.swap_xy      = 0;
    tp_cfg.flags.mirror_x     = 0;
    tp_cfg.flags.mirror_y     = 0;

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch));
    ESP_LOGI(TAG, "GT911 initialized");
}

esp_lcd_touch_handle_t touch_get_handle()
{
    return s_touch;
}
