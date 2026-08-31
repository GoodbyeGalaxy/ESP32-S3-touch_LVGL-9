#include "audio_data.h"

static_assert(sizeof(AudioPacket) == 1080, "AudioPacket size mismatch — check struct layout");

QueueHandle_t g_audio_queue = nullptr;
