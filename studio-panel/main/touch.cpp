#include "touch.h"
#include "board.h"
#include "ch422g.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_touch = nullptr;

void touch_init(i2c_master_bus_handle_t i2c_bus)
{
    // Reset-Sequenz wurde bereits in main.cpp via ch422g_touch_reset() durchgeführt.
    // Nutze den neuen I2C-Master-API-Handle (kompatibel mit esp_lcd_new_panel_io_i2c_v2).

    esp_lcd_panel_io_handle_t tp_io = nullptr;

    // v2-Variante: nimmt i2c_master_bus_handle_t statt Port-Nummer
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr                    = GT911_I2C_ADDR;
    io_cfg.scl_speed_hz                = BSP_I2C_FREQ;
    io_cfg.control_phase_bytes         = 1;
    io_cfg.dc_bit_offset               = 0;
    io_cfg.lcd_cmd_bits                = 16;
    io_cfg.lcd_param_bits              = 8;
    io_cfg.flags.disable_control_phase = 1;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(i2c_bus, &io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max            = LCD_H_RES;
    tp_cfg.y_max            = LCD_V_RES;
    tp_cfg.rst_gpio_num     = GPIO_NUM_NC;  // Reset via CH422G
    tp_cfg.int_gpio_num     = GPIO_NUM_NC;  // INT-Pin separat konfiguriert
    tp_cfg.levels.reset     = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy    = 0;
    tp_cfg.flags.mirror_x   = 0;
    tp_cfg.flags.mirror_y   = 0;

    esp_err_t err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed (0x%x) — continuing without touch", err);
        s_touch = nullptr;
        return;
    }
    ESP_LOGI(TAG, "GT911 initialized");
}

esp_lcd_touch_handle_t touch_get_handle()
{
    return s_touch;
}
