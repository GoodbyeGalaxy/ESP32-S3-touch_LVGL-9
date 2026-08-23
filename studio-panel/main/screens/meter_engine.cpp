#include "meter_engine.h"
#include "audio_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <cmath>
#include <algorithm>
#include <cstring>

static float to_db(float a)
{
    if (a < 1e-6f) return -60.0f;
    return std::max(20.0f * log10f(a), -60.0f);
}

// FFT bin index boundaries (log-scaled 20Hz–20kHz, 256 bins)
static constexpr int BAND_LO[6] = {  0,  51,  93, 136, 170, 221 };
static constexpr int BAND_HI[6] = { 50,  92, 135, 169, 220, 255 };

MeterEngine::MeterEngine() { reset(); }

void MeterEngine::reset()
{
    memset(&r_, 0, sizeof(r_));
    for (auto &v : r_.short_term_history) v = -40.0f;
    for (auto &v : r_.bands) v = 0.0f;
    vu_pwr_l_ = vu_pwr_r_ = 1e-12f;
    ppm_i_l_  = ppm_i_r_  = -60.0f;
    ppm_ii_l_ = ppm_ii_r_ = -60.0f;
    peak_hold_l_ = peak_hold_r_ = -60.0f;
    peak_hold_timer_ = 0.0f;
    demo_time_ = demo_phase_ = demo_env_ = 0.0f;
    demo_rms_sq_l_ = demo_rms_sq_r_ = 0.0f;
    demo_m_acc_ = demo_s_acc_ = demo_i_acc_ = 0.0f;
    memset(band_smoothed_, 0, sizeof(band_smoothed_));
}

const MeterReadings &MeterEngine::tick(float dt)
{
    AudioPacket pkt;
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) {
        tick_real(pkt, dt);
    } else {
        tick_demo(dt);
    }
    update_ballistics(dt);
    return r_;
}

void MeterEngine::tick_real(const AudioPacket &pkt, float dt)
{
    r_.peak_l = pkt.peak_l;
    r_.peak_r = pkt.peak_r;
    r_.rms_l  = pkt.rms_l;
    r_.rms_r  = pkt.rms_r;
    r_.lufs_m = pkt.momentary;
    r_.lufs_s = pkt.short_term;
    r_.lufs_i = pkt.integrated;
    r_.gonio_l = pkt.gonio_l;
    r_.gonio_r = pkt.gonio_r;

    r_.history_tick += dt;
    if (r_.history_tick >= 1.0f) {
        r_.history_tick -= 1.0f;
        r_.short_term_history[r_.history_head] = pkt.short_term;
        r_.history_head = (r_.history_head + 1) % 60;
    }

    bool fft_present = (pkt.flags & 0x01) && (pkt.fft_bins == 256);
    update_bands(fft_present ? pkt.bins : nullptr, fft_present);
}

void MeterEngine::tick_demo(float dt)
{
    demo_time_  += dt;
    demo_phase_ += 0.25f * dt;
    demo_env_ = 0.5f + 0.35f * sinf(2.0f * M_PI * 0.08f * demo_time_);

    float l = demo_env_ * sinf(2.0f * M_PI * 0.7f * demo_time_);
    float r = demo_env_ * sinf(2.0f * M_PI * 0.7f * demo_time_ + demo_phase_);
    r_.gonio_l = l;
    r_.gonio_r = r;

    r_.peak_l = to_db(fabsf(l));
    r_.peak_r = to_db(fabsf(r));

    float alpha_rms = 1.0f - expf(-dt / 0.30f);
    demo_rms_sq_l_ += alpha_rms * (l*l - demo_rms_sq_l_);
    demo_rms_sq_r_ += alpha_rms * (r*r - demo_rms_sq_r_);
    r_.rms_l = to_db(sqrtf(std::max(demo_rms_sq_l_, 0.0f)));
    r_.rms_r = to_db(sqrtf(std::max(demo_rms_sq_r_, 0.0f)));

    float power = 0.5f * (l*l + r*r);
    float am = 1.0f - expf(-dt / 0.40f);
    float as_ = 1.0f - expf(-dt / 3.00f);
    float ai = 1.0f - expf(-dt / 30.0f);
    demo_m_acc_ += am * (power - demo_m_acc_);
    demo_s_acc_ += as_ * (power - demo_s_acc_);
    demo_i_acc_ += ai * (0.0158f - demo_i_acc_);
    r_.lufs_m = to_db(sqrtf(std::max(demo_m_acc_, 1e-12f)));
    r_.lufs_s = to_db(sqrtf(std::max(demo_s_acc_, 1e-12f)));
    r_.lufs_i = to_db(sqrtf(std::max(demo_i_acc_, 1e-12f)));

    r_.history_tick += dt;
    if (r_.history_tick >= 1.0f) {
        r_.history_tick -= 1.0f;
        r_.short_term_history[r_.history_head] = r_.lufs_s;
        r_.history_head = (r_.history_head + 1) % 60;
    }
    update_bands(nullptr, false);
}

void MeterEngine::update_ballistics(float dt)
{
    // VU: power averaging τ=300ms
    constexpr float VU_ALPHA = 1.0f - 0.8953f; // 1 - exp(-0.033/0.30)
    float p_l = powf(10.0f, r_.peak_l / 10.0f);
    float p_r = powf(10.0f, r_.peak_r / 10.0f);
    vu_pwr_l_ += VU_ALPHA * (p_l - vu_pwr_l_);
    vu_pwr_r_ += VU_ALPHA * (p_r - vu_pwr_r_);
    r_.vu_l = 10.0f * log10f(vu_pwr_l_ < 1e-12f ? 1e-12f : vu_pwr_l_);
    r_.vu_r = 10.0f * log10f(vu_pwr_r_ < 1e-12f ? 1e-12f : vu_pwr_r_);

    // PPM Type I: instant attack, 1.5 dB/s decay
    constexpr float D1 = 1.5f * 0.033f;
    ppm_i_l_ = r_.peak_l > ppm_i_l_ ? r_.peak_l : std::max(ppm_i_l_ - D1, -60.0f);
    ppm_i_r_ = r_.peak_r > ppm_i_r_ ? r_.peak_r : std::max(ppm_i_r_ - D1, -60.0f);
    r_.ppm_i_l = ppm_i_l_;
    r_.ppm_i_r = ppm_i_r_;

    // PPM Type II: instant attack, 4.7 dB/s decay
    constexpr float D2 = 4.7f * 0.033f;
    ppm_ii_l_ = r_.peak_l > ppm_ii_l_ ? r_.peak_l : std::max(ppm_ii_l_ - D2, -60.0f);
    ppm_ii_r_ = r_.peak_r > ppm_ii_r_ ? r_.peak_r : std::max(ppm_ii_r_ - D2, -60.0f);
    r_.ppm_ii_l = ppm_ii_l_;
    r_.ppm_ii_r = ppm_ii_r_;

    // Peak hold: 3s freeze, 30 dB/s decay
    if (r_.peak_l >= peak_hold_l_) { peak_hold_l_ = r_.peak_l; peak_hold_timer_ = 3.0f; }
    else if (peak_hold_timer_ > 0.0f) peak_hold_timer_ -= dt;
    else peak_hold_l_ = std::max(peak_hold_l_ - 30.0f * dt, -60.0f);

    if (r_.peak_r >= peak_hold_r_) peak_hold_r_ = r_.peak_r;
    else if (peak_hold_timer_ <= 0.0f) peak_hold_r_ = std::max(peak_hold_r_ - 30.0f * dt, -60.0f);

    r_.peak_hold_l = peak_hold_l_;
    r_.peak_hold_r = peak_hold_r_;
}

void MeterEngine::update_bands(const float *bins, bool present)
{
    constexpr float ALPHA = 0.20f;
    if (present && bins) {
        for (int b = 0; b < 6; b++) {
            float sum = 0.0f;
            int n = BAND_HI[b] - BAND_LO[b] + 1;
            for (int i = BAND_LO[b]; i <= BAND_HI[b]; i++) sum += bins[i];
            band_smoothed_[b] += ALPHA * (sum / n - band_smoothed_[b]);
        }
    } else {
        for (int b = 0; b < 6; b++)
            band_smoothed_[b] += ALPHA * (0.0f - band_smoothed_[b]);
    }
    for (int b = 0; b < 6; b++) r_.bands[b] = band_smoothed_[b];
}
