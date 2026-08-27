#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <cstdint>

enum class TransportState : uint8_t { Stopped, Playing, Recording, Paused };

// Transport state received from the Mac-side Studio One helper via WebSocket.
struct StudioOneState {
    float          bpm;        // 20..400 BPM; 0.0f = unknown
    char           timesig[8]; // e.g. "4/4", null-terminated
    char           pos[16];    // e.g. "001.01.000", null-terminated
    TransportState state;
};

// Length-1 overwrite queue — ws_client writes, studio_one screen reads via xQueuePeek.
extern QueueHandle_t g_studio_one_queue;
