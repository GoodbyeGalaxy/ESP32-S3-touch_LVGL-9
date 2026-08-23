#include "skin_digital.h"
#include "theme.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <cmath>
#include <cstring>
#include <algorithm>

// ── SkinDigital::create ───────────────────────────────────────────────────────

void SkinDigital::create(lv_obj_t *parent)
{
    bar_l_data_ = {-60.0f, -60.0f};
    bar_r_data_ = {-60.0f, -60.0f};

    create_bars(parent);
    create_gonio(parent);
    create_history(parent);
    create_numerics(parent);
    create_scale(parent);
    create_spec_strip(parent);
}

// ── SkinDigital::destroy ──────────────────────────────────────────────────────

void SkinDigital::destroy()
{
    if (gonio_buf_) {
        heap_caps_free(gonio_buf_);
        gonio_buf_ = nullptr;
    }
}

// ── SkinDigital::update ───────────────────────────────────────────────────────

void SkinDigital::update(const MeterReadings &r)
{
    // Bars
    bar_l_data_.rms_db       = r.rms_l;
    bar_l_data_.peak_hold_db = r.peak_hold_l;
    lv_obj_invalidate(bar_l_);

    bar_r_data_.rms_db       = r.rms_r;
    bar_r_data_.peak_hold_db = r.peak_hold_r;
    lv_obj_invalidate(bar_r_);

    // Goniometer
    update_gonio(r);

    // History — invalidate so draw_cb re-reads cached snapshot
    // Cache current history snapshot for draw_cb (which cannot take MeterReadings directly)
    memcpy(hist_snap_, r.short_term_history, sizeof(hist_snap_));
    hist_head_snap_ = r.history_head;
    lv_obj_invalidate(history_);

    // Numerics
    lv_label_set_text_fmt(num_i_,    "I:    %+.1f LKFS", r.lufs_i);
    lv_label_set_text_fmt(num_s_,    "S:    %+.1f LKFS", r.lufs_s);
    lv_label_set_text_fmt(num_m_,    "M:    %+.1f LKFS", r.lufs_m);

    float peak = std::max(r.peak_hold_l, r.peak_hold_r);
    lv_label_set_text_fmt(num_peak_, "Peak: %+.1f dBFS", peak);

    lv_color_t peak_color = (peak > -3.0f) ? lv_color_hex(0xE05050) : THEME_TEXT_PRIMARY;
    lv_obj_set_style_text_color(num_peak_, peak_color, 0);

    // Spectral strip — copy band magnitudes then invalidate
    memcpy(spec_bands_, r.bands, sizeof(spec_bands_));
    lv_obj_invalidate(spec_strip_);
}

// ── Bar widgets ───────────────────────────────────────────────────────────────

void SkinDigital::bar_draw_cb(lv_event_t *e)
{
    auto *d        = static_cast<BarData*>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t   *obj   = lv_event_get_target_obj(e);

    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t h = lv_area_get_height(&a);

    // Background
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(0x686868);  // ≥38% luminance — IPS panel minimum
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

    // -18 dBFS reference line (yellow dashed)
    {
        int32_t y18 = db_to_y(-18.0f);
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color     = lv_color_hex(0xD4B000);
        dsc.width     = 2;
        dsc.dash_width = 6;
        dsc.dash_gap   = 4;
        dsc.p1.x = (lv_value_precise_t)(a.x1 + 6);
        dsc.p1.y = (lv_value_precise_t)y18;
        dsc.p2.x = (lv_value_precise_t)(a.x2 - 6);
        dsc.p2.y = (lv_value_precise_t)y18;
        lv_draw_line(layer, &dsc);
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

void SkinDigital::create_bars(lv_obj_t *parent)
{
    auto make_bar = [&](BarData *d) -> lv_obj_t* {
        lv_obj_t *c = lv_obj_create(parent);
        lv_obj_remove_style_all(c);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(c, bar_draw_cb, LV_EVENT_DRAW_MAIN, d);
        return c;
    };

    bar_l_ = make_bar(&bar_l_data_);
    lv_obj_set_size(bar_l_, 90, 380);
    lv_obj_set_pos(bar_l_, 16, 40);

    bar_r_ = make_bar(&bar_r_data_);
    lv_obj_set_size(bar_r_, 90, 380);
    lv_obj_set_pos(bar_r_, 116, 40);
}

// ── Goniometer ────────────────────────────────────────────────────────────────

// static — redraws all ring-buffer points onto canvas using lv_canvas layer API
void SkinDigital::gonio_redraw(lv_obj_t *canvas, GonioPoint *pts, int head, void *buf)
{
    (void)buf;  // buf already set on canvas via lv_canvas_set_buffer in create_gonio

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
        int idx = (head + i) % 80;
        auto &pt = pts[idx];
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

void SkinDigital::create_gonio(lv_obj_t *parent)
{
    constexpr int W = 250, H = 250;
    size_t buf_size = (size_t)W * H * sizeof(uint16_t);  // RGB565 = 2 bytes/pixel
    gonio_buf_ = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!gonio_buf_) {
        gonio_buf_ = malloc(buf_size);
    }
    memset(gonio_buf_, 0, buf_size);

    // Initialize ring buffer to center (no signal = dot at center)
    for (auto &p : gonio_pts_) { p.x = 125; p.y = 125; }
    gonio_head_ = 0;

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, gonio_buf_, W, H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas, 278, 40);
    gonio_ = canvas;
}

void SkinDigital::update_gonio(const MeterReadings &r)
{
    float l = r.gonio_l;
    float rr = r.gonio_r;

    // Mid-Side (Lissajous rotated 45°): side=horizontal, mid=vertical
    constexpr float SQRT2_INV = 0.7071f;
    float mid  = (l + rr) * SQRT2_INV;
    float side = (l - rr) * SQRT2_INV;

    // Map ±1.0 → ±110 px from center (125, 125)
    int16_t cx = (int16_t)(125.0f + side * 110.0f);
    int16_t cy = (int16_t)(125.0f - mid  * 110.0f);
    // Clamp to canvas bounds (leaving 15px margin for point radius)
    cx = (int16_t)(cx < 15 ? 15 : cx > 235 ? 235 : cx);
    cy = (int16_t)(cy < 15 ? 15 : cy > 235 ? 235 : cy);

    // Add to ring buffer; head will point to NEXT write (= oldest after increment)
    gonio_pts_[gonio_head_] = {cx, cy};
    gonio_head_ = (gonio_head_ + 1) % 80;

    gonio_redraw(gonio_, gonio_pts_, gonio_head_, gonio_buf_);
}

// ── History graph ─────────────────────────────────────────────────────────────

void SkinDigital::history_draw_cb(lv_event_t *e)
{
    auto *skin  = static_cast<SkinDigital*>(lv_event_get_user_data(e));
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
        dsc.bg_color = lv_color_hex(0x606060);  // ≥38% luminance — IPS panel minimum
        dsc.radius   = THEME_RADIUS;
        lv_draw_rect(layer, &dsc, &a);
    }

    // 60 bars: left = oldest, right = newest
    constexpr float DB_MIN   = -40.0f;
    constexpr float DB_MAX   =  -6.0f;
    constexpr float DB_RANGE = DB_MAX - DB_MIN;

    float bar_w = (float)w / 60.0f;

    for (int i = 0; i < 60; i++) {
        int   idx = (skin->hist_head_snap_ + i) % 60;  // oldest→newest
        float val = skin->hist_snap_[idx];

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

void SkinDigital::create_history(lv_obj_t *parent)
{
    // Pre-fill snapshot with silence
    for (auto &v : hist_snap_) v = -40.0f;
    hist_head_snap_ = 0;

    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(c, history_draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_set_size(c, 358, 122);
    lv_obj_set_pos(c, 224, 298);
    history_ = c;
}

// ── Numerics ──────────────────────────────────────────────────────────────────

void SkinDigital::create_numerics(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(panel, 194, 110);
    lv_obj_set_pos(panel, 590, 48);

    auto make_label = [&](int y_offset) -> lv_obj_t* {
        lv_obj_t *lbl = lv_label_create(panel);
        lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_set_pos(lbl, 0, y_offset);
        return lbl;
    };

    num_i_    = make_label(0);
    num_s_    = make_label(24);
    num_m_    = make_label(48);
    num_peak_ = make_label(72);

    lv_label_set_text(num_i_,    "I:    --- LKFS");
    lv_label_set_text(num_s_,    "S:    --- LKFS");
    lv_label_set_text(num_m_,    "M:    --- LKFS");
    lv_label_set_text(num_peak_, "Peak: --- dBFS");
}

// ── dB Scale column ───────────────────────────────────────────────────────────

void SkinDigital::create_scale(lv_obj_t *parent)
{
    // Thin column between the bars and the goniometer (x=212, y=40, w=60, h=380)
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(col, 60, 380);
    lv_obj_set_pos(col, 212, 40);
    scale_col_ = col;

    // dBFS tick values to label (0 dBFS = top at y=0, -60 = bottom at y=380)
    static const int ticks[] = { 0, -6, -12, -18, -24, -36, -48, -60 };
    for (int t : ticks) {
        float norm = (t + 60.0f) / 60.0f;  // 0..1
        int32_t y  = (int32_t)((1.0f - norm) * 380.0f) - 8;  // -8 to center label
        if (y < 0) y = 0;

        lv_obj_t *lbl = lv_label_create(col);
        lv_obj_set_style_text_color(lbl, THEME_TEXT_SECONDARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_set_pos(lbl, 0, y);
        lv_label_set_text_fmt(lbl, "%d", t);
    }
}

// ── Spectral strip ────────────────────────────────────────────────────────────

void SkinDigital::spec_strip_draw_cb(lv_event_t *e)
{
    auto *skin = static_cast<SkinDigital*>(lv_event_get_user_data(e));
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t   *obj   = lv_event_get_target_obj(e);

    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a);
    int32_t h = lv_area_get_height(&a);

    // Dark background
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(0x505050);
        dsc.radius   = THEME_RADIUS;
        lv_draw_rect(layer, &dsc, &a);
    }

    // Band colors: Sub, Bass, LowMid, Mid, HighMid, Air
    static const uint32_t band_colors[6] = {
        0x8060E0u,  // Sub    — violet
        0x3080E0u,  // Bass   — blue
        0x30A850u,  // LowMid — green
        0xC8B030u,  // Mid    — yellow
        0xE07830u,  // HiMid  — orange
        0xE04848u,  // Air    — red
    };

    int32_t bar_w = w / 6;
    for (int b = 0; b < 6; b++) {
        float mag = skin->spec_bands_[b];
        mag = std::max(0.0f, std::min(1.0f, mag));
        int32_t bar_h = (int32_t)(mag * (float)(h - 4));
        if (bar_h < 1) bar_h = 1;

        int32_t x0 = a.x1 + b * bar_w + 2;
        int32_t x1 = a.x1 + (b + 1) * bar_w - 2;

        lv_area_t ba = { x0, a.y2 - bar_h - 2, x1, a.y2 - 2 };
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(band_colors[b]);
        dsc.radius   = 2;
        lv_draw_rect(layer, &dsc, &ba);
    }
}

void SkinDigital::create_spec_strip(lv_obj_t *parent)
{
    lv_obj_t *strip = lv_obj_create(parent);
    lv_obj_remove_style_all(strip);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(strip, spec_strip_draw_cb, LV_EVENT_DRAW_MAIN, this);
    lv_obj_set_size(strip, 194, 90);
    lv_obj_set_pos(strip, 590, 190);
    spec_strip_ = strip;
}
