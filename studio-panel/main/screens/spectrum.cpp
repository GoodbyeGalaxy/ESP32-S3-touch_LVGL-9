#include "spectrum.h"
#include "audio_data.h"
#include "theme.h"
#include "screens/home.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <cmath>
#include <cstring>
#include <algorithm>

static const char *TAG = "spectrum";

// ── Data ──────────────────────────────────────────────────────────────────────

struct SpectrumScreenData {
    // Three separate screen objects (each is an lv_obj root)
    lv_obj_t *scr_bars;      // View 1 — classic bars
    lv_obj_t *scr_curve;     // View 2 — FFT area chart
    lv_obj_t *scr_waterfall; // View 3 — scrolling heatmap

    // Per-view draw containers
    lv_obj_t *bars_canvas;
    lv_obj_t *curve_canvas;
    lv_obj_t *wf_canvas;    // lv_canvas with PSRAM buffer
    void     *wf_buf;       // PSRAM allocated waterfall buffer

    // Shared
    lv_timer_t *timer;
    float    smoothed[256];  // exponential MA; shared across views
    bool     frozen;         // when true: timer runs but display not invalidated

    // Freeze icon (visible on all views when frozen)
    lv_obj_t *freeze_icon_bars;
    lv_obj_t *freeze_icon_curve;
    lv_obj_t *freeze_icon_wf;

    // Peak hold for bars view
    float    peak_hold[256];
    float    peak_hold_timer[256];

    // Waterfall
    uint8_t  color_preset; // 0=Classic 1=Green 2=Warm 3=Purple
    bool     wf_rtl;       // true = right-to-left (default)

    // Context menu
    lv_obj_t *ctx_menu;    // nullptr when hidden

    // BOOT button state
    volatile int64_t btn_press_us; // timestamp of last press (from ISR)
    volatile bool    btn_event;    // set by ISR on release
    volatile bool    btn_long;     // set by ISR: true if press >= 1s
};

// Single data instance per screen lifetime — allocated on create, freed on delete
static SpectrumScreenData *s_data = nullptr;

// ── BOOT button ───────────────────────────────────────────────────────────────

// ISR: records press/release timing; sets btn_event + btn_long on release.
// IRAM_ATTR required for ISR functions.
static void IRAM_ATTR boot_btn_isr(void *arg)
{
    auto *d = static_cast<SpectrumScreenData*>(arg);
    int level = gpio_get_level(GPIO_NUM_0);
    int64_t now = esp_timer_get_time();
    if (level == 0) {
        d->btn_press_us = now;                          // falling edge = press
    } else {
        d->btn_long  = (now - d->btn_press_us) >= 1000000; // 1s threshold
        d->btn_event = true;                            // rising edge = release
    }
}

// Configures GPIO_NUM_0 as input with pull-up and ANYEDGE interrupt.
// Installs ISR service (idempotent if already installed) and registers boot_btn_isr.
static void boot_btn_init(SpectrumScreenData *d)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,  // both press and release
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_NUM_0, boot_btn_isr, d);
}

// Removes the BOOT button ISR handler; does not uninstall the service globally.
static void boot_btn_deinit()
{
    gpio_isr_handler_remove(GPIO_NUM_0);
}

// ── Navigation ────────────────────────────────────────────────────────────────

static void navigate_to_bars(SpectrumScreenData *d);   // forward decl
static void navigate_to_curve(SpectrumScreenData *d);
static void navigate_to_wf(SpectrumScreenData *d);
static void navigate_back_home();

// Loads scr_bars with a right-slide animation (returning from curve/waterfall).
static void navigate_to_bars(SpectrumScreenData *d)
{
    lv_screen_load_anim(d->scr_bars, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

// Loads scr_curve with a left-slide animation (going forward from bars).
static void navigate_to_curve(SpectrumScreenData *d)
{
    lv_screen_load_anim(d->scr_curve, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

// Loads scr_waterfall with a left-slide animation (going forward from curve).
static void navigate_to_wf(SpectrumScreenData *d)
{
    lv_screen_load_anim(d->scr_waterfall, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

// Creates a fresh home screen and loads it (deletes old spectrum screens via auto_del).
static void navigate_back_home()
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
}

// ── Forward declarations (implemented in Tasks 6-8) ──────────────────────────

static void spectrum_bars_draw(lv_event_t *e);
static void spectrum_curve_draw(lv_event_t *e);
static void spectrum_wf_update(SpectrumScreenData *d);
static void spectrum_ctx_menu_show(SpectrumScreenData *d);
static void spectrum_ctx_menu_hide(SpectrumScreenData *d);

// ── Placeholder stubs (replaced in Tasks 6-8) ────────────────────────────────

// maps screen pixel to FFT bin on log frequency scale (20Hz–20kHz)
static int x_to_bin(int x, int w)
{
    constexpr float F_MIN   = 20.0f;
    constexpr float F_MAX   = 20000.0f;
    constexpr float NYQUIST = 22050.0f;
    float ratio = (float)x / (float)w;
    float freq  = F_MIN * powf(F_MAX / F_MIN, ratio);
    int   bin   = (int)(freq / NYQUIST * 256.0f);
    return (bin < 0) ? 0 : (bin > 255) ? 255 : bin;
}

// Interpolates between two lv_color_t values. t = 0.0..1.0.
static lv_color_t color_lerp(lv_color_t a, lv_color_t b, float t)
{
    uint8_t r  = (uint8_t)(a.red   + t * ((float)b.red   - (float)a.red));
    uint8_t g  = (uint8_t)(a.green + t * ((float)b.green - (float)a.green));
    uint8_t bu = (uint8_t)(a.blue  + t * ((float)b.blue  - (float)a.blue));
    return lv_color_make(r, g, bu);
}

// LV_EVENT_DRAW_MAIN; reads smoothed[] and peak_hold[] from SpectrumScreenData
static void spectrum_bars_draw(lv_event_t *e)
{
    auto *d     = static_cast<SpectrumScreenData*>(lv_event_get_user_data(e));
    auto *layer = lv_event_get_layer(e);
    auto *obj   = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a);
    int32_t h = lv_area_get_height(&a);

    // Black background (pure visualisation area — luminance rule exempt)
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(0x0A0A0A);
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &a);
    }

    // Color stops: dark green → bright green → yellow → red (by amplitude)
    static const lv_color_t C0 = lv_color_make(0x20, 0x50, 0x20);  // dark green  (quiet)
    static const lv_color_t C1 = lv_color_make(0x30, 0xBC, 0x30);  // bright green
    static const lv_color_t C2 = lv_color_make(0xC8, 0xA0, 0x30);  // yellow
    static const lv_color_t C3 = lv_color_make(0xE0, 0x50, 0x50);  // red         (loud)

    for (int x = 0; x < w; x++) {
        int   bin   = x_to_bin(x, w);
        float mag   = d->smoothed[bin];          // 0.0..1.0
        int32_t bar_h = (int32_t)(mag * (float)h);
        if (bar_h < 1) bar_h = 1;

        // Bar color interpolated by magnitude
        lv_color_t col;
        if      (mag < 0.33f) col = color_lerp(C0, C1, mag / 0.33f);
        else if (mag < 0.66f) col = color_lerp(C1, C2, (mag - 0.33f) / 0.33f);
        else                  col = color_lerp(C2, C3, (mag - 0.66f) / 0.34f);

        lv_area_t ba = { a.x1 + (lv_coord_t)x, a.y2 - bar_h,
                         a.x1 + (lv_coord_t)x, a.y2 };
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = col;
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &ba);

        // Peak hold: 1px white dot
        if (d->peak_hold[bin] > 0.01f) {
            int32_t py = a.y2 - (int32_t)(d->peak_hold[bin] * (float)h);
            lv_area_t pa = { a.x1 + (lv_coord_t)x, py,
                             a.x1 + (lv_coord_t)x, py };
            lv_draw_rect_dsc_t pdsc;
            lv_draw_rect_dsc_init(&pdsc);
            pdsc.bg_color = lv_color_hex(0xE8E8E8);
            lv_draw_rect(layer, &pdsc, &pa);
        }
    }
}

// LV_EVENT_DRAW_MAIN; draws smoothed[] as filled area chart with blue→cyan gradient.
// Reuses x_to_bin() from Task 6 — must remain in same translation unit.
static void spectrum_curve_draw(lv_event_t *e)
{
    auto *d     = static_cast<SpectrumScreenData*>(lv_event_get_user_data(e));
    auto *layer = lv_event_get_layer(e);
    auto *obj   = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a);
    int32_t h = lv_area_get_height(&a);

    // Very dark background — pure visualisation, luminance rule exempt
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(0x050510);
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &a);
    }

    // Filled area: trapezoids between adjacent X columns
    for (int x = 0; x < w - 1; x++) {
        float mag0 = d->smoothed[x_to_bin(x,   w)];
        float mag1 = d->smoothed[x_to_bin(x+1, w)];
        float avg  = (mag0 + mag1) * 0.5f;

        int32_t top = a.y2 - (int32_t)(std::max(mag0, mag1) * (float)h);
        lv_area_t fa = { a.x1 + x, top, a.x1 + x + 1, a.y2 };

        // Gradient: dark blue base → cyan/white at peak
        lv_color_t col;
        if (avg < 0.5f) {
            col = lv_color_make(
                (uint8_t)(0x1A * avg * 2),
                (uint8_t)(0x3A * avg * 2),
                (uint8_t)(0x8A + (uint8_t)(0x30 * avg * 2)));
        } else {
            float t = (avg - 0.5f) * 2.0f;
            col = lv_color_make(
                (uint8_t)(0x1A + (uint8_t)(0xD0 * t)),
                (uint8_t)(0x3A + (uint8_t)(0x96 * t)),
                (uint8_t)(0xBA + (uint8_t)(0x45 * t)));
        }

        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = col;
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &fa);
    }

    // Bright top line (cyan)
    {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x70D0FF);
        dsc.width = 2;
        for (int x = 0; x < w - 1; x++) {
            dsc.p1.x = (lv_value_precise_t)(a.x1 + x);
            dsc.p1.y = (lv_value_precise_t)(a.y2 - (int32_t)(d->smoothed[x_to_bin(x,   w)] * (float)h));
            dsc.p2.x = (lv_value_precise_t)(a.x1 + x + 1);
            dsc.p2.y = (lv_value_precise_t)(a.y2 - (int32_t)(d->smoothed[x_to_bin(x+1, w)] * (float)h));
            lv_draw_line(layer, &dsc);
        }
    }
}
static void spectrum_wf_update(SpectrumScreenData *) {}
static void spectrum_ctx_menu_show(SpectrumScreenData *) {}
static void spectrum_ctx_menu_hide(SpectrumScreenData *) {}

// ── Freeze icon helper ────────────────────────────────────────────────────────

// Creates a small ❚❚ label in the top-right corner of parent screen.
static lv_obj_t *make_freeze_icon(lv_obj_t *parent)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(lbl, THEME_ACCENT, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_RIGHT, -8, THEME_STATUSBAR_H + 4);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    return lbl;
}

// ── Timer ─────────────────────────────────────────────────────────────────────

// ~30 Hz tick: reads queue, smooths bins, handles BOOT button, updates active view.
// IN:  lv_timer_t* with user_data = SpectrumScreenData*
// OUT: none (side-effects: updates smoothed[], peak_hold[], invalidates canvas)
static void spectrum_timer_cb(lv_timer_t *timer)
{
    auto *d = static_cast<SpectrumScreenData*>(lv_timer_get_user_data(timer));

    // Handle BOOT button event (set by ISR)
    if (d->btn_event) {
        d->btn_event = false;
        if (d->btn_long) {
            d->frozen = !d->frozen;
            auto set_icon = [&](lv_obj_t *icon) {
                if (icon) {
                    d->frozen ? lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN)
                              : lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                }
            };
            set_icon(d->freeze_icon_bars);
            set_icon(d->freeze_icon_curve);
            set_icon(d->freeze_icon_wf);
            ESP_LOGI(TAG, "Freeze: %s", d->frozen ? "ON" : "OFF");
        } else {
            // Short press: cycle views bars → curve → waterfall → bars
            lv_obj_t *active = lv_screen_active();
            if (active == d->scr_bars)        navigate_to_curve(d);
            else if (active == d->scr_curve)  navigate_to_wf(d);
            else                              navigate_to_bars(d);
        }
    }

    if (d->frozen) return;

    // Read latest audio packet and update smoothed[] bins
    AudioPacket pkt;
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) {
        constexpr float ALPHA = 0.35f;  // exponential MA — balances responsiveness and smoothness
        for (int i = 0; i < 256; i++) {
            d->smoothed[i] += ALPHA * (pkt.bins[i] - d->smoothed[i]);
        }
        // Update peak hold (bars view)
        for (int i = 0; i < 256; i++) {
            if (d->smoothed[i] > d->peak_hold[i]) {
                d->peak_hold[i] = d->smoothed[i];
                d->peak_hold_timer[i] = 2.0f;  // 2s freeze
            } else if (d->peak_hold_timer[i] > 0) {
                d->peak_hold_timer[i] -= 0.033f;
            } else {
                d->peak_hold[i] = std::max(d->peak_hold[i] - 0.033f * 0.5f, 0.0f);
            }
        }
    }

    // Invalidate whichever view is currently active
    lv_obj_t *active = lv_screen_active();
    if (active == d->scr_bars && d->bars_canvas)
        lv_obj_invalidate(d->bars_canvas);
    else if (active == d->scr_curve && d->curve_canvas)
        lv_obj_invalidate(d->curve_canvas);
    else if (active == d->scr_waterfall)
        spectrum_wf_update(d);
}

// ── Back button callbacks ─────────────────────────────────────────────────────

static void on_back_bars(lv_event_t *)    { navigate_back_home(); }
static void on_back_curve(lv_event_t *)   { navigate_back_home(); }
static void on_back_wf(lv_event_t *)      { navigate_back_home(); }

// ── Screen + canvas creation helpers ─────────────────────────────────────────

// Creates a themed screen with a Back button and a full-screen draw container.
// IN:  back_cb    — LV_EVENT_CLICKED callback for Back button
//      draw_cb    — LV_EVENT_DRAW_MAIN callback for canvas (may be nullptr)
//      d          — shared screen data pointer, passed as user_data to draw_cb
//      canvas_out — receives the created draw container (may be nullptr)
//      icon_out   — receives the freeze icon label (may be nullptr)
// OUT: new screen object (lv_obj_t*)
static lv_obj_t *make_spectrum_screen(lv_event_cb_t back_cb,
                                       lv_event_cb_t draw_cb,
                                       SpectrumScreenData *d,
                                       lv_obj_t **canvas_out,
                                       lv_obj_t **icon_out)
{
    lv_obj_t *scr = theme_make_screen();

    // Full-screen draw area (below statusbar, above back button)
    lv_obj_t *canvas = lv_obj_create(scr);
    lv_obj_remove_style_all(canvas);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(canvas, 800, 480 - THEME_STATUSBAR_H - 60);
    lv_obj_set_pos(canvas, 0, THEME_STATUSBAR_H);
    if (draw_cb) lv_obj_add_event_cb(canvas, draw_cb, LV_EVENT_DRAW_MAIN, d);
    if (canvas_out) *canvas_out = canvas;

    // Back button
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 100, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(lbl);

    // Freeze icon (top-right, hidden by default)
    if (icon_out) *icon_out = make_freeze_icon(scr);

    return scr;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

// LV_EVENT_DELETE callback for scr_bars only.
// Performs full cleanup: timer, ISR handler, PSRAM waterfall buffer, data struct.
// IN:  e — event with user_data = SpectrumScreenData*
static void on_bars_delete(lv_event_t *e)
{
    auto *d = static_cast<SpectrumScreenData*>(lv_event_get_user_data(e));
    // Only fully clean up if this is the last remaining spectrum screen
    // (curve and waterfall may still be alive during animation)
    // Full cleanup when timer is deleted (only once):
    if (!d->timer) return;
    lv_timer_delete(d->timer);
    d->timer = nullptr;
    boot_btn_deinit();
    if (d->wf_buf) { heap_caps_free(d->wf_buf); d->wf_buf = nullptr; }
    s_data = nullptr;
    delete d;
}

// ── Public entry point ────────────────────────────────────────────────────────

// Creates all three spectrum screens and the shared data struct.
// Initialises the BOOT button ISR and the 30 Hz update timer.
// Returns scr_bars (View 1); caller loads it with lv_screen_load() / lv_screen_load_anim().
// Guard: if s_data != nullptr, returns the existing scr_bars immediately (no double-create).
lv_obj_t *spectrum_screen_create()
{
    if (s_data) {
        // Already exists (shouldn't happen but guard anyway)
        return s_data->scr_bars;
    }

    auto *d = new SpectrumScreenData{};
    d->wf_rtl      = true;   // right-to-left default
    d->color_preset = 0;     // Classic

    // Create three screens
    d->scr_bars      = make_spectrum_screen(on_back_bars,  spectrum_bars_draw,  d, &d->bars_canvas,  &d->freeze_icon_bars);
    d->scr_curve     = make_spectrum_screen(on_back_curve, spectrum_curve_draw, d, &d->curve_canvas, &d->freeze_icon_curve);
    d->scr_waterfall = make_spectrum_screen(on_back_wf,    nullptr,             d, nullptr,           &d->freeze_icon_wf);
    // waterfall gets its own canvas in Task 8

    // Cleanup only on bars screen delete (first created, first destroyed on exit)
    lv_obj_add_event_cb(d->scr_bars, on_bars_delete, LV_EVENT_DELETE, d);

    boot_btn_init(d);
    d->timer = lv_timer_create(spectrum_timer_cb, 33, d);

    s_data = d;
    return d->scr_bars;  // caller loads this screen
}
