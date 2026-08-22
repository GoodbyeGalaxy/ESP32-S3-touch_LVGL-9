#include "display.h"
#include "board.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel = nullptr;

void display_init()
{
    const int data_pins[] = LCD_DATA_PINS;

    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src                      = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz              = LCD_PCLK_HZ;
    cfg.timings.h_res                = LCD_H_RES;
    cfg.timings.v_res                = LCD_V_RES;
    cfg.timings.hsync_pulse_width    = LCD_HSYNC_PULSE;
    cfg.timings.hsync_back_porch     = LCD_HSYNC_BP;
    cfg.timings.hsync_front_porch    = LCD_HSYNC_FP;
    cfg.timings.vsync_pulse_width    = LCD_VSYNC_PULSE;
    cfg.timings.vsync_back_porch     = LCD_VSYNC_BP;
    cfg.timings.vsync_front_porch    = LCD_VSYNC_FP;
    cfg.timings.flags.pclk_active_neg = 0;
    cfg.data_width                   = 16;
    cfg.bits_per_pixel               = 16;
    cfg.num_fbs                      = 2;
    cfg.bounce_buffer_size_px        = 10 * LCD_H_RES;
    cfg.psram_trans_align            = 64;
    cfg.hsync_gpio_num               = LCD_PIN_HSYNC;
    cfg.vsync_gpio_num               = LCD_PIN_VSYNC;
    cfg.de_gpio_num                  = LCD_PIN_DE;
    cfg.pclk_gpio_num                = LCD_PIN_PCLK;
    cfg.disp_gpio_num                = -1;  // Backlight via CH422G
    cfg.flags.fb_in_psram            = 1;

    for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_pins[i];

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_LOGI(TAG, "RGB panel ready %dx%d @ %lu Hz", LCD_H_RES, LCD_V_RES, (unsigned long)LCD_PCLK_HZ);
}

esp_lcd_panel_handle_t display_get_panel()
{
    return s_panel;
}
