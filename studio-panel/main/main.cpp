#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "board.h"
#include "ch422g.h"
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "esp_lv_adapter.h"
#include "audio_data.h"
#include "wifi.h"
#include "usb_midi_driver.h"

static const char *TAG = "main";

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Studio Panel booting... APP_BUILD=" __DATE__ " " __TIME__);

    uint8_t num_fbs = esp_lv_adapter_get_required_frame_buffer_count(
        ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        ESP_LV_ADAPTER_ROTATE_0);
    ESP_LOGI(TAG, "esp_lvgl_adapter requires %u frame buffer(s)", num_fbs);

    i2c_master_bus_handle_t i2c_bus = ch422g_init();
    ch422g_i2c_scan(i2c_bus);

    display_init(num_fbs);
    ch422g_touch_reset();
    touch_init(i2c_bus);
    ch422g_backlight_on();

    g_audio_queue = xQueueCreate(1, sizeof(AudioPacket));
    configASSERT(g_audio_queue != nullptr);

    wifi_init();        // non-blocking; net_receiver starts when IP assigned
    usb_midi_driver_init(); // USB MIDI Class Device (TinyUSB, own task on Core 1)
    ui_init();

    ESP_LOGI(TAG, "Boot complete.");
}
