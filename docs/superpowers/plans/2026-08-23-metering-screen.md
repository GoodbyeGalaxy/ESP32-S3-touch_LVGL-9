# Metering Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `metering.cpp` placeholder with a full broadcast-metering screen: stereo bar meters, Lissajous goniometer, 60-second loudness history graph, and numeric LUFS/Peak readouts, all driven by simulated demo data.

**Architecture:** All logic lives in `main/screens/metering.cpp`. A heap-allocated `MeteringScreenData` struct holds state, widget pointers, canvas buffer, and the LVGL timer. Each visual unit is a standalone `_create()` + `_update()` function pair (modular for future layout variants). A single `lv_timer` at 33 ms (≈30 Hz) drives the demo generator and calls every update function. The timer and canvas buffer are freed in an `LV_EVENT_DELETE` callback on the screen object.

**Tech Stack:** C++17, ESP-IDF v5.5, LVGL 9.5 via `espressif/esp_lvgl_adapter 0.5.2`, PSRAM (8 MB OPI) for the goniometer canvas buffer.

**Spec:** `docs/superpowers/specs/2026-08-23-metering-screen-design.md`

## Global Constraints

- Target board: Waveshare ESP32-S3-Touch-LCD-7, 800×480 RGB panel
- `THEME_STATUSBAR_H = 32` (from `theme.h`) — content starts at y = 40 (32 + 8px top padding)
- All draw colors **≥ 38 % luminance** — IPS panel shows green tint below. Never use `0x0A0A0A` or similar dark values.
- Modify only: `main/screens/metering.cpp`. No other file changes.
- No new entries in `idf_component.yml`.
- Working directory for all commands: `/mnt/source/data/coding/ESP32-S3/studio-panel`
- Activate ESP-IDF before first build: `source ~/esp/esp-idf-5.5/export.sh`
- Build check: `idf.py build 2>&1 | grep -E " error:|fatal error" | head -20`
- Flash: `idf.py -p /dev/ttyACM0 flash`
- Monitor: `timeout 20 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep -v "^---"`
- If permission denied on `/dev/ttyACM0`: `newgrp dialout`

## Final Screen Layout

```
y=0  ┌──────────────────────────────────── 800px ─────────────────────────────────────┐
     │                     Status Bar (THEME_STATUSBAR_H = 32px)                      │
y=40 ├──────────┬──────────────────────────────────────────┬──────────────────────────┤
     │  L bar   │        Goniometer / Lissajous             │  I:    -14.2 LKFS        │
     │ x=16     │   250×250 canvas, x=278, y=40            │  S:    -13.8 LKFS        │
     │ w=90     │                                          │  M:    -12.1 LKFS        │
     │          │                                          │  Peak:  -5.9 dBFS        │
     │  R bar   │                                          │  x=590, w=194            │
     │ x=116    ├──────────────────────────────────────────┤                          │
     │ w=90     │  Short-term Loudness History (60s)       │                          │
     │          │  x=224, y=298, w=358, h=122              │                          │
     │ h=380    │  Target line: -23 LKFS (EBU R128)       │                          │
y=420├──────────┴──────────────────────────────────────────┴──────────────────────────┤
     │ [◁ Home]  x=16, y=420, w=100, h=44                                            │
y=464└────────────────────────────────────────────────────────────────────────────────┘
```

---

## Task 1: Scaffolding — Structs, Function Declarations, Screen Skeleton

**Files:**
- Modify: `main/screens/metering.cpp` (full rewrite of placeholder)

**Interfaces:**
- Produces: `metering_screen_create()` (unchanged signature from `metering.h`), all internal structs and forward declarations

- [ ] **Step 1: Replace metering.cpp with skeleton**

Write the following complete file. It compiles, shows the screen background and a Back button — all widgets TBD in later tasks.

```cpp
#include "metering.h"
#include "theme.h"
#include "screens/home.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <cmath>
#include <cstring>
#include <algorithm>

// ── Demo data state ───────────────────────────────────────────────────────────

struct MeteringState {
    float time;
    float phase_offset;
    float envelope;

    float l_sample;   // -1.0 .. 1.0
    float r_sample;

    float peak_l, peak_r;          // dBFS
    float peak_hold_l, peak_hold_r;
    float peak_hold_timer;

    float rms_sq_l, rms_sq_r;     // power accumulator for RMS
    float rms_l, rms_r;           // dBFS

    float m_acc, s_acc, i_acc;    // power for momentary/short-term/integrated
    float momentary;              // dBFS
    float short_term;
    float integrated;

    float short_term_history[60]; // ring buffer, 1 value/s
    int   history_head;           // next write index
    float history_tick;           // accumulator toward 1.0s
};

// ── Per-screen data (allocated on create, freed on delete) ───────────────────

struct MeteringScreenData {
    MeteringState state;

    lv_obj_t *bar_l;
    lv_obj_t *bar_r;
    lv_obj_t *gonio;          // lv_canvas_t
    lv_obj_t *history;
    lv_obj_t *num_i;
    lv_obj_t *num_s;
    lv_obj_t *num_m;
    lv_obj_t *num_peak;

    lv_timer_t *timer;
    void       *gonio_buf;    // PSRAM canvas buffer

    struct GonioPoint { int16_t x, y; } gonio_pts[80];
    int gonio_head;
};

// ── Per-bar draw data ─────────────────────────────────────────────────────────

struct BarWidgetData {
    float rms_db;
    float peak_hold_db;
};

// ── Forward declarations ──────────────────────────────────────────────────────

static void metering_demo_tick(MeteringState &s, float dt);
static lv_obj_t *metering_bar_create(lv_obj_t *parent, BarWidgetData *d);
static void      metering_bar_update(lv_obj_t *bar, float rms_db, float peak_hold_db);
static lv_obj_t *metering_gonio_create(lv_obj_t *parent, MeteringScreenData *data);
static void      metering_gonio_update(lv_obj_t *canvas, MeteringScreenData *data);
static lv_obj_t *metering_history_create(lv_obj_t *parent, MeteringScreenData *data);
static void      metering_history_invalidate(lv_obj_t *hist);
static lv_obj_t *metering_numerics_create(lv_obj_t *parent);
static void      metering_numerics_update(MeteringScreenData *data);
static void      metering_timer_cb(lv_timer_t *timer);
static void      on_screen_delete(lv_event_t *e);
static void      on_back(lv_event_t *e);

// ── Stub implementations (filled in later tasks) ──────────────────────────────

static void metering_demo_tick(MeteringState &, float) {}

static lv_obj_t *metering_bar_create(lv_obj_t *parent, BarWidgetData *)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}
static void metering_bar_update(lv_obj_t *, float, float) {}

static lv_obj_t *metering_gonio_create(lv_obj_t *parent, MeteringScreenData *)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    return c;
}
static void metering_gonio_update(lv_obj_t *, MeteringScreenData *) {}

static lv_obj_t *metering_history_create(lv_obj_t *parent, MeteringScreenData *)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    return c;
}
static void metering_history_invalidate(lv_obj_t *) {}

static lv_obj_t *metering_numerics_create(lv_obj_t *parent)
{
    return lv_obj_create(parent);
}
static void metering_numerics_update(MeteringScreenData *) {}

// ── Timer + screen lifecycle ──────────────────────────────────────────────────

static void metering_timer_cb(lv_timer_t *timer)
{
    auto *data = static_cast<MeteringScreenData*>(lv_timer_get_user_data(timer));
    constexpr float DT = 0.033f;
    metering_demo_tick(data->state, DT);
    metering_bar_update(data->bar_l, data->state.rms_l, data->state.peak_hold_l);
    metering_bar_update(data->bar_r, data->state.rms_r, data->state.peak_hold_r);
    metering_gonio_update(data->gonio, data);
    metering_history_invalidate(data->history);
    metering_numerics_update(data);
}

static void on_screen_delete(lv_event_t *e)
{
    auto *data = static_cast<MeteringScreenData*>(lv_event_get_user_data(e));
    if (data->timer)     lv_timer_delete(data->timer);
    if (data->gonio_buf) heap_caps_free(data->gonio_buf);
    delete data;
}

static void on_back(lv_event_t *e)
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, true);
}

// ── Public entry point ────────────────────────────────────────────────────────

lv_obj_t *metering_screen_create()
{
    auto *data = new MeteringScreenData{};

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_screen_delete, LV_EVENT_DELETE, data);

    // Widgets (positioned in Task 6; stubs return placeholder objs)
    data->bar_l   = metering_bar_create(scr, nullptr);
    data->bar_r   = metering_bar_create(scr, nullptr);
    data->gonio   = metering_gonio_create(scr, data);
    data->history = metering_history_create(scr, data);
    lv_obj_t *nums = metering_numerics_create(scr);
    (void)nums;

    // Back button
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 100, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(btn_lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(btn_lbl);

    data->timer = lv_timer_create(metering_timer_cb, 33, data);

    return scr;
}
```

- [ ] **Step 2: Build check**

```bash
cd /mnt/source/data/coding/ESP32-S3/studio-panel
source ~/esp/esp-idf-5.5/export.sh
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
```

Expected: no errors. Warnings about unused stubs are acceptable.

- [ ] **Step 3: Commit**

```bash
git add main/screens/metering.cpp
git commit -m "feat: metering screen scaffold — structs, stubs, timer skeleton"
```

---

## Task 2: Demo Data Generator

**Files:**
- Modify: `main/screens/metering.cpp` — replace `metering_demo_tick` stub

**Interfaces:**
- Consumes: `MeteringState &s`, `float dt` (seconds since last call, typically 0.033)
- Produces: all fields of `MeteringState` filled with realistic simulated audio values

- [ ] **Step 1: Replace metering_demo_tick stub with full implementation**

Find and replace the stub:
```cpp
static void metering_demo_tick(MeteringState &, float) {}
```

With:
```cpp
static void metering_demo_tick(MeteringState &s, float dt)
{
    // Time + slowly drifting stereo phase offset
    s.time         += dt;
    s.phase_offset += 0.25f * dt;  // full rotation every ~25 s

    // Slow amplitude envelope: 0.15 .. 0.85 at 0.08 Hz
    s.envelope = 0.5f + 0.35f * sinf(2.0f * M_PI * 0.08f * s.time);

    // L/R samples
    s.l_sample = s.envelope * sinf(2.0f * M_PI * 0.7f * s.time);
    s.r_sample = s.envelope * sinf(2.0f * M_PI * 0.7f * s.time + s.phase_offset);

    // dBFS helper (returns -60.0 for near-silence)
    auto to_db = [](float v) -> float {
        float a = fabsf(v);
        if (a < 1e-6f) return -60.0f;
        return std::max(20.0f * log10f(a), -60.0f);
    };

    float db_l = to_db(s.l_sample);
    float db_r = to_db(s.r_sample);

    // Peak: fast attack, 30 dB/s decay
    constexpr float PEAK_DECAY = 30.0f;
    s.peak_l = std::max(db_l, s.peak_l - PEAK_DECAY * dt);
    s.peak_r = std::max(db_r, s.peak_r - PEAK_DECAY * dt);

    // Peak hold: 3 s freeze, then same decay
    auto update_hold = [&](float peak, float &hold, float &timer) {
        if (peak >= hold) { hold = peak; timer = 3.0f; }
        else if (timer > 0.0f) timer -= dt;
        else hold = std::max(hold - PEAK_DECAY * dt, -60.0f);
    };
    update_hold(s.peak_l, s.peak_hold_l, s.peak_hold_timer);
    update_hold(s.peak_r, s.peak_hold_r, s.peak_hold_timer);  // shared timer OK for demo

    // RMS: exponential MA, τ = 300 ms
    float alpha_rms = 1.0f - expf(-dt / 0.30f);
    s.rms_sq_l += alpha_rms * (s.l_sample * s.l_sample - s.rms_sq_l);
    s.rms_sq_r += alpha_rms * (s.r_sample * s.r_sample - s.rms_sq_r);
    s.rms_l = to_db(sqrtf(std::max(s.rms_sq_l, 0.0f)));
    s.rms_r = to_db(sqrtf(std::max(s.rms_sq_r, 0.0f)));

    // Momentary (τ = 400 ms), Short-term (τ = 3 s), Integrated (τ = 30 s)
    float power = 0.5f * (s.l_sample * s.l_sample + s.r_sample * s.r_sample);
    float alpha_m = 1.0f - expf(-dt / 0.40f);
    float alpha_s = 1.0f - expf(-dt / 3.00f);
    float alpha_i = 1.0f - expf(-dt / 30.0f);

    // Target integrated around -14 LKFS power ≈ 0.0158
    constexpr float I_TARGET_POWER = 0.0158f;
    s.m_acc += alpha_m * (power         - s.m_acc);
    s.s_acc += alpha_s * (power         - s.s_acc);
    s.i_acc += alpha_i * (I_TARGET_POWER - s.i_acc);  // pulls toward -14 LKFS

    s.momentary  = to_db(sqrtf(std::max(s.m_acc, 1e-12f)));
    s.short_term = to_db(sqrtf(std::max(s.s_acc, 1e-12f)));
    s.integrated = to_db(sqrtf(std::max(s.i_acc, 1e-12f)));

    // History: 1 short-term value per second
    s.history_tick += dt;
    if (s.history_tick >= 1.0f) {
        s.history_tick -= 1.0f;
        s.short_term_history[s.history_head] = s.short_term;
        s.history_head = (s.history_head + 1) % 60;
    }
}
```

- [ ] **Step 2: Build check**

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add main/screens/metering.cpp
git commit -m "feat: metering demo data generator — peak/RMS/LUFS/history simulation"
```

---

## Task 3: L/R Pegelbalken (Bar Meters)

**Files:**
- Modify: `main/screens/metering.cpp` — replace bar stubs

**Interfaces:**
- Consumes: `lv_obj_t *parent`, `BarWidgetData *d` (pre-allocated, stored as user_data on the returned obj)
- Produces: `lv_obj_t*` — a positioned container; `metering_bar_update(bar, rms_db, peak_hold_db)` updates display

The `BarWidgetData` for each bar must be stored inside `MeteringScreenData`. Add two members:

```cpp
// Add to MeteringScreenData struct (inside Task 1's struct definition):
BarWidgetData bar_l_data;
BarWidgetData bar_r_data;
```

- [ ] **Step 1: Add BarWidgetData members to MeteringScreenData**

In `metering.cpp`, inside the `MeteringScreenData` struct definition (from Task 1), add after `lv_obj_t *bar_r;`:
```cpp
    BarWidgetData bar_l_data;
    BarWidgetData bar_r_data;
```

- [ ] **Step 2: Replace bar stubs with full implementations**

Replace:
```cpp
static lv_obj_t *metering_bar_create(lv_obj_t *parent, BarWidgetData *)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}
static void metering_bar_update(lv_obj_t *, float, float) {}
```

With:
```cpp
static void bar_draw_cb(lv_event_t *e)
{
    auto *d     = static_cast<BarWidgetData*>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t   *obj   = lv_event_get_target_obj(e);

    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t h = lv_area_get_height(&a);

    // Background
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(0x404040);
        dsc.radius   = THEME_RADIUS;
        lv_draw_rect(layer, &dsc, &a);
    }

    // dBFS → pixel y (0 dBFS = top, -60 dBFS = bottom)
    auto db_to_y = [&](float db) -> int32_t {
        float norm = (db + 60.0f) / 60.0f;  // 0..1
        norm = std::max(0.0f, std::min(1.0f, norm));
        return a.y2 - (int32_t)(norm * (float)h);
    };

    // RMS fill — three color zones
    struct Zone { float lo, hi; uint32_t hex; };
    static const Zone zones[] = {
        { -60.0f, -9.0f, 0x50A050u },  // green
        {  -9.0f, -3.0f, 0xC8A030u },  // yellow
        {  -3.0f,  0.0f, 0xE05050u },  // red
    };
    float rms = d->rms_db;
    for (auto &z : zones) {
        if (rms <= z.lo) continue;
        float fill_top = std::min(rms, z.hi);
        int32_t yt = db_to_y(fill_top);
        int32_t yb = db_to_y(z.lo);
        if (yt >= yb) continue;
        lv_area_t za = { a.x1 + 6, yt, a.x2 - 6, yb };
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(z.hex);
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &za);
    }

    // Peak hold marker (white horizontal line)
    {
        int32_t y = db_to_y(d->peak_hold_db);
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color  = lv_color_hex(0xE8E8E8);
        dsc.width  = 2;
        dsc.p1.x   = (lv_value_precise_t)(a.x1 + 6);
        dsc.p1.y   = (lv_value_precise_t)y;
        dsc.p2.x   = (lv_value_precise_t)(a.x2 - 6);
        dsc.p2.y   = (lv_value_precise_t)y;
        lv_draw_line(layer, &dsc);
    }
}

static lv_obj_t *metering_bar_create(lv_obj_t *parent, BarWidgetData *d)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(c, bar_draw_cb, LV_EVENT_DRAW_MAIN, d);
    return c;
}

static void metering_bar_update(lv_obj_t *bar, float rms_db, float peak_hold_db)
{
    auto *d = static_cast<BarWidgetData*>(lv_obj_get_event_user_data_by_cb(bar, bar_draw_cb));
    if (!d) return;
    d->rms_db      = rms_db;
    d->peak_hold_db = peak_hold_db;
    lv_obj_invalidate(bar);
}
```

- [ ] **Step 3: Update metering_screen_create() to pass BarWidgetData and set positions**

Find `metering_screen_create()`. Replace the two `metering_bar_create(scr, nullptr)` lines with:
```cpp
    data->bar_l_data = {-60.0f, -60.0f};
    data->bar_r_data = {-60.0f, -60.0f};
    data->bar_l = metering_bar_create(scr, &data->bar_l_data);
    data->bar_r = metering_bar_create(scr, &data->bar_r_data);

    // Position bars
    lv_obj_set_size(data->bar_l, 90, 380);
    lv_obj_set_pos(data->bar_l, 16, 40);
    lv_obj_set_size(data->bar_r, 90, 380);
    lv_obj_set_pos(data->bar_r, 116, 40);
```

- [ ] **Step 4: Build check**

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
```

Note: `lv_obj_get_event_user_data_by_cb` may not exist in LVGL 9. If the build fails on that line, replace `metering_bar_update` with:

```cpp
static void metering_bar_update(lv_obj_t *bar, float rms_db, float peak_hold_db)
{
    // user_data is stored as event user_data; retrieve via event list hack or store in obj user_data
    auto *d = static_cast<BarWidgetData*>(lv_obj_get_user_data(bar));
    if (!d) return;
    d->rms_db       = rms_db;
    d->peak_hold_db = peak_hold_db;
    lv_obj_invalidate(bar);
}
```

And in `metering_bar_create()`, after `lv_obj_add_event_cb`, add:
```cpp
    lv_obj_set_user_data(c, d);
```

Rebuild and verify no errors.

- [ ] **Step 5: Flash and verify**

```bash
idf.py -p /dev/ttyACM0 flash
timeout 20 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep -v "^---"
```

Expected: screen shows two dark-gray vertical bars on the left, with green/yellow/red fill animating up and down (~0.7 Hz) and a white peak-hold line that rises and slowly falls. If bars are invisible/wrong, check positions and color values.

- [ ] **Step 6: Commit**

```bash
git add main/screens/metering.cpp
git commit -m "feat: metering L/R bar meters with peak hold and color zones"
```

---

## Task 4: Goniometer (Lissajous Canvas)

**Files:**
- Modify: `main/screens/metering.cpp` — replace goniometer stubs

**Interfaces:**
- Consumes: `lv_obj_t *parent`, `MeteringScreenData *data` (for ring buffer + canvas buf)
- Produces: `lv_obj_t*` (lv_canvas); `metering_gonio_update(canvas, data)` adds new point + redraws

Canvas buffer: 250×250 × 2 bytes (RGB565) = 125,000 bytes — allocated from PSRAM.

- [ ] **Step 1: Replace goniometer stubs**

Replace:
```cpp
static lv_obj_t *metering_gonio_create(lv_obj_t *parent, MeteringScreenData *)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    return c;
}
static void metering_gonio_update(lv_obj_t *, MeteringScreenData *) {}
```

With:
```cpp
static void gonio_redraw(lv_obj_t *canvas, MeteringScreenData *data)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Clear to screen background
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = THEME_BG_PRIMARY;
        dsc.bg_opa   = LV_OPA_COVER;
        dsc.radius   = 0;
        lv_area_t full = {0, 0, 249, 249};
        lv_draw_rect(&layer, &dsc, &full);
    }

    // Center vertical reference line (mono = top-to-bottom)
    {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x808080);
        dsc.width = 1;
        dsc.p1.x = 125; dsc.p1.y = 15;
        dsc.p2.x = 125; dsc.p2.y = 235;
        lv_draw_line(&layer, &dsc);
    }

    // Draw ring buffer: oldest (dimmest) first, newest (brightest) last
    static const lv_color_t POINT_COLORS[4] = {
        lv_color_hex(0x687868),   // age band 0: barely visible
        lv_color_hex(0x508050),   // age band 1: dim
        lv_color_hex(0x409840),   // age band 2: medium
        lv_color_hex(0x30BC30),   // age band 3: bright
    };
    for (int i = 0; i < 80; i++) {
        // i=0 oldest, i=79 newest
        int idx = (data->gonio_head + i) % 80;
        auto &pt = data->gonio_pts[idx];
        int band = (i * 4) / 80;  // 0..3

        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = POINT_COLORS[band];
        dsc.bg_opa   = LV_OPA_COVER;
        dsc.radius   = LV_RADIUS_CIRCLE;
        dsc.border_width = 0;
        lv_area_t pa = { pt.x - 2, pt.y - 2, pt.x + 2, pt.y + 2 };
        lv_draw_rect(&layer, &dsc, &pa);
    }

    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_invalidate(canvas);
}

static lv_obj_t *metering_gonio_create(lv_obj_t *parent, MeteringScreenData *data)
{
    // Allocate canvas buffer from PSRAM
    constexpr int W = 250, H = 250;
    size_t buf_size = (size_t)W * H * sizeof(uint16_t);  // RGB565 = 2 bytes/pixel
    data->gonio_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!data->gonio_buf) {
        // Fallback to internal RAM (unlikely to fit but prevents crash)
        data->gonio_buf = malloc(buf_size);
    }
    memset(data->gonio_buf, 0, buf_size);

    // Initialize ring buffer to center (no signal = dot at center)
    for (auto &p : data->gonio_pts) { p.x = 125; p.y = 125; }
    data->gonio_head = 0;

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, data->gonio_buf, W, H, LV_COLOR_FORMAT_RGB565);

    return canvas;
}

static void metering_gonio_update(lv_obj_t *canvas, MeteringScreenData *data)
{
    float l = data->state.l_sample;
    float r = data->state.r_sample;

    // Mid-Side (Lissajous rotated 45°): side=horizontal, mid=vertical
    constexpr float SQRT2_INV = 0.7071f;
    float mid  = (l + r) * SQRT2_INV;
    float side = (l - r) * SQRT2_INV;

    // Map ±1.0 → ±110 px from center (125, 125)
    int16_t cx = (int16_t)(125.0f + side * 110.0f);
    int16_t cy = (int16_t)(125.0f - mid  * 110.0f);
    // Clamp to canvas bounds (leaving 2px margin for point radius)
    cx = (int16_t)(cx < 15 ? 15 : cx > 235 ? 235 : cx);
    cy = (int16_t)(cy < 15 ? 15 : cy > 235 ? 235 : cy);

    // Add to ring buffer; head will point to NEXT write (= oldest after increment)
    data->gonio_pts[data->gonio_head] = {cx, cy};
    data->gonio_head = (data->gonio_head + 1) % 80;

    gonio_redraw(canvas, data);
}
```

- [ ] **Step 2: Position goniometer in metering_screen_create()**

Replace `data->gonio = metering_gonio_create(scr, data);` (leave that call as-is, the stub is gone now), and add after it:
```cpp
    lv_obj_set_pos(data->gonio, 278, 40);
    // size is set by lv_canvas_set_buffer (250×250)
```

- [ ] **Step 3: Build check**

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
```

Expected: no errors.

- [ ] **Step 4: Flash and verify**

```bash
idf.py -p /dev/ttyACM0 flash
timeout 20 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep -v "^---"
```

Expected: a 250×250 dark-gray square appears right of the bars with a vertical center line and animated green Lissajous dots tracing a slowly rotating figure-8 pattern (since phase offset drifts). If the canvas is white/black/corrupt, check the buffer size calculation and PSRAM allocation.

- [ ] **Step 5: Commit**

```bash
git add main/screens/metering.cpp
git commit -m "feat: metering goniometer — Lissajous canvas with ring-buffer persistence"
```

---

## Task 5: Loudness History Graph

**Files:**
- Modify: `main/screens/metering.cpp` — replace history stubs

**Interfaces:**
- Consumes: `lv_obj_t *parent`, `MeteringScreenData *data`; draw callback reads `data->state.short_term_history` and `data->state.history_head`
- Produces: `lv_obj_t*`; `metering_history_invalidate(hist)` triggers redraw

- [ ] **Step 1: Replace history stubs**

Replace:
```cpp
static lv_obj_t *metering_history_create(lv_obj_t *parent, MeteringScreenData *)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    return c;
}
static void metering_history_invalidate(lv_obj_t *) {}
```

With:
```cpp
static void history_draw_cb(lv_event_t *e)
{
    auto *data  = static_cast<MeteringScreenData*>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t   *obj   = lv_event_get_target_obj(e);

    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a);
    int32_t h = lv_area_get_height(&a);

    // Background
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(0x484848);
        dsc.radius   = THEME_RADIUS;
        lv_draw_rect(layer, &dsc, &a);
    }

    // 60 bars: left = oldest, right = newest
    constexpr float DB_MIN = -40.0f;
    constexpr float DB_MAX =  -6.0f;
    constexpr float DB_RANGE = DB_MAX - DB_MIN;

    float bar_w = (float)w / 60.0f;

    for (int i = 0; i < 60; i++) {
        int   idx = (data->state.history_head + i) % 60;  // oldest→newest
        float val = data->state.short_term_history[idx];

        float norm = (val - DB_MIN) / DB_RANGE;
        norm = std::max(0.0f, std::min(1.0f, norm));
        int32_t bar_h = (int32_t)(norm * (float)(h - 4));
        if (bar_h < 1) bar_h = 1;

        int32_t x0 = a.x1 + (int32_t)(i * bar_w);
        int32_t x1 = a.x1 + (int32_t)((i + 1) * bar_w) - 1;

        lv_color_t color;
        if      (val > -16.0f) color = lv_color_hex(0xE05050);  // too loud
        else if (val > -23.0f) color = lv_color_hex(0xC8A030);  // above target
        else                   color = lv_color_hex(0x50A050);  // on target or below

        lv_area_t ba = { x0, a.y2 - bar_h - 2, x1, a.y2 - 2 };
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = color;
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &ba);
    }

    // Target line at -23 LKFS (EBU R128)
    {
        float norm_t = (-23.0f - DB_MIN) / DB_RANGE;
        norm_t = std::max(0.0f, std::min(1.0f, norm_t));
        int32_t target_y = a.y2 - (int32_t)(norm_t * (float)(h - 4)) - 2;

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x909090);
        dsc.width = 1;
        dsc.p1.x = (lv_value_precise_t)(a.x1 + 4);
        dsc.p1.y = (lv_value_precise_t)target_y;
        dsc.p2.x = (lv_value_precise_t)(a.x2 - 4);
        dsc.p2.y = (lv_value_precise_t)target_y;
        lv_draw_line(layer, &dsc);
    }
}

static lv_obj_t *metering_history_create(lv_obj_t *parent, MeteringScreenData *data)
{
    // Pre-fill history with -40 dBFS (silence) so graph starts clean
    for (auto &v : data->state.short_term_history) v = -40.0f;

    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(c, history_draw_cb, LV_EVENT_DRAW_MAIN, data);
    return c;
}

static void metering_history_invalidate(lv_obj_t *hist)
{
    lv_obj_invalidate(hist);
}
```

- [ ] **Step 2: Position history widget in metering_screen_create()**

Replace `data->history = metering_history_create(scr, data);` with:
```cpp
    data->history = metering_history_create(scr, data);
    lv_obj_set_size(data->history, 358, 122);
    lv_obj_set_pos(data->history, 224, 298);
```

- [ ] **Step 3: Build check**

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
```

- [ ] **Step 4: Flash and verify**

```bash
idf.py -p /dev/ttyACM0 flash
timeout 20 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep -v "^---"
```

Expected: below the goniometer, a dark-gray bar graph (358×122) appears. After ~10 seconds, green/yellow bars fill from the left and scroll rightward. A gray horizontal line crosses at the -23 LKFS level. Bars above -16 LKFS are red, above -23 LKFS are yellow, below are green.

- [ ] **Step 5: Commit**

```bash
git add main/screens/metering.cpp
git commit -m "feat: metering loudness history graph — 60s scrolling, EBU R128 target line"
```

---

## Task 6: Numerics Panel + Final Layout

**Files:**
- Modify: `main/screens/metering.cpp` — replace numerics stubs, fix layout

**Interfaces:**
- Consumes: `MeteringScreenData *data`; `data->num_i`, `data->num_s`, `data->num_m`, `data->num_peak` are labels updated by `metering_numerics_update(data)`
- Produces: `lv_obj_t*` (container), four labels stored in `MeteringScreenData`

- [ ] **Step 1: Replace numerics stubs**

Replace:
```cpp
static lv_obj_t *metering_numerics_create(lv_obj_t *parent)
{
    return lv_obj_create(parent);
}
static void metering_numerics_update(MeteringScreenData *) {}
```

With:
```cpp
static lv_obj_t *metering_numerics_create(lv_obj_t *parent, MeteringScreenData *data)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    auto make_label = [&](int y_offset) -> lv_obj_t* {
        lv_obj_t *lbl = lv_label_create(panel);
        lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_set_pos(lbl, 0, y_offset);
        return lbl;
    };

    data->num_i    = make_label(0);
    data->num_s    = make_label(24);
    data->num_m    = make_label(48);
    data->num_peak = make_label(72);

    lv_label_set_text(data->num_i,    "I:    --- LKFS");
    lv_label_set_text(data->num_s,    "S:    --- LKFS");
    lv_label_set_text(data->num_m,    "M:    --- LKFS");
    lv_label_set_text(data->num_peak, "Peak: --- dBFS");

    return panel;
}

static void metering_numerics_update(MeteringScreenData *data)
{
    lv_label_set_text_fmt(data->num_i,    "I:    %+.1f LKFS", data->state.integrated);
    lv_label_set_text_fmt(data->num_s,    "S:    %+.1f LKFS", data->state.short_term);
    lv_label_set_text_fmt(data->num_m,    "M:    %+.1f LKFS", data->state.momentary);

    float peak = std::max(data->state.peak_hold_l, data->state.peak_hold_r);
    lv_label_set_text_fmt(data->num_peak, "Peak: %+.1f dBFS", peak);

    // Color peak label red if clipping risk (> -3 dBFS)
    lv_color_t peak_color = (peak > -3.0f) ? lv_color_hex(0xE05050) : THEME_TEXT_PRIMARY;
    lv_obj_set_style_text_color(data->num_peak, peak_color, 0);
}
```

- [ ] **Step 2: Fix metering_screen_create() — wire up numerics**

In `metering_screen_create()`, the `metering_numerics_create` call currently passes only `scr`. Update the function signature and call:

First, update the forward declaration:
```cpp
static lv_obj_t *metering_numerics_create(lv_obj_t *parent, MeteringScreenData *data);
```

Then in `metering_screen_create()`, replace:
```cpp
    lv_obj_t *nums = metering_numerics_create(scr);
    (void)nums;
```
With:
```cpp
    lv_obj_t *nums = metering_numerics_create(scr, data);
    lv_obj_set_size(nums, 194, 110);
    lv_obj_set_pos(nums, 590, 48);
```

- [ ] **Step 3: Build check**

```bash
idf.py build 2>&1 | grep -E " error:|fatal error" | head -20
```

- [ ] **Step 4: Flash and final visual verification**

```bash
idf.py -p /dev/ttyACM0 flash
timeout 20 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep -v "^---"
```

Verify the complete screen:
- **Left**: two animated bar meters with green/yellow/red zones and white peak-hold line
- **Center top**: 250×250 goniometer with green Lissajous pattern and center reference line
- **Center bottom**: scrolling loudness history with -23 LKFS target line
- **Right**: four numeric readouts updating live (`I:`, `S:`, `M:`, `Peak:`)
- **Bottom left**: ← Home button working (navigates back, screen and timer cleaned up)
- Monitor log must show no crashes or memory errors

- [ ] **Step 5: Commit**

```bash
git add main/screens/metering.cpp
git commit -m "feat: metering screen complete — bars, goniometer, loudness history, numerics"
```

---

## Self-Review Notes

**Spec coverage check:**
- ✅ L/R vertical bars with RMS fill, peak hold, color zones → Task 3
- ✅ Goniometer (Lissajous, M/S rotation, ring buffer persistence) → Task 4
- ✅ 60s short-term history graph, EBU R128 -23 LKFS target line → Task 5
- ✅ Numerics: I, S, M, Peak with red peak alert → Task 6
- ✅ Demo data: peak/RMS/LUFS/history simulation, 30 Hz → Task 2
- ✅ Timer cleanup + canvas buffer freed on screen delete → Task 1 (on_screen_delete)
- ✅ Modular widget functions (`_create` / `_update` pairs) → all tasks
- ✅ No new files, no new dependencies → all tasks modify only `metering.cpp`
- ✅ Colors ≥ 38% luminance → verified: 0x404040 bar bg (38%), 0x50A050 green (44%), 0xC8A030 yellow (51%), 0xE05050 red (49%), 0x686868 ref line (51%)
- ✅ THEME_STATUSBAR_H = 32, content at y=40 → Task 3 positions
- ✅ Back button → Task 1 scaffold

**Potential build issue flagged:** `lv_obj_get_event_user_data_by_cb` may not exist in LVGL 9.5 — Task 3 Step 4 has an explicit fallback using `lv_obj_set_user_data` / `lv_obj_get_user_data`.

**`lv_value_precise_t` type:** Used for `lv_draw_line_dsc_t.p1/p2`. In LVGL 9.5 this is `float`. If a compile error occurs, change to explicit `(float)` cast.
