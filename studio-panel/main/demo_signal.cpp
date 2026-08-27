#include "demo_signal.h"
#include "audio_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cmath>
#include <cstring>

static const char *TAG = "demo_sig";

// ── State ─────────────────────────────────────────────────────────────────────

static std::atomic<bool>     s_forced{false};
static std::atomic<bool>     s_running{false};
static std::atomic<uint32_t> s_last_real_ms{0};  // ms timestamp of last real packet

// Real signal present = last real packet within 2000ms.
static constexpr uint32_t REAL_TIMEOUT_MS = 2000;

// ── Signal helpers ────────────────────────────────────────────────────────────

// Gaussian peak centred at bin c with width w, amplitude a.
// IN: bin index i 0..255, centre c, width w, amplitude a. OUT: contribution 0..a.
static float gauss(int i, float c, float w, float a)
{
    float x = ((float)i - c) / w;
    return a * expf(-0.5f * x * x);
}

// ── Demo task ─────────────────────────────────────────────────────────────────

// Lissajous ratio table: (freq_l, freq_r) pairs — cycles every 8s per step.
static constexpr int RATIO_COUNT = 5;
static const float RATIO_L[RATIO_COUNT] = { 1.0f, 2.0f, 3.0f, 3.0f, 1.0f };
static const float RATIO_R[RATIO_COUNT] = { 1.0f, 3.0f, 4.0f, 5.0f, 2.0f };

static void demo_task(void *)
{
    uint32_t tick = 0;           // counts at 30 Hz
    uint32_t seq  = 1;

    // Lissajous phase accumulators
    float phase_l = 0.0f;
    float phase_r = 0.0f;

    // Wandering peak 1 (bass sweep)
    float peak1_center = 20.0f;  // bin index 0..255, sweeps bass region
    float peak1_dir    = 1.0f;

    // Kick transient state
    float transient_amp = 0.0f;
    int   next_kick     = 90;    // ticks until next simulated kick (~3s at 30Hz)

    // Peak/RMS breath (sin, period 3.7s = 111 ticks)
    float breath_phase = 0.0f;

    // Momentary LUFS (lpf over RMS)
    float momentary_lufs = -18.0f;

    static constexpr float TWO_PI = 6.28318530f;
    static constexpr float DT_S   = 1.0f / 30.0f;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(33));  // ~30 Hz

        // Decide if demo is active
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        bool forced = s_forced.load(std::memory_order_relaxed);
        bool silent = (now_ms - s_last_real_ms.load(std::memory_order_relaxed)) > REAL_TIMEOUT_MS;

        if (!forced && !silent) {
            // Real signal present and not forced — skip
            tick++;
            continue;
        }

        // ── Lissajous ratio selection ─────────────────────────────────────────
        // Each ratio lasts 8s = 240 ticks. Cycle through RATIO_COUNT.
        int ratio_idx = (tick / 240) % RATIO_COUNT;
        float fl = RATIO_L[ratio_idx] * 220.0f * TWO_PI * DT_S;  // ~220 Hz base
        float fr = RATIO_R[ratio_idx] * 220.0f * TWO_PI * DT_S;

        // Smooth ratio transition: gentle phase continuation
        phase_l = fmodf(phase_l + fl, TWO_PI);
        phase_r = fmodf(phase_r + fr + 0.17f, TWO_PI);  // small static phase offset

        float gonio_l = sinf(phase_l);
        float gonio_r = sinf(phase_r);

        // ── FFT bins ──────────────────────────────────────────────────────────

        // Peak 1: sweeps 20..200 (bass) over ~12s = 360 ticks
        peak1_center += peak1_dir * (180.0f / 360.0f);
        if (peak1_center >= 200.0f) { peak1_center = 200.0f; peak1_dir = -1.0f; }
        if (peak1_center <= 20.0f)  { peak1_center = 20.0f;  peak1_dir =  1.0f; }

        // Peak 2: fixed 1kHz (~bin 128), amplitude breathes (4s = 120 ticks)
        float p2_amp = 0.5f + 0.4f * sinf((float)tick * TWO_PI / 120.0f);

        // Peak 3 (kick transient): random burst every ~3s
        if (--next_kick <= 0) {
            transient_amp = 1.0f;
            next_kick = 75 + (int)(sinf((float)tick * 0.13f) * 15.0f + 15.0f); // 75..105
        }
        transient_amp *= 0.82f;  // decay

        // Build bins
        float bins[256];
        float kick_center = 20.0f + 10.0f * fabsf(sinf((float)tick * 0.41f)); // 20..30
        for (int i = 0; i < 256; i++) {
            float v = gauss(i, peak1_center, 18.0f, 0.6f)
                    + gauss(i, 128.0f,       22.0f, p2_amp)
                    + gauss(i, kick_center,  8.0f,  transient_amp);
            // Add a small noise floor
            v += 0.04f * fabsf(sinf((float)(i * 7 + tick * 3)));
            if (v > 1.0f) v = 1.0f;
            if (v < 0.0f) v = 0.0f;
            bins[i] = v;
        }

        // ── Peak / RMS breath ──────────────────────────────────────────────────
        breath_phase = fmodf(breath_phase + TWO_PI * DT_S / 3.7f, TWO_PI);
        float breath = 0.5f + 0.5f * sinf(breath_phase);  // 0..1
        float rms_dbfs  = -24.0f + breath * 18.0f;          // -24..-6 dBFS
        float peak_dbfs = rms_dbfs + 3.0f + transient_amp * 8.0f;
        if (peak_dbfs > 0.0f) peak_dbfs = 0.0f;

        // Momentary LUFS: lpf with alpha=0.1 per tick
        float target_lufs = rms_dbfs - 3.0f;
        momentary_lufs += 0.1f * (target_lufs - momentary_lufs);

        // ── Assemble packet ───────────────────────────────────────────────────
        AudioPacket pkt{};
        pkt.magic    = 0xAB;
        pkt.version  = 1;
        pkt.flags    = 0x03;  // FFT + Gonio present
        pkt.seq      = seq++;
        pkt.peak_l   = peak_dbfs + 0.5f * gonio_l;
        pkt.peak_r   = peak_dbfs - 0.5f * gonio_r;
        pkt.rms_l    = rms_dbfs;
        pkt.rms_r    = rms_dbfs - 0.3f;
        pkt.momentary   = momentary_lufs;
        pkt.short_term  = momentary_lufs - 1.0f;
        pkt.integrated  = momentary_lufs - 2.0f;
        pkt.gonio_l  = gonio_l;
        pkt.gonio_r  = gonio_r;
        pkt.fft_bins = 256;
        memcpy(pkt.bins, bins, sizeof(bins));

        xQueueOverwrite(g_audio_queue, &pkt);
        tick++;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void demo_signal_init()
{
    if (s_running.exchange(true)) return;  // idempotent

    // Seed last-real timestamp far in the past so auto-mode activates immediately.
    s_last_real_ms.store(0, std::memory_order_relaxed);

    xTaskCreatePinnedToCore(demo_task, "demo_sig", 4096, nullptr, 3, nullptr, 0);
    ESP_LOGI(TAG, "demo signal task started");
}

void demo_signal_set_forced(bool forced)
{
    s_forced.store(forced, std::memory_order_relaxed);
    ESP_LOGI(TAG, "demo forced=%d", (int)forced);
}

void demo_signal_notify_packet()
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    s_last_real_ms.store(now_ms, std::memory_order_relaxed);
}

bool demo_signal_is_active()
{
    if (s_forced.load(std::memory_order_relaxed)) return true;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    return (now_ms - s_last_real_ms.load(std::memory_order_relaxed)) > REAL_TIMEOUT_MS;
}
