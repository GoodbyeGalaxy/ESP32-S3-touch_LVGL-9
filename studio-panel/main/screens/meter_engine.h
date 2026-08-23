#pragma once
#include <cstdint>
#include "audio_data.h"

// All computed metering values for one 30Hz tick.
// Engine fills this; skins read it. No LVGL types here.
struct MeterReadings {
    // Instantaneous (from AudioPacket directly)
    float peak_l,  peak_r;          // dBFS sample peak
    float rms_l,   rms_r;           // dBFS RMS
    float lufs_m,  lufs_s, lufs_i;  // LUFS momentary/short/integrated
    float gonio_l, gonio_r;         // raw sample -1..1 for Lissajous

    // Ballistic outputs — all in dBFS
    float vu_l,     vu_r;           // VU (300ms RMS power avg)
    float ppm_i_l,  ppm_i_r;        // PPM Type I  (EBU, decay 1.5 dB/s)
    float ppm_ii_l, ppm_ii_r;       // PPM Type II (BBC, decay 4.7 dB/s)

    // Peak hold (instantaneous, skin manages its own hold timer if needed)
    float peak_hold_l, peak_hold_r;

    // Spectral balance: 6 bands, smoothed magnitude 0..1
    // [0]=Sub <80Hz  [1]=Bass 80-250  [2]=LowMid 250-800
    // [3]=Mid 800-2k [4]=HighMid 2-8k [5]=Air 8-20k
    float bands[6];

    // Loudness history ring buffer (60 values, 1/s)
    float  short_term_history[60];
    int    history_head;           // next write index
    float  history_tick;           // accumulator toward 1.0s
};

class MeterEngine {
public:
    MeterEngine();

    // Call once per 30Hz tick from LVGL timer callback.
    // Reads g_audio_queue (non-destructive xQueuePeek).
    // Returns reference to internal readings (valid until next tick).
    const MeterReadings &tick(float dt);

    void reset();

private:
    MeterReadings r_;

    // Internal ballistic state
    float vu_pwr_l_,  vu_pwr_r_;
    float ppm_i_l_,   ppm_i_r_;
    float ppm_ii_l_,  ppm_ii_r_;
    float peak_hold_l_, peak_hold_r_;
    float peak_hold_timer_l_, peak_hold_timer_r_;
    float band_smoothed_[6];

    // Demo fallback state (used when no UDP data)
    float demo_time_, demo_phase_, demo_env_;
    float demo_rms_sq_l_, demo_rms_sq_r_;
    float demo_m_acc_, demo_s_acc_, demo_i_acc_;

    void tick_demo(float dt);
    void tick_real(const AudioPacket &pkt, float dt);
    void update_ballistics(float dt);
    void update_bands(const float *bins, bool present);
};
