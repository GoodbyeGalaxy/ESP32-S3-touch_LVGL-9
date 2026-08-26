#include "display.h"
#include "board.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_log.h"

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel = nullptr;

void display_init(uint8_t num_fbs)
{
    constexpr int data_pins[] = LCD_DATA_PINS;
    static_assert(sizeof(data_pins) / sizeof(data_pins[0]) == 16,
                  "LCD_DATA_PINS must have exactly 16 entries for RGB565");

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
    cfg.num_fbs                      = num_fbs;
    cfg.bounce_buffer_size_px        = LCD_H_RES * 6;  // 6 Lines — Kompromiss: mehr Puffer ohne DRAM-Engpass
    cfg.psram_trans_align            = 64;
    cfg.hsync_gpio_num               = LCD_PIN_HSYNC;
    cfg.vsync_gpio_num               = LCD_PIN_VSYNC;
    cfg.de_gpio_num                  = LCD_PIN_DE;
    cfg.pclk_gpio_num                = LCD_PIN_PCLK;
    cfg.disp_gpio_num                = -1;
    cfg.flags.fb_in_psram            = 1;

    for (size_t i = 0; i < sizeof(data_pins) / sizeof(data_pins[0]); ++i)
        cfg.data_gpio_nums[i] = data_pins[i];

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    // Alle Framebuffer mit Hintergrundfarbe vorbelegen und in physisches PSRAM flushen.
    // Verhindert PSRAM-Initialisierungsartefakte (0x07E0 grün) als Fallback-Inhalt.
    // RGB565 für 0x282828: R=5, G=10, B=5 → 0x2945
    {
        void *fbs[3] = {};
        if (num_fbs == 1) esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fbs[0]);
        if (num_fbs == 2) esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fbs[0], &fbs[1]);
        if (num_fbs >= 3) esp_lcd_rgb_panel_get_frame_buffer(s_panel, 3, &fbs[0], &fbs[1], &fbs[2]);

        const size_t fb_bytes = (size_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
        for (uint8_t i = 0; i < num_fbs; i++) {
            if (!fbs[i]) continue;
            uint16_t *p = static_cast<uint16_t *>(fbs[i]);
            for (size_t px = 0; px < (size_t)LCD_H_RES * LCD_V_RES; px++) p[px] = 0x630C;
            esp_cache_msync(fbs[i], fb_bytes,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
        ESP_LOGI(TAG, "Frame buffers pre-filled (%u × %zu KB)", num_fbs, fb_bytes / 1024);
    }

    ESP_LOGI(TAG, "RGB panel ready %dx%d @ %lu Hz", LCD_H_RES, LCD_V_RES, (unsigned long)LCD_PCLK_HZ);
}

esp_lcd_panel_handle_t display_get_panel()
{
    return s_panel;
}
