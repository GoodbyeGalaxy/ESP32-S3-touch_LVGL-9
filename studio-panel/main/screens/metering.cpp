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

// dt in seconds; called at ~30 Hz from LVGL task — all math must be fast (no heavy trig loops)
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

// Creates a bar widget container (stubs return placeholder obj); called from metering_screen_create()
static lv_obj_t *metering_bar_create(lv_obj_t *parent, BarWidgetData *)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

// Updates bar widget RMS and peak-hold values; idempotent if called from timer
static void metering_bar_update(lv_obj_t *, float, float) {}

// Creates goniometer canvas object; needs PSRAM buffer allocation in later task
static lv_obj_t *metering_gonio_create(lv_obj_t *parent, MeteringScreenData *)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    return c;
}

// Redraws goniometer from data->gonio_pts array
static void metering_gonio_update(lv_obj_t *, MeteringScreenData *) {}

// Creates history graph (ring-buffer visualization); drawn via draw_cb
static lv_obj_t *metering_history_create(lv_obj_t *parent, MeteringScreenData *)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    return c;
}

// Marks history widget for redraw on next LVGL cycle (idempotent if called from timer)
static void metering_history_invalidate(lv_obj_t *) {}

// Creates numeric readouts container; positions num_i, num_s, num_m, num_peak inside parent
static lv_obj_t *metering_numerics_create(lv_obj_t *parent)
{
    return lv_obj_create(parent);
}

// Updates all numeric readouts from metering state; called from timer
static void metering_numerics_update(MeteringScreenData *) {}

// ── Timer + screen lifecycle ──────────────────────────────────────────────────

// ~33ms tick: advances state, updates all widgets; idempotent (timer holds strong ref to screen)
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

// Cleanup on screen delete — stops timer, frees PSRAM, deletes data
static void on_screen_delete(lv_event_t *e)
{
    auto *data = static_cast<MeteringScreenData*>(lv_event_get_user_data(e));
    if (data->timer)     lv_timer_delete(data->timer);
    if (data->gonio_buf) heap_caps_free(data->gonio_buf);
    delete data;
}

// Back button handler — loads home screen with right-slide animation
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
