#include "metering.h"
#include "theme.h"
#include "screens/home.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <cmath>
#include <cstring>
#include <algorithm>

// ── Per-bar draw data ─────────────────────────────────────────────────────────

struct BarWidgetData {
    float rms_db;
    float peak_hold_db;
};

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

    BarWidgetData bar_l_data;
    BarWidgetData bar_r_data;
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

// called by LVGL draw system; user_data is BarWidgetData*
static void bar_draw_cb(lv_event_t *e)
{
    auto *d        = static_cast<BarWidgetData*>(lv_event_get_user_data(e));
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

// d must outlive the returned obj — stored as user_data
static lv_obj_t *metering_bar_create(lv_obj_t *parent, BarWidgetData *d)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(c, bar_draw_cb, LV_EVENT_DRAW_MAIN, d);
    lv_obj_set_user_data(c, d);
    return c;
}

// safe to call from LVGL timer callback without locking
static void metering_bar_update(lv_obj_t *bar, float rms_db, float peak_hold_db)
{
    auto *d = static_cast<BarWidgetData*>(lv_obj_get_user_data(bar));
    if (!d) return;
    d->rms_db       = rms_db;
    d->peak_hold_db = peak_hold_db;
    lv_obj_invalidate(bar);
}

// clears canvas and redraws all 80 ring-buffer points; must be called from LVGL task
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

// allocates gonio_buf from PSRAM; caller frees via data->gonio_buf in on_screen_delete
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

// adds new M/S point to ring buffer then redraws; safe in LVGL timer callback
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

// LV_EVENT_DRAW_MAIN handler; reads state via user_data — no lock needed (LVGL task)
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
    constexpr float DB_MIN   = -40.0f;
    constexpr float DB_MAX   =  -6.0f;
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
        else                   color = lv_color_hex(0x50A050);   // on target or below

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

// pre-fills history with -40 dBFS so graph starts clean, not with garbage
static lv_obj_t *metering_history_create(lv_obj_t *parent, MeteringScreenData *data)
{
    for (auto &v : data->state.short_term_history) v = -40.0f;

    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(c, history_draw_cb, LV_EVENT_DRAW_MAIN, data);
    return c;
}

// triggers redraw; called from timer at ~30 Hz
static void metering_history_invalidate(lv_obj_t *hist)
{
    lv_obj_invalidate(hist);
}

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
    data->bar_l_data = {-60.0f, -60.0f};
    data->bar_r_data = {-60.0f, -60.0f};
    data->bar_l   = metering_bar_create(scr, &data->bar_l_data);
    data->bar_r   = metering_bar_create(scr, &data->bar_r_data);

    // Position bars
    lv_obj_set_size(data->bar_l, 90, 380);
    lv_obj_set_pos(data->bar_l, 16, 40);
    lv_obj_set_size(data->bar_r, 90, 380);
    lv_obj_set_pos(data->bar_r, 116, 40);
    data->gonio   = metering_gonio_create(scr, data);
    lv_obj_set_pos(data->gonio, 278, 40);
    // size is set by lv_canvas_set_buffer (250×250)
    data->history = metering_history_create(scr, data);
    lv_obj_set_size(data->history, 358, 122);
    lv_obj_set_pos(data->history, 224, 298);
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
