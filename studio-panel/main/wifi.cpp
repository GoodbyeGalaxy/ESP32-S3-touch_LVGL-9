#include "wifi.h"
#include "net_receiver.h"
#include "screens/statusbar.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include <atomic>
#include <cstring>

static const char *TAG = "wifi";
static std::atomic<bool> s_connected{false};

// Carries IP string for connected state; heap-allocated, freed after use.
static void wifi_statusbar_async(void *arg)
{
    if (arg) {
        statusbar_update_wifi(true, static_cast<const char*>(arg));
        free(arg);
    } else {
        statusbar_update_wifi(false);
    }
}

// Handles both WIFI_EVENT (disconnect) and IP_EVENT (got-IP) in a single callback.
// Runs in the system event-loop task — must NOT touch LVGL widgets directly.
// Widget updates are posted via lv_async_call to be executed in the LVGL task.
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        lv_async_call(wifi_statusbar_async, nullptr);  // nullptr = disconnected
        ESP_LOGW(TAG, "Disconnected — reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        auto *event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        // Heap-alloc IP string — freed in wifi_statusbar_async after display
        char *ip_buf = static_cast<char*>(malloc(16));
        if (ip_buf) snprintf(ip_buf, 16, IPSTR, IP2STR(&event->ip_info.ip));
        lv_async_call(wifi_statusbar_async, ip_buf);
        net_receiver_start();  // safe to call multiple times — idempotent
        // Start SNTP time sync (idempotent — safe to call again on reconnect)
        if (!esp_sntp_enabled()) {
            setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
            tzset();
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
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

void wifi_get_ip(char *buf, size_t len)
{
    if (!s_connected.load()) { snprintf(buf, len, "--"); return; }
    esp_netif_t *netif = esp_netif_get_default_netif();
    if (!netif) { snprintf(buf, len, "--"); return; }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    } else {
        snprintf(buf, len, "--");
    }
}

void wifi_reconnect()
{
    if (!s_connected.load()) {
        esp_wifi_connect();
    }
}
