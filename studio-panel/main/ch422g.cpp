// CH422G IO-Expander — neue I2C-Master-API (IDF 5.x)
//
// Der CH422G nutzt ein ungewöhnliches Zwei-Adressen-Protokoll:
//   - Schreibe 1 Byte an Slave-Adresse 0x24 → Output-Enable-Register
//   - Schreibe 1 Byte an Slave-Adresse 0x38 → Output-Daten-Register
// Jede "Register-Adresse" ist eine eigene I2C-Slave-Adresse, kein Sub-Register.
//
// Quelle: Waveshare ESP32-S3-Touch-LCD-7 Demo (09_lvgl_v9_demo/components/ch422g)

#include "ch422g.h"
#include "board.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "ch422g";

// Geteilter I2C-Bus-Handle — wird von touch.cpp über ch422g_init() abgerufen
static i2c_master_bus_handle_t s_i2c_bus = nullptr;

// Temporäre Device-Handles für OE- und Data-Register
static i2c_master_dev_handle_t s_dev_oe  = nullptr;
static i2c_master_dev_handle_t s_dev_out = nullptr;

// ── Hilfsfunktion: 1 Byte an CH422G senden ────────────────────────────────

static esp_err_t ch422g_write(i2c_master_dev_handle_t dev, uint8_t val)
{
    return i2c_master_transmit(dev, &val, 1, /*timeout_ms=*/20);
}

// ── Öffentliche API ────────────────────────────────────────────────────────

void ch422g_i2c_scan(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "I2C scan 0x01–0x7F ...");
    for (uint8_t addr = 0x01; addr < 0x7F; addr++) {
        if (i2c_master_probe(bus, addr, /*timeout_ms=*/10) == ESP_OK) {
            ESP_LOGI(TAG, "  found: 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "I2C scan done");
}

i2c_master_bus_handle_t ch422g_init()
{
    // ── I2C Bus Recovery: 9× SCL toggelн, befreit festgehaltene SDA ──────
    // Standard I2C recovery sequence (NXP UM10204 §3.1.16)
    gpio_set_direction(BSP_I2C_SCL, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction(BSP_I2C_SDA, GPIO_MODE_INPUT);
    gpio_set_level(BSP_I2C_SCL, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    for (int i = 0; i < 9; i++) {
        gpio_set_level(BSP_I2C_SCL, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(BSP_I2C_SCL, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
        if (gpio_get_level(BSP_I2C_SDA)) break;  // SDA frei → Bus recovered
    }
    // STOP condition: SDA low → high während SCL high
    gpio_set_direction(BSP_I2C_SDA, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(BSP_I2C_SDA, 0);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(BSP_I2C_SDA, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    // Pins freigeben, damit I2C-Master-Treiber sie übernehmen kann
    gpio_reset_pin(BSP_I2C_SCL);
    gpio_reset_pin(BSP_I2C_SDA);
    ESP_LOGI(TAG, "I2C bus recovery complete (SDA=%s)",
             gpio_get_level(BSP_I2C_SDA) ? "high=OK" : "still-low=HW-problem");

    // ── I2C Bus konfigurieren ──────────────────────────────────────────────
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port      = BSP_I2C_PORT;
    bus_cfg.sda_io_num    = BSP_I2C_SDA;
    bus_cfg.scl_io_num    = BSP_I2C_SCL;
    bus_cfg.clk_source    = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
    ESP_LOGI(TAG, "I2C master bus created (SDA=%d SCL=%d)", BSP_I2C_SDA, BSP_I2C_SCL);

    // ── CH422G Device: OE-Register (Slave-Adresse 0x24) ───────────────────
    i2c_device_config_t dev_oe_cfg = {};
    dev_oe_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_oe_cfg.device_address  = CH422G_OE_ADDR;
    dev_oe_cfg.scl_speed_hz    = BSP_I2C_FREQ;

    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_oe_cfg, &s_dev_oe));

    // ── CH422G Device: Data-Register (Slave-Adresse 0x38) ─────────────────
    i2c_device_config_t dev_out_cfg = {};
    dev_out_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_out_cfg.device_address  = CH422G_OUT_ADDR;
    dev_out_cfg.scl_speed_hz    = BSP_I2C_FREQ;

    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_out_cfg, &s_dev_out));

    // ── Output Enable setzen (CH422G Schritt 1) ───────────────────────────
    // Schreibe 0x01 an Adresse 0x24 → aktiviert alle IOs als Output
    esp_err_t err = ch422g_write(s_dev_oe, 0x01);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH422G OE write failed: %s — prüfe I2C-Bus und Pull-ups",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CH422G output enable OK");
    }

    return s_i2c_bus;
}

void ch422g_backlight_on()
{
    // 0x1E = 0b00011110 → BL=1, LCD_RST=1, TP_RST=1, alle Resets freigegeben
    esp_err_t err = ch422g_write(s_dev_out, CH422G_VAL_BL_ON);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CH422G backlight_on failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Backlight ON");
    }
}

void ch422g_touch_reset()
{
    // GT911 Reset-Sequenz (aus Waveshare-Demo):
    //   1. Touch-RST low halten (CH422G Data 0x2C)
    //   2. GPIO4 (INT-Pin) auf Low ziehen → wählt GT911 I2C-Adresse 0x5D
    //   3. Touch-RST high (CH422G Data 0x2E) → GT911 bootet mit Adresse 0x5D
    //   4. GPIO4 wieder als Eingang freigeben

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask  = 1ULL << TOUCH_INT_PIN;
    io_conf.mode          = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    // RST low → Reset aktiv, BL bleibt an
    ch422g_write(s_dev_out, CH422G_VAL_TOUCH_RST_L);
    vTaskDelay(pdMS_TO_TICKS(100));

    // INT-Pin auf Low → GT911 wählt Adresse 0x5D
    gpio_set_level(TOUCH_INT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // RST high → GT911 bootet
    ch422g_write(s_dev_out, CH422G_VAL_TOUCH_RST_H);
    vTaskDelay(pdMS_TO_TICKS(200));

    // GPIO4 wieder als Eingang für Interrupt-Betrieb
    io_conf.mode      = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "GT911 reset sequence complete (addr 0x5D)");
}
