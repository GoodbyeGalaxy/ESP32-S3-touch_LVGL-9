#include "ch422g.h"
#include "board.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "ch422g";
static uint8_t s_state = 0;

void ch422g_init()
{
    i2c_config_t cfg = {};
    cfg.mode             = I2C_MODE_MASTER;
    cfg.sda_io_num       = BSP_I2C_SDA;
    cfg.scl_io_num       = BSP_I2C_SCL;
    cfg.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    cfg.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    cfg.master.clk_speed = BSP_I2C_FREQ;

    ESP_ERROR_CHECK(i2c_param_config(BSP_I2C_PORT, &cfg));
    ESP_ERROR_CHECK(i2c_driver_install(BSP_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    ch422g_set(0);   // alle Ausgänge low (Backlight aus, Reset aktiv-low)
    ESP_LOGI(TAG, "I2C + CH422G initialized");
}

void ch422g_set(uint8_t mask)
{
    s_state = mask;
    esp_err_t err = i2c_master_write_to_device(
        BSP_I2C_PORT, CH422G_I2C_ADDR,
        &s_state, 1,
        pdMS_TO_TICKS(20));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH422G write failed: %s", esp_err_to_name(err));
    }
}

void ch422g_set_bits(uint8_t bits)
{
    ch422g_set(s_state | bits);
}

void ch422g_clear_bits(uint8_t bits)
{
    ch422g_set(s_state & ~bits);
}
