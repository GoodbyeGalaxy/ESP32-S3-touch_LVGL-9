#include "audio_data.h"

static_assert(sizeof(AudioPacket) == 1072, "AudioPacket size mismatch — check struct layout");

QueueHandle_t g_audio_queue = nullptr;
