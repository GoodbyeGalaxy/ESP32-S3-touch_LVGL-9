#include "touch.h"
#include "board.h"
#include "ch422g.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_touch = nullptr;

// GT911 hat zwei mögliche Adressen — welche aktiv ist hängt vom INT-Pin beim Reset ab.
// Falls CH422G-Reset fehlschlägt (SDA stuck-low), bleibt die Adresse undefiniert.
// Daher beide probieren: 0x5D zuerst (von uns gewählt via INT=low), dann 0x14 als Fallback.
static const uint8_t GT911_ADDRS[] = { 0x5D, 0x14 };

static esp_lcd_panel_io_handle_t make_tp_io(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
{
    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr                    = addr;
    io_cfg.scl_speed_hz                = BSP_I2C_FREQ;
    io_cfg.control_phase_bytes         = 1;
    io_cfg.dc_bit_offset               = 0;
    io_cfg.lcd_cmd_bits                = 16;
    io_cfg.lcd_param_bits              = 8;
    io_cfg.flags.disable_control_phase = 1;
    esp_lcd_new_panel_io_i2c_v2(i2c_bus, &io_cfg, &tp_io);
    return tp_io;
}

void touch_init(i2c_master_bus_handle_t i2c_bus)
{
    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max        = LCD_H_RES;
    tp_cfg.y_max        = LCD_V_RES;
    tp_cfg.rst_gpio_num = GPIO_NUM_NC;
    tp_cfg.int_gpio_num = GPIO_NUM_NC;
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;

    for (uint8_t addr : GT911_ADDRS) {
        esp_lcd_panel_io_handle_t tp_io = make_tp_io(i2c_bus, addr);
        if (!tp_io) {
            ESP_LOGW(TAG, "panel_io create failed for 0x%02X", addr);
            continue;
        }

        esp_err_t err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "GT911 OK at 0x%02X", addr);
            return;
        }

        ESP_LOGW(TAG, "GT911 at 0x%02X failed (%s)", addr, esp_err_to_name(err));
        esp_lcd_panel_io_del(tp_io);
        s_touch = nullptr;
    }

    ESP_LOGE(TAG, "GT911 not found at 0x5D or 0x14 — no touch");
}

esp_lcd_touch_handle_t touch_get_handle()
{
    return s_touch;
}
