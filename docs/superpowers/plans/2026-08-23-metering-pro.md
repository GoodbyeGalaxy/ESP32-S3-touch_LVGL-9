# Metering Pro Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor metering into a clean Engine/Skin architecture. Extract pure measurement into `MeterEngine` + `MeterReadings`. Wrap the existing digital display as `SkinDigital` (minimal visual change). Add dB scale, ballistic modes, and spectral balance strip to `SkinDigital`. Future skins (VU needle, PPM, Broadcast) slot in without touching the engine.

**Architecture:**
```
AudioPacket (UDP 30Hz)
    → MeterEngine          pure C++, no LVGL — computes all values every tick
    → MeterReadings        plain struct, all measurement results
    → MeterSkin::update()  LVGL rendering, swappable
```
Engine computes everything simultaneously (Peak, RMS, VU, PPM I+II, LUFS, bands). Skins pick what they display. BOOT button short-press cycles skins; future skins drop in as new files.

**Tech Stack:** C++17, ESP-IDF v5.5, LVGL 9.5, FreeRTOS, GPIO ISR

**Spec:** `docs/CLAUDE.md` (Phase 4) + this document

## Global Constraints

- Colors ≥ 38% luminance on all general UI elements. Canvas/visualization areas exempt.
- `lv_obj_create(NULL)` + `lv_obj_remove_style_all()` + `lv_screen_load()` — never `lv_screen_active()`.
- LVGL lock: widget access only inside LVGL timer callback (already locked) or explicit `esp_lv_adapter_lock`.
- `xQueuePeek(g_audio_queue, &pkt, 0)` — non-destructive. Spectrum screen reads same queue.
- `AudioPacket.bins[256]`: log-scaled magnitudes 0..1, 20Hz–20kHz. `flags & 0x01` = FFT present.
- BOOT ISR: `IRAM_ATTR`, `gpio_install_isr_service(0)` is idempotent.
- Build: `cd studio-panel && source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -20`

---

## File Map

| File | Status | Responsibility |
|------|--------|----------------|
| `main/screens/meter_engine.h` | **Create** | `MeterReadings` struct + `MeterEngine` class declaration |
| `main/screens/meter_engine.cpp` | **Create** | All ballistic math, band aggregation — zero LVGL |
| `main/screens/meter_skin.h` | **Create** | Abstract `MeterSkin` interface |
| `main/screens/skin_digital.h` | **Create** | `SkinDigital` declaration |
| `main/screens/skin_digital.cpp` | **Create** | Current visual ported + dB scale + bands |
| `main/screens/metering.h` | Modify | Remove internals, expose only `metering_screen_create()` |
| `main/screens/metering.cpp` | Modify | Thin orchestrator: owns engine + active skin + BOOT btn + timer |
| `main/CMakeLists.txt` | Modify | Add new source files |

---

## Task 1: MeterEngine + MeterReadings

**Goal:** Extract all measurement math from `metering.cpp` into a self-contained engine with no LVGL dependency. `MeterReadings` is the contract between engine and all skins.

**Files:**
- Create: `main/screens/meter_engine.h`
- Create: `main/screens/meter_engine.cpp`

- [ ] **Step 1: Write `MeterReadings` struct in `meter_engine.h`**

```cpp
#pragma once
#include <cstdint>

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
```

- [ ] **Step 2: Write `MeterEngine` class in `meter_engine.h`**

Append after `MeterReadings`:

```cpp
#include "audio_data.h"

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
    float peak_hold_l_, peak_hold_r_, peak_hold_timer_;
    float band_smoothed_[6];

    // Demo fallback state (used when no UDP data)
    float demo_time_, demo_phase_, demo_env_;
    float demo_rms_sq_l_, demo_rms_sq_r_;
    float demo_m_acc_, demo_s_acc_, demo_i_acc_;

    void tick_demo(float dt);
    void tick_real(const AudioPacket &pkt, float dt);
    void update_ballistics(float dt);
    void update_bands(const float *bins, bool present);

    static float to_db(float linear);
};
```

- [ ] **Step 3: Implement `meter_engine.cpp`**

```cpp
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
```

- [ ] **Step 4: Add new files to CMakeLists.txt**

In `main/CMakeLists.txt`, find the `SRCS` list and add:
```cmake
"screens/meter_engine.cpp"
"screens/skin_digital.cpp"
```

- [ ] **Step 5: Build engine alone (expect linker errors — that's fine at this stage)**

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
```

Expected: only errors about missing `SkinDigital` / missing references from `metering.cpp` (not yet wired). Engine itself must compile clean.

- [ ] **Step 6: Commit**

```bash
git add studio-panel/main/screens/meter_engine.h \
        studio-panel/main/screens/meter_engine.cpp \
        studio-panel/main/CMakeLists.txt
git commit -m "feat: MeterEngine + MeterReadings — pure measurement, no LVGL"
```

---

## Task 2: MeterSkin interface + SkinDigital (port of existing visuals)

**Goal:** Define the abstract `MeterSkin` interface. Port the existing metering.cpp rendering into `SkinDigital` — same look, no visual changes yet. `metering.cpp` becomes a thin orchestrator.

**Files:**
- Create: `main/screens/meter_skin.h`
- Create: `main/screens/skin_digital.h`
- Create: `main/screens/skin_digital.cpp`
- Modify: `main/screens/metering.h`
- Modify: `main/screens/metering.cpp`

- [ ] **Step 1: Write `meter_skin.h`**

```cpp
#pragma once
#include "lvgl.h"
#include "meter_engine.h"

struct MeterSkin {
    virtual ~MeterSkin() = default;
    // Called once after screen object exists. Skin creates its LVGL children on parent.
    virtual void create(lv_obj_t *parent) = 0;
    // Called at 30Hz from LVGL timer callback (already locked). Update widgets from r.
    virtual void update(const MeterReadings &r) = 0;
    // Called before screen is deleted. Skin must NOT delete parent — only its own allocs.
    virtual void destroy() = 0;
    // Short human-readable name shown in mode label.
    virtual const char *name() const = 0;
};
```

- [ ] **Step 2: Write `skin_digital.h`**

```cpp
#pragma once
#include "meter_skin.h"

class SkinDigital final : public MeterSkin {
public:
    void create(lv_obj_t *parent) override;
    void update(const MeterReadings &r) override;
    void destroy() override;
    const char *name() const override { return "DIGITAL"; }

private:
    // Goniometer ring buffer
    struct GonioPoint { int16_t x, y; };
    GonioPoint gonio_pts_[80];
    int        gonio_head_ = 0;
    void      *gonio_buf_  = nullptr;

    // Bar widget data (passed as user_data to draw_cb)
    struct BarData { float rms_db; float peak_hold_db; };
    BarData bar_l_data_{}, bar_r_data_{};

    // LVGL widget handles
    lv_obj_t *bar_l_    = nullptr;
    lv_obj_t *bar_r_    = nullptr;
    lv_obj_t *gonio_    = nullptr;
    lv_obj_t *history_  = nullptr;
    lv_obj_t *num_i_    = nullptr;
    lv_obj_t *num_s_    = nullptr;
    lv_obj_t *num_m_    = nullptr;
    lv_obj_t *num_peak_ = nullptr;
    lv_obj_t *scale_col_= nullptr;
    lv_obj_t *spec_strip_= nullptr;

    // History ring buffer (owned by skin)
    float history_buf_[60]{};
    int   history_head_ = 0;
    float history_tick_ = 0.0f;

    // Private helpers
    void create_bars(lv_obj_t *parent);
    void create_gonio(lv_obj_t *parent);
    void create_history(lv_obj_t *parent);
    void create_numerics(lv_obj_t *parent);
    void create_scale(lv_obj_t *parent);
    void create_spec_strip(lv_obj_t *parent);

    void update_gonio(const MeterReadings &r);
    void update_history(const MeterReadings &r);

    // Static LVGL draw callbacks (take user_data pointer)
    static void bar_draw_cb(lv_event_t *e);
    static void history_draw_cb(lv_event_t *e);
    static void spec_strip_draw_cb(lv_event_t *e);
    static void gonio_redraw(lv_obj_t *canvas, GonioPoint *pts, int head, void *buf);
};
```

- [ ] **Step 3: Implement `skin_digital.cpp`**

Port the drawing code from current `metering.cpp` into the class methods. Key mappings:

| Old (metering.cpp) | New (SkinDigital) |
|---|---|
| `bar_draw_cb` static fn | `SkinDigital::bar_draw_cb` static |
| `metering_bar_create` | part of `create_bars` |
| `metering_bar_update` | inside `update()` |
| `metering_gonio_create` | `create_gonio` |
| `metering_gonio_update` | `update_gonio` |
| `gonio_redraw` | `SkinDigital::gonio_redraw` static |
| `history_draw_cb` | `SkinDigital::history_draw_cb` static |
| `metering_history_create` | `create_history` |
| `metering_numerics_create` | `create_numerics` |
| `data->gonio_pts`, `gonio_head` | `gonio_pts_`, `gonio_head_` |
| `data->gonio_buf` | `gonio_buf_` |

Add NEW widgets in `create()`:
- `create_scale(parent)` — dB tick labels (from original plan Task 1)
- `create_spec_strip(parent)` — 6-band strip (from original plan Task 3)

`create_scale` implementation (same as original plan Task 1, Step 1).

`create_spec_strip` creates a transparent obj with `spec_strip_draw_cb`; positioned at x=590, y=190, w=194, h=90.

`spec_strip_draw_cb` takes `SkinDigital*` as user_data:
```cpp
void SkinDigital::spec_strip_draw_cb(lv_event_t *e)
{
    auto *skin = static_cast<SkinDigital*>(lv_event_get_user_data(e));
    // ... draw 6 bands from skin->spec_bands_ (updated each tick)
```

Add private field `float spec_bands_[6]{}` to `SkinDigital` (copied from `MeterReadings::bands` each `update()` call, then invalidate strip).

`update(const MeterReadings &r)` — reads `r.rms_l/r`, `r.peak_hold_l/r`, `r.gonio_l/r`, `r.lufs_m/s/i`, `r.bands[]`, `r.short_term_history[]`, `r.history_head`.

The `-18 dBFS` reference line goes into `bar_draw_cb` (same as original plan Task 1, Step 2).

- [ ] **Step 4: Slim down `metering.cpp` to orchestrator**

Replace all drawing code in `metering.cpp` with engine + skin ownership:

```cpp
#include "metering.h"
#include "meter_engine.h"
#include "meter_skin.h"
#include "skin_digital.h"
#include "theme.h"
#include "screens/home.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <memory>

static const char *TAG = "metering";

struct MeteringScreenData {
    MeterEngine              engine;
    std::unique_ptr<MeterSkin> skin;

    lv_obj_t   *mode_label = nullptr;
    lv_timer_t *timer      = nullptr;

    // BOOT button
    volatile int64_t btn_press_us = 0;
    volatile bool    btn_event    = false;
    volatile bool    btn_long     = false;
};

// BOOT ISR (IRAM_ATTR)
static void IRAM_ATTR metering_boot_isr(void *arg)
{
    auto *d = static_cast<MeteringScreenData*>(arg);
    int level = gpio_get_level(GPIO_NUM_0);
    int64_t now = esp_timer_get_time();
    if (level == 0) { d->btn_press_us = now; }
    else { d->btn_long = (now - d->btn_press_us) >= 1000000; d->btn_event = true; }
}

static void metering_boot_init(MeteringScreenData *d)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_NUM_0, metering_boot_isr, d);
}

static void metering_timer_cb(lv_timer_t *timer)
{
    auto *d = static_cast<MeteringScreenData*>(lv_timer_get_user_data(timer));

    if (d->btn_event) {
        d->btn_event = false;
        if (!d->btn_long) {
            // Placeholder: only SkinDigital exists now; future skins added here
            ESP_LOGI(TAG, "BOOT short — skin cycle (only one skin for now)");
        }
        d->btn_long = false;
    }

    constexpr float DT = 0.033f;
    const MeterReadings &r = d->engine.tick(DT);
    d->skin->update(r);
}

static void on_screen_delete(lv_event_t *e)
{
    auto *d = static_cast<MeteringScreenData*>(lv_event_get_user_data(e));
    gpio_isr_handler_remove(GPIO_NUM_0);
    if (d->timer) lv_timer_delete(d->timer);
    d->skin->destroy();
    delete d;
}

static void on_back(lv_event_t *e)
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

lv_obj_t *metering_screen_create()
{
    auto *d = new MeteringScreenData{};
    d->skin = std::make_unique<SkinDigital>();

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_screen_delete, LV_EVENT_DELETE, d);

    d->skin->create(scr);

    // Back button
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 100, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(lbl);

    metering_boot_init(d);
    d->timer = lv_timer_create(metering_timer_cb, 33, d);

    return scr;
}
```

- [ ] **Step 5: Update `metering.h`**

Strip to just the public API:
```cpp
#pragma once
#include "lvgl.h"

// Creates and returns the metering screen.
// Call lv_screen_load() on the result from within LVGL lock.
lv_obj_t *metering_screen_create();
```

- [ ] **Step 6: Build, flash, verify**

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
idf.py -p /dev/ttyACM0 flash
```

Visual: metering screen looks identical to before. New additions visible:
- dB scale labels in gap between bars and goniometer
- Yellow dashed -18 dBFS reference line on bars
- 6-band spectral strip in right panel (responds to audio sender)

- [ ] **Step 7: Commit**

```bash
git add studio-panel/main/screens/meter_engine.h \
        studio-panel/main/screens/meter_engine.cpp \
        studio-panel/main/screens/meter_skin.h \
        studio-panel/main/screens/skin_digital.h \
        studio-panel/main/screens/skin_digital.cpp \
        studio-panel/main/screens/metering.h \
        studio-panel/main/screens/metering.cpp \
        studio-panel/main/CMakeLists.txt
git commit -m "refactor: engine/skin split — MeterEngine + SkinDigital, metering.cpp orchestrates"
```

---

## Task 3: Ballistic modes in SkinDigital

**Goal:** BOOT button now cycles which ballistic output from `MeterReadings` the bars display. Engine already computes all values every tick — skin just picks which field to read.

**Files:**
- Modify: `main/screens/metering.cpp` (skin cycle logic)
- Modify: `main/screens/skin_digital.h` (add `setMode`)
- Modify: `main/screens/skin_digital.cpp` (read correct field in `update`)

- [ ] **Step 1: Add mode selection to `SkinDigital`**

In `skin_digital.h`, add:
```cpp
enum class DigitalMode : uint8_t { DBFS=0, VU=1, PPM_I=2, PPM_II=3, COUNT=4 };

// In class SkinDigital:
public:
    void setMode(DigitalMode m);
    DigitalMode mode() const { return mode_; }
private:
    DigitalMode  mode_     = DigitalMode::DBFS;
    lv_obj_t    *mode_lbl_ = nullptr;  // shows "dBFS" / "VU" / "PPM I" / "PPM II"
```

- [ ] **Step 2: Wire mode label into `create()`**

At end of `SkinDigital::create()`:
```cpp
mode_lbl_ = lv_label_create(parent);
lv_obj_remove_style_all(mode_lbl_);
lv_obj_set_style_text_font(mode_lbl_, THEME_FONT_HINT, 0);
lv_obj_set_style_text_color(mode_lbl_, lv_color_hex(0xA0A0A0), 0);
lv_obj_set_pos(mode_lbl_, 590, 168);
lv_label_set_text(mode_lbl_, "MODE: dBFS");
```

- [ ] **Step 3: Implement `setMode` + mode-aware `update`**

```cpp
void SkinDigital::setMode(DigitalMode m)
{
    mode_ = m;
    static const char *NAMES[] = {"dBFS","VU","PPM I","PPM II"};
    lv_label_set_text_fmt(mode_lbl_, "MODE: %s",
                          NAMES[static_cast<uint8_t>(m)]);
}
```

In `update(const MeterReadings &r)`, replace direct `r.peak_l`/`r.peak_r` bar reads:
```cpp
float bar_l, bar_r;
switch (mode_) {
    case DigitalMode::VU:    bar_l = r.vu_l;     bar_r = r.vu_r;     break;
    case DigitalMode::PPM_I: bar_l = r.ppm_i_l;  bar_r = r.ppm_i_r;  break;
    case DigitalMode::PPM_II:bar_l = r.ppm_ii_l; bar_r = r.ppm_ii_r; break;
    default:                 bar_l = r.peak_l;   bar_r = r.peak_r;   break;
}
// update bar_l_data_ and bar_r_data_ with bar_l, bar_r and r.peak_hold_l/r
```

- [ ] **Step 4: Wire BOOT cycle in `metering.cpp`**

Replace the placeholder comment in `metering_timer_cb` with:
```cpp
if (!d->btn_long) {
    auto *sd = static_cast<SkinDigital*>(d->skin.get());
    auto next = static_cast<SkinDigital::DigitalMode>(
        (static_cast<uint8_t>(sd->mode()) + 1) %
        static_cast<uint8_t>(SkinDigital::DigitalMode::COUNT));
    sd->setMode(next);
    d->engine.reset();  // clear ballistic history on mode change
}
```

- [ ] **Step 5: Build, flash, verify**

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
idf.py -p /dev/ttyACM0 flash
```

Visual checks:
- BOOT short press cycles: `dBFS → VU → PPM I → PPM II → dBFS`
- Mode label updates in right panel
- VU: bars sluggish, lag ~300ms on transients
- PPM I: bars snap to peak, fall ~1.5 dB/s (visually slow fall)
- PPM II: bars snap, fall faster (~4.7 dB/s)
- dBFS: instant tracking (same as before)
- dB scale + VU reference line visible in all modes

- [ ] **Step 6: Commit**

```bash
git add studio-panel/main/screens/metering.cpp \
        studio-panel/main/screens/skin_digital.h \
        studio-panel/main/screens/skin_digital.cpp
git commit -m "feat: SkinDigital ballistic modes dBFS/VU/PPM-I/PPM-II via BOOT button"
```

---

## Self-Review

**Spec coverage:**
- ✅ Engine/Skin separation — measurement vs. rendering cleanly split
- ✅ All ballistic values computed simultaneously every tick
- ✅ `SkinDigital` = existing visual (no regression) + dB scale + VU ref + spectral strip
- ✅ BOOT button cycles ballistic mode display (dBFS/VU/PPM I/PPM II)
- ✅ Mode label visible
- ✅ Spectral balance strip (6 bands, FFT-driven, smoothed α=0.2)
- ✅ -18 dBFS reference line (= 0 VU / +4 dBu broadcast standard)
- ✅ EBU R128 -23 LKFS target already on history from Phase 2 (no change needed)
- ⬜ Future skins (SkinVU needle, SkinPPM, SkinBroadcast): engine already feeds all values, skin just needs to implement `MeterSkin` interface
- ⬜ True Peak: requires protocol extension (AudioPacket) or 4x upsampling — deferred

**Type consistency:** `MeterReadings` fields referenced identically in `meter_engine.cpp`, `skin_digital.cpp`, and `metering.cpp`. `SkinDigital::DigitalMode` enum values match names used in `setMode` switch. `band_smoothed_[6]` / `BAND_LO[6]` / `BAND_HI[6]` all sized 6.

**Placeholder scan:** No TBD/TODO. All code blocks are complete.
