#include "wifi.h"
#include "net_receiver.h"
#include "screens/statusbar.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include <atomic>
#include <cstring>

static const char *TAG = "wifi";
static std::atomic<bool> s_connected{false};

// lv_async_call callback — runs in LVGL task, safe to touch widgets.
// arg encodes state: (void*)1 = connected, nullptr = disconnected.
static void wifi_statusbar_async(void *arg)
{
    statusbar_update_wifi(arg != nullptr);
}

// Handles both WIFI_EVENT (disconnect) and IP_EVENT (got-IP) in a single callback.
// Runs in the system event-loop task — must NOT touch LVGL widgets directly.
// Widget updates are posted via lv_async_call to be executed in the LVGL task.
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        lv_async_call(wifi_statusbar_async, nullptr);
        ESP_LOGW(TAG, "Disconnected — reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        lv_async_call(wifi_statusbar_async, (void*)1);
        auto *event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        net_receiver_start();  // safe to call multiple times — idempotent
    }
}

// Non-blocking: initialises NVS, event loop, WiFi stack, registers handlers.
// Connection result is delivered asynchronously via the event loop.
void wifi_init()
{
    // NVS may be dirty after OTA or version change — erase and re-init.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS dirty (err=0x%x) — erasing and reinitialising", err);
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register handler for both WIFI_EVENT and IP_EVENT families.
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_cfg = {};
    strncpy((char*)wifi_cfg.sta.ssid,     CONFIG_ESP_WIFI_SSID,     sizeof(wifi_cfg.sta.ssid));
    strncpy((char*)wifi_cfg.sta.password, CONFIG_ESP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    // Require at least WPA2 — rejects open or WEP networks.
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Kick off the first connection attempt; retries happen in event handler.
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting to '%s'", CONFIG_ESP_WIFI_SSID);
}

// Reads atomic flag set by IP_EVENT handler — safe from any task/ISR context.
bool wifi_is_connected() { return s_connected.load(); }
