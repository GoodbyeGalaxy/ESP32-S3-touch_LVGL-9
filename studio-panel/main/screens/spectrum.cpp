// spectrum.cpp — Spectrum Analyzer Screen
// Layout: Head 32px (statusbar, global), Content 800×392px (Y=32..424),
//         Foot 56px (Y=424..480, THEME_FOOT_BG, ← Home + [BAR]/[WAVE] toggle).
// Visualisation: 64 bars (4 bins averaged), freq-based color gradient, peak-hold 2s.
// Second view: line curve (green, 2px, no peak-hold).

#include "spectrum.h"
#include "audio_data.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/metering.h"
#include "lvgl.h"
#include "esp_log.h"
#include <cmath>
#include <cstring>
#include <algorithm>

static const char *TAG = "spectrum";

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int   SCREEN_W    = 800;
static constexpr int   SCREEN_H    = 480;
static constexpr int   HEAD_H      = THEME_STATUSBAR_H;          // 32
static constexpr int   FOOT_H      = 56;
static constexpr int   CONTENT_Y   = HEAD_H;                     // 32
static constexpr int   CONTENT_H   = SCREEN_H - HEAD_H - FOOT_H; // 392
static constexpr int   FOOT_Y      = CONTENT_Y + CONTENT_H;      // 424

static constexpr int   NUM_BARS    = 64;
static constexpr int   BINS_PER_BAR = 256 / NUM_BARS;            // 4
// Bar widths are computed per-bar as floor(i+1 * W/64) - floor(i * W/64) - 1
// to distribute 800px evenly across 64 bars without accumulated rounding error.

static constexpr int   FREQ_LABEL_H = 52;                        // px reserved at bottom of content for labels
static constexpr int   VIS_H        = CONTENT_H - FREQ_LABEL_H; // 340 px for bars/curve
static constexpr float PEAK_HOLD_S  = 2.0f;
static constexpr float PEAK_FALL_RATE = 0.5f;  // magnitude/s after hold expires
static constexpr float TIMER_PERIOD_MS = 33.0f; // ~30 Hz
static constexpr float DT           = TIMER_PERIOD_MS / 1000.0f;
static constexpr float SMOOTHING    = 0.35f;    // EMA alpha

// ── Data ──────────────────────────────────────────────────────────────────────

struct SpectrumData {
    float bins[NUM_BARS];       // averaged+smoothed FFT magnitude, 0..1
    float peak_hold[NUM_BARS];  // current peak-hold magnitude
    float peak_timer[NUM_BARS]; // seconds until peak starts falling
    bool  wave_mode;            // false=bars, true=line curve

    lv_timer_t *timer;
    lv_obj_t   *scr;
    lv_obj_t   *vis_area;       // draw container (LV_EVENT_DRAW_MAIN)
    lv_obj_t   *mode_btn_lbl;   // label inside [BAR]/[WAVE] button
};

// Single instance per screen lifetime
static SpectrumData *s_data = nullptr;

// ── Color helper ──────────────────────────────────────────────────────────────

// IN:  freq_fraction = 0..1 (0=20Hz/bass, 1=20kHz/treble), magnitude = 0..1 (brightness)
// OUT: lv_color_t — gradient: bass blue → mid green → treble yellow/white
static lv_color_t bin_to_color(float magnitude, float freq_fraction)
{
    // Base gradient by frequency (independent of magnitude):
    //   freq 0.0 → pure blue    0x3B82F6
    //   freq 0.5 → green        0x22C55E
    //   freq 1.0 → yellow/white 0xF0C020
    uint8_t r, g, b;
    if (freq_fraction < 0.5f) {
        float t = freq_fraction * 2.0f; // 0..1
        r = (uint8_t)(0x3B + t * (int)(0x22 - 0x3B));  // 59 → 34
        g = (uint8_t)(0x82 + t * (int)(0xC5 - 0x82));  // 130 → 197
        b = (uint8_t)(0xF6 + t * (int)(0x5E - 0xF6));  // 246 → 94
    } else {
        float t = (freq_fraction - 0.5f) * 2.0f; // 0..1
        r = (uint8_t)(0x22 + t * (int)(0xF0 - 0x22));  // 34 → 240
        g = (uint8_t)(0xC5 + t * (int)(0xC0 - 0xC5));  // 197 → 192
        b = (uint8_t)(0x5E + t * (int)(0x20 - 0x5E));  // 94 → 32
    }
    // Apply magnitude as brightness multiplier (dim bar = less bright color)
    // Minimum brightness ~30% so very quiet bars still show their color
    float bright = 0.30f + 0.70f * magnitude;
    r = (uint8_t)(r * bright);
    g = (uint8_t)(g * bright);
    b = (uint8_t)(b * bright);
    return lv_color_make(r, g, b);
}

// ── Draw callback ─────────────────────────────────────────────────────────────

// IN:  LV_EVENT_DRAW_MAIN on vis_area, user_data = SpectrumData*
// OUT: draws bar spectrum or line curve depending on wave_mode
static void spectrum_draw_cb(lv_event_t *e)
{
    auto *d     = static_cast<SpectrumData*>(lv_event_get_user_data(e));
    auto *layer = lv_event_get_layer(e);
    auto *obj   = lv_event_get_target_obj(e);

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    int32_t w = lv_area_get_width(&area);

    // Draw dark background — pure visualisation area, luminance rule exempt
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(0x0A0A0A);
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &area);
    }

    // Working area for bars/curve (excludes bottom label zone)
    lv_coord_t vis_bot  = area.y1 + (lv_coord_t)VIS_H;  // bars drawn in [area.y1, vis_bot]
    lv_coord_t label_y  = vis_bot + 4;                    // baseline for frequency labels

    if (!d->wave_mode) {
        // ── BAR MODE ─────────────────────────────────────────────────────────

        for (int i = 0; i < NUM_BARS; i++) {
            // Compute pixel-precise bar bounds (avoids gap accumulation)
            lv_coord_t x0 = (lv_coord_t)((float)i       * (float)w / (float)NUM_BARS);
            lv_coord_t x1 = (lv_coord_t)((float)(i + 1) * (float)w / (float)NUM_BARS) - 1;

            float mag          = d->bins[i];
            float freq_frac    = (float)i / (float)(NUM_BARS - 1);
            lv_coord_t bar_h_px = (lv_coord_t)(mag * (float)VIS_H);
            if (bar_h_px < 1) bar_h_px = 1;

            lv_color_t col = bin_to_color(mag, freq_frac);

            lv_area_t ba = {
                (lv_coord_t)(area.x1 + x0),
                (lv_coord_t)(vis_bot - bar_h_px),
                (lv_coord_t)(area.x1 + x1),
                vis_bot
            };
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color = col;
            dsc.radius   = 0;
            lv_draw_rect(layer, &dsc, &ba);

            // Peak-hold marker — 2px height, near-white
            float pk = d->peak_hold[i];
            if (pk > 0.01f) {
                lv_coord_t py = vis_bot - (lv_coord_t)(pk * (float)VIS_H);
                lv_area_t pa = {
                    (lv_coord_t)(area.x1 + x0),
                    (lv_coord_t)(py - 1),
                    (lv_coord_t)(area.x1 + x1),
                    py
                };
                lv_draw_rect_dsc_t pdsc;
                lv_draw_rect_dsc_init(&pdsc);
                pdsc.bg_color = lv_color_hex(0xF0F0F0);
                pdsc.radius   = 0;
                lv_draw_rect(layer, &pdsc, &pa);
            }
        }

    } else {
        // ── WAVE / LINE CURVE MODE ────────────────────────────────────────────
        // Draw green line segments between adjacent bar midpoints. No peak-hold.

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x22C55E);
        dsc.width = 2;

        for (int i = 0; i < NUM_BARS - 1; i++) {
            lv_coord_t x0_mid = (lv_coord_t)(((float)i       + 0.5f) * (float)w / (float)NUM_BARS);
            lv_coord_t x1_mid = (lv_coord_t)(((float)(i + 1) + 0.5f) * (float)w / (float)NUM_BARS);
            lv_coord_t y0     = vis_bot - (lv_coord_t)(d->bins[i]     * (float)VIS_H);
            lv_coord_t y1     = vis_bot - (lv_coord_t)(d->bins[i + 1] * (float)VIS_H);

            dsc.p1.x = (lv_value_precise_t)(area.x1 + x0_mid);
            dsc.p1.y = (lv_value_precise_t)y0;
            dsc.p2.x = (lv_value_precise_t)(area.x1 + x1_mid);
            dsc.p2.y = (lv_value_precise_t)y1;
            lv_draw_line(layer, &dsc);
        }
    }

    // ── Frequency labels (bottom 52px of vis_area) ────────────────────────────
    // Labels: 20Hz, 100, 1k, 5k, 10k, 20k — positioned by log-frequency mapping.
    // The 64 bars span 20Hz–20kHz logarithmically.
    // Bar index for frequency f: i = round((log(f/20) / log(1000)) * (NUM_BARS-1))

    struct FreqLabel { const char *text; float freq; };
    static const FreqLabel LABELS[] = {
        {"20Hz", 20.0f},
        {"100",  100.0f},
        {"1k",   1000.0f},
        {"5k",   5000.0f},
        {"10k",  10000.0f},
        {"20k",  20000.0f},
    };

    lv_draw_label_dsc_t ldsc;
    lv_draw_label_dsc_init(&ldsc);
    ldsc.color = THEME_TEXT_HINT;
    ldsc.font  = THEME_FONT_HINT;

    for (const auto &lbl : LABELS) {
        float log_ratio = logf(lbl.freq / 20.0f) / logf(20000.0f / 20.0f);
        lv_coord_t lx   = (lv_coord_t)(log_ratio * (float)w);
        // Clamp near edges to keep text readable
        if (lx < 2)      lx = 2;
        if (lx > w - 30) lx = (lv_coord_t)(w - 30);

        lv_area_t la = {
            (lv_coord_t)(area.x1 + lx),
            label_y,
            (lv_coord_t)(area.x1 + lx + 60),
            (lv_coord_t)(label_y + 20)
        };
        ldsc.text = lbl.text;
        lv_draw_label(layer, &ldsc, &la);
    }
}

// ── Cleanup ───────────────────────────────────────────────────────────────────

// IN:  LV_EVENT_DELETE on scr, user_data = SpectrumData*
// OUT: timer deleted, s_data freed
static void on_scr_delete(lv_event_t *e)
{
    auto *d = static_cast<SpectrumData*>(lv_event_get_user_data(e));
    if (!d) return;
    if (d->timer) {
        lv_timer_delete(d->timer);
        d->timer = nullptr;
    }
    if (s_data == d) s_data = nullptr;
    delete d;
    ESP_LOGI(TAG, "spectrum screen deleted");
}

// ── Timer ─────────────────────────────────────────────────────────────────────

// IN:  lv_timer_t* with user_data = SpectrumData*, fires every 33ms
// OUT: bins[] updated from queue, peak_hold[] updated, vis_area invalidated
static void spectrum_timer_cb(lv_timer_t *timer)
{
    auto *d = static_cast<SpectrumData*>(lv_timer_get_user_data(timer));
    if (!d || !d->vis_area) return;

    AudioPacket pkt;
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) {
        // Average 4 raw bins per display bar + exponential smoothing
        for (int i = 0; i < NUM_BARS; i++) {
            float avg = 0.0f;
            for (int k = 0; k < BINS_PER_BAR; k++) {
                avg += pkt.bins[i * BINS_PER_BAR + k];
            }
            avg /= (float)BINS_PER_BAR;
            // EMA smoothing
            d->bins[i] += SMOOTHING * (avg - d->bins[i]);

            // Peak-hold logic (bars mode only but computed always)
            if (d->bins[i] > d->peak_hold[i]) {
                d->peak_hold[i]  = d->bins[i];
                d->peak_timer[i] = PEAK_HOLD_S;
            } else if (d->peak_timer[i] > 0.0f) {
                d->peak_timer[i] -= DT;
            } else {
                d->peak_hold[i] = std::max(d->peak_hold[i] - PEAK_FALL_RATE * DT, 0.0f);
            }
        }
    }
    // Always invalidate — if no data arrives, display stays at last known state
    lv_obj_invalidate(d->vis_area);
}

// ── Foot button callbacks ─────────────────────────────────────────────────────

// IN:  LV_EVENT_CLICKED on Home button, user_data unused
// OUT: cleans up spectrum screen, loads home
static void on_home_btn(lv_event_t *e)
{
    (void)e;
    if (!s_data) return;

    // Stop timer before navigation to prevent use-after-free during animation
    if (s_data->timer) {
        lv_timer_delete(s_data->timer);
        s_data->timer = nullptr;
    }

    lv_obj_t *home = home_screen_create();
    // auto_del=true: LVGL deletes s_data->scr after animation; on_scr_delete fires
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
}

// IN:  LV_EVENT_CLICKED on [BAR]/[WAVE] toggle button, user_data = SpectrumData*
// OUT: toggles wave_mode, updates button label, invalidates vis_area
static void on_mode_btn(lv_event_t *e)
{
    auto *d = static_cast<SpectrumData*>(lv_event_get_user_data(e));
    if (!d) return;
    d->wave_mode = !d->wave_mode;
    lv_label_set_text(d->mode_btn_lbl, d->wave_mode ? "WAVE" : "BAR");
    lv_obj_invalidate(d->vis_area);
}

// ── Screen creation ───────────────────────────────────────────────────────────

// IN:  nothing
// OUT: fully built spectrum screen (not loaded — caller does lv_screen_load/anim)
//      Idempotent: returns existing screen if called while one is alive.
lv_obj_t *spectrum_screen_create()
{
    if (s_data) {
        // Guard: already running (e.g. during animation)
        return s_data->scr;
    }

    auto *d     = new SpectrumData{};
    d->wave_mode = false;

    // ── Screen root ───────────────────────────────────────────────────────────
    lv_obj_t *scr = theme_make_screen();
    d->scr = scr;
    lv_obj_add_event_cb(scr, on_scr_delete, LV_EVENT_DELETE, d);

    // ── Content / Visualisation area (Y=32, H=392) ───────────────────────────
    // Covers full content zone including freq-label strip at bottom.
    lv_obj_t *vis = lv_obj_create(scr);
    lv_obj_remove_style_all(vis);
    lv_obj_clear_flag(vis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(vis, 0, CONTENT_Y);
    lv_obj_set_size(vis, SCREEN_W, CONTENT_H);
    lv_obj_add_event_cb(vis, spectrum_draw_cb, LV_EVENT_DRAW_MAIN, d);
    d->vis_area = vis;

    // ── Foot bar (Y=424, H=56) ────────────────────────────────────────────────
    lv_obj_t *foot = lv_obj_create(scr);
    lv_obj_remove_style_all(foot);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(foot, 0, FOOT_Y);
    lv_obj_set_size(foot, SCREEN_W, FOOT_H);
    lv_obj_set_style_bg_color(foot, THEME_FOOT_BG, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);

    // ← Home button (left side, 90×40px)
    lv_obj_t *home_btn = lv_btn_create(foot);
    lv_obj_set_size(home_btn, 90, 40);
    lv_obj_align(home_btn, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_color(home_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(home_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(home_btn, THEME_RADIUS, 0);
    lv_obj_set_style_border_width(home_btn, 0, 0);
    lv_obj_set_style_shadow_width(home_btn, 0, 0);
    lv_obj_add_event_cb(home_btn, on_home_btn, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t *lbl = lv_label_create(home_btn);
        lv_label_set_text(lbl, LV_SYMBOL_LEFT " Home");
        lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_center(lbl);
    }

    // [BAR] / [WAVE] toggle button (right side, 90×40px)
    lv_obj_t *mode_btn = lv_btn_create(foot);
    lv_obj_set_size(mode_btn, 90, 40);
    lv_obj_align(mode_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(mode_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mode_btn, THEME_RADIUS, 0);
    lv_obj_set_style_border_width(mode_btn, 0, 0);
    lv_obj_set_style_shadow_width(mode_btn, 0, 0);
    lv_obj_add_event_cb(mode_btn, on_mode_btn, LV_EVENT_CLICKED, d);
    {
        lv_obj_t *lbl = lv_label_create(mode_btn);
        lv_label_set_text(lbl, "BAR");
        lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_center(lbl);
        d->mode_btn_lbl = lbl;
    }

    // ── 2D Swipe — routes through nav_controller ──────────────────────────────
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        nav_swipe(dir_h, dir_v);
    }, nullptr);

    // ── Timer: 30 Hz update ───────────────────────────────────────────────────
    d->timer = lv_timer_create(spectrum_timer_cb, (uint32_t)TIMER_PERIOD_MS, d);

    s_data = d;
    ESP_LOGI(TAG, "spectrum screen created");
    return scr;
}
