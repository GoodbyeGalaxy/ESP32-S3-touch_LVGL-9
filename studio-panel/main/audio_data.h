#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Binary UDP packet received from companion script. All floats little-endian.
// Total size must equal 1080 bytes — verified by static_assert in audio_data.cpp.
struct AudioPacket {
    uint8_t  magic;       // must be 0xAB
    uint8_t  version;     // must be 1
    uint16_t flags;       // Bit0=FFT present, Bit1=Gonio present
    uint32_t seq;         // monotonically increasing, for drop detection
    float    peak_l;      // dBFS
    float    peak_r;
    float    rms_l;       // dBFS
    float    rms_r;
    float    rms_mono;    // dBFS — true mono sum RMS: RMS((L+R)/2), phase-correct
    float    rms_side;    // dBFS — true side RMS: RMS((L-R)/2), phase-correct
    float    momentary;   // LKFS, 400ms window
    float    short_term;  // LKFS, 3s window
    float    integrated;  // LKFS, cumulative
    float    gonio_l;     // raw sample -1..1 for Lissajous
    float    gonio_r;
    uint32_t fft_bins;    // must equal 256
    float    bins[256];   // magnitude 0.0..1.0, log-scaled, 20Hz..20kHz
} __attribute__((packed));

// Capacity 2: xQueueOverwrite ensures screens always read the latest packet.
// Both Metering and Spectrum screens use xQueuePeek (non-destructive).
extern QueueHandle_t g_audio_queue;
