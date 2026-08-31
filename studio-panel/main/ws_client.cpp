#include "ws_client.h"
#include "studio_one_data.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstring>
#include <cstdio>

static const char *TAG = "ws_client";

QueueHandle_t g_studio_one_queue = nullptr;

static std::atomic<bool> s_running{false};
static std::atomic<bool> s_connected{false};

// Parses a JSON transport message and writes to g_studio_one_queue.
// IN: null-terminated JSON string. OUT: nothing.
// Silently ignores messages with wrong "type" or missing fields.
static void handle_message(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "transport") != 0) {
        cJSON_Delete(root);
        return;
    }

    StudioOneState state = {};
    state.bpm = 0.0f;
    strncpy(state.timesig, "4/4", sizeof(state.timesig));
    strncpy(state.pos, "---", sizeof(state.pos));
    state.state = TransportState::Stopped;

    cJSON *bpm = cJSON_GetObjectItemCaseSensitive(root, "bpm");
    if (cJSON_IsNumber(bpm))
        state.bpm = (float)bpm->valuedouble;

    cJSON *timesig = cJSON_GetObjectItemCaseSensitive(root, "timesig");
    if (cJSON_IsString(timesig))
        strncpy(state.timesig, timesig->valuestring, sizeof(state.timesig) - 1);

    cJSON *pos = cJSON_GetObjectItemCaseSensitive(root, "pos");
    if (cJSON_IsString(pos))
        strncpy(state.pos, pos->valuestring, sizeof(state.pos) - 1);

    cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(st)) {
        if      (strcmp(st->valuestring, "playing")   == 0) state.state = TransportState::Playing;
        else if (strcmp(st->valuestring, "recording") == 0) state.state = TransportState::Recording;
        else if (strcmp(st->valuestring, "paused")    == 0) state.state = TransportState::Paused;
        else                                                 state.state = TransportState::Stopped;
    }

    xQueueOverwrite(g_studio_one_queue, &state);
    cJSON_Delete(root);
}

// WebSocket event handler — runs in the websocket client task.
// IN: event_id + event_data from esp_websocket_client. OUT: nothing.
static void ws_event_handler(void * /*arg*/, esp_event_base_t /*base*/,
                             int32_t event_id, void *event_data)
{
    auto *data = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Connected to %s:%d", CONFIG_WS_HOST, CONFIG_WS_PORT);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Disconnected — client will reconnect automatically");
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 1 && data->data_ptr && data->data_len > 0)
            handle_message(static_cast<const char*>(data->data_ptr), data->data_len);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

// WebSocket client task — creates the client, starts it, then suspends forever.
// Reconnect is handled internally by esp_websocket_client (auto_reconnect=true).
// IN: unused task arg. OUT: nothing (task never returns normally).
static void ws_task(void *)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "ws://%s:%d/studio-one", CONFIG_WS_HOST, CONFIG_WS_PORT);

    esp_websocket_client_config_t cfg = {};
    cfg.uri            = uri;
    cfg.reconnect_timeout_ms  = 5000;
    cfg.network_timeout_ms    = 10000;

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_websocket_client_init() failed");
        s_running = false;
        vTaskDelete(nullptr);
        return;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, nullptr);
    esp_websocket_client_start(client);

    ESP_LOGI(TAG, "Connecting to %s", uri);

    // Task persists — client handles reconnect internally.
    while (true) vTaskDelay(pdMS_TO_TICKS(10000));
}

// Idempotent: only starts the task if not already running.
// Safe to call from WiFi event handler on reconnect.
void ws_client_start()
{
    if (s_running.exchange(true)) return;
    if (!g_studio_one_queue)
        g_studio_one_queue = xQueueCreate(1, sizeof(StudioOneState));
    xTaskCreatePinnedToCore(ws_task, "ws_client", 4096, nullptr, 4, nullptr, 1);
}

bool ws_client_connected() { return s_connected.load(); }
