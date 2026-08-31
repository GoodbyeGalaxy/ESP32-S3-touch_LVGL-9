// meter_phase.cpp — Phase Correlation Meter.
// Large horizontal bar showing correlation = gonio_l * gonio_r (averaged).
// Below it: a 60-second rolling history graph.
// Labels: -1 "OUT OF PHASE" | 0 "MONO" | +1 "IN PHASE".
// Canvas 800×480, PSRAM, ARGB8888. Updates at 20 Hz.

#include <initializer_list>
#include "meter_phase.h"
#include "metering_hub.h"
#include "audio_data.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "screens/settings_overlay.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>
#include <cmath>

static const char *TAG __attribute__((unused)) = "meter_phase";

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int CW = 800;
static constexpr int CH = 480;

// History ring: 60 seconds at 20 Hz = 1200 samples
static constexpr int HIST_SIZE = 1200;

// Layout regions (all in canvas coords):
// Statusbar: Y=0..31
// BAR region: Y=50..130 (large correlation bar + decorations)
// NUMERIC region: Y=140..190 (big numeric readout)
// HISTORY region: Y=200..400 (history graph)
// Foot: Y=424..479
static constexpr int BAR_Y       = 60;
static constexpr int BAR_H       = 60;
static constexpr int BAR_MARGIN  = 40;  // left/right margin for bar
static constexpr int BAR_X       = BAR_MARGIN;
static constexpr int BAR_W       = CW - 2 * BAR_MARGIN;

static constexpr int NUM_Y       = 138;
static constexpr int NUM_H       = 40;

static constexpr int HIST_X      = 20;
static constexpr int HIST_Y      = 200;
static constexpr int HIST_W      = CW - 40;
static constexpr int HIST_H      = 190;

// ── State ─────────────────────────────────────────────────────────────────────

struct PhaseState {
    lv_timer_t   *timer;
    lv_obj_t     *canvas;
    lv_obj_t     *num_label;   // big numeric label (LVGL object over canvas)
    lv_color32_t *buf;

    float         corr_smooth;  // smoothed correlation value
    float         history[HIST_SIZE];
    int           hist_head;
    uint32_t      last_seq;
    int hist_sel = 1;  // 0=30 s, 1=60 s, 2=120 s
};

// ── Pixel helpers ─────────────────────────────────────────────────────────────

static inline void put_px(lv_color32_t *buf, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    lv_color32_t *p = buf + y * CW + x;
    p->red = r; p->green = g; p->blue = b; p->alpha = 0xFF;
}

static void fill_rect(lv_color32_t *buf, int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b)
{
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            put_px(buf, col, row, r, g, b);
}

// ── Correlation color: green at +1, red at -1, yellow at 0 ───────────────────

static void corr_color(float c, uint8_t &r, uint8_t &g, uint8_t &b)
{
    // c in [-1, +1]
    if (c >= 0.0f) {
        // 0 → yellow (FF,CC,00), +1 → green (00,FF,44)
        float t = c;
        r = (uint8_t)(0xFF * (1.0f - t));
        g = (uint8_t)(0xCC + (0xFF - 0xCC) * t);
        b = (uint8_t)(0x00 + 0x44 * t);
    } else {
        // -1 → red (FF,00,00), 0 → yellow (FF,CC,00)
        float t = c + 1.0f;  // 0→red, 1→yellow
        r = 0xFF;
        g = (uint8_t)(0xCC * t);
        b = 0x00;
    }
}

// ── Render ────────────────────────────────────────────────────────────────────

static void render_phase(PhaseState *st)
{
    lv_color32_t *buf = st->buf;

    // Clear to background
    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) buf[i] = bg;

    float corr = st->corr_smooth;
    if (corr >  1.0f) corr =  1.0f;
    if (corr < -1.0f) corr = -1.0f;

    // ── Track background (dim) ────────────────────────────────────────────────
    fill_rect(buf, BAR_X, BAR_Y, BAR_W, BAR_H, 0x18, 0x18, 0x18);

    // Tick marks at -1, -0.5, 0, +0.5, +1
    auto tick_x = [&](float v) -> int {
        return BAR_X + (int)((v + 1.0f) * 0.5f * (float)(BAR_W - 1));
    };
    for (float v : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f }) {
        int tx = tick_x(v);
        for (int y = BAR_Y - 6; y < BAR_Y; y++) put_px(buf, tx, y, 0x50, 0x50, 0x50);
        for (int y = BAR_Y + BAR_H; y < BAR_Y + BAR_H + 6; y++) put_px(buf, tx, y, 0x50, 0x50, 0x50);
    }

    // ── Filled correlation bar ────────────────────────────────────────────────
    // Fill from center (x at 0) toward direction
    int center_x = tick_x(0.0f);
    int fill_x   = tick_x(corr);

    int bx = (corr >= 0.0f) ? center_x : fill_x;
    int bw = abs(fill_x - center_x);
    if (bw < 2) bw = 2;

    // Draw with gradient: iterate column by column
    for (int col = bx; col < bx + bw; col++) {
        float v = (float)(col - BAR_X) / (float)(BAR_W - 1) * 2.0f - 1.0f;
        uint8_t cr, cg, cb;
        corr_color(v, cr, cg, cb);
        // Inner bar (inset 4px top/bottom for 3D look)
        for (int row = BAR_Y + 4; row < BAR_Y + BAR_H - 4; row++)
            put_px(buf, col, row, cr, cg, cb);
        // Top/bottom edges (brighter)
        for (int row = BAR_Y; row < BAR_Y + 4; row++)
            put_px(buf, col, row, (uint8_t)((int)cr * 140/100), (uint8_t)((int)cg * 140/100), (uint8_t)((int)cb * 140/100));
        for (int row = BAR_Y + BAR_H - 4; row < BAR_Y + BAR_H; row++)
            put_px(buf, col, row, (uint8_t)((int)cr * 70/100), (uint8_t)((int)cg * 70/100), (uint8_t)((int)cb * 70/100));
    }

    // Center reference line
    for (int y = BAR_Y; y < BAR_Y + BAR_H; y++) put_px(buf, center_x, y, 0xFF, 0xFF, 0xFF);

    // Needle (current position indicator) — vertical white line
    if (fill_x >= BAR_X && fill_x < BAR_X + BAR_W) {
        for (int y = BAR_Y - 8; y < BAR_Y + BAR_H + 8; y++)
            put_px(buf, fill_x, y, 0xFF, 0xFF, 0xFF);
    }

    // ── History graph ─────────────────────────────────────────────────────────
    // Frame
    fill_rect(buf, HIST_X, HIST_Y, HIST_W, HIST_H, 0x12, 0x12, 0x12);

    // Center line at corr=0
    int hist_cy = HIST_Y + HIST_H / 2;
    for (int x = HIST_X; x < HIST_X + HIST_W; x++) put_px(buf, x, hist_cy, 0x28, 0x28, 0x28);

    // Draw history — oldest left, newest right
    int prev_hy = -1;
    for (int px = 0; px < HIST_W; px++) {
        int sample_idx = (st->hist_head - HIST_W + px + HIST_SIZE) % HIST_SIZE;
        float v = st->history[sample_idx];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;

        // Map: +1 → HIST_Y, -1 → HIST_Y + HIST_H - 1
        int hy = HIST_Y + (int)((1.0f - v) * 0.5f * (float)(HIST_H - 1));
        int hx = HIST_X + px;

        uint8_t hr, hg, hb;
        corr_color(v, hr, hg, hb);
        // Dim the history a bit
        hr = (uint8_t)((int)hr * 180 / 255);
        hg = (uint8_t)((int)hg * 180 / 255);
        hb = (uint8_t)((int)hb * 180 / 255);

        if (prev_hy < 0) {
            put_px(buf, hx, hy, hr, hg, hb);
        } else {
            // Vertical fill from prev_hy to hy for continuity
            int y0 = prev_hy < hy ? prev_hy : hy;
            int y1 = prev_hy < hy ? hy : prev_hy;
            for (int y = y0; y <= y1; y++) put_px(buf, hx, y, hr, hg, hb);
        }
        prev_hy = hy;
    }

    lv_obj_invalidate(st->canvas);
}

// ── Timer callback ────────────────────────────────────────────────────────────

static void phase_timer_cb(lv_timer_t *t)
{
    PhaseState *st = static_cast<PhaseState *>(lv_timer_get_user_data(t));
    if (!st) return;

    AudioPacket pkt{};
    bool got_pkt = (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE);

    if (got_pkt && pkt.seq != st->last_seq) {
        st->last_seq = pkt.seq;

        // Instantaneous correlation: product of L and R samples
        float inst = pkt.gonio_l * pkt.gonio_r;
        // Clamp to [-1, +1]
        if (inst >  1.0f) inst =  1.0f;
        if (inst < -1.0f) inst = -1.0f;

        // Smooth (slower for stability)
        st->corr_smooth += (inst - st->corr_smooth) * 0.15f;

        // Push to history
        st->history[st->hist_head] = st->corr_smooth;
        st->hist_head = (st->hist_head + 1) % HIST_SIZE;

        // Update numeric label
        char buf[16];
        float display_val = st->corr_smooth;
        if (display_val >  1.0f) display_val =  1.0f;
        if (display_val < -1.0f) display_val = -1.0f;
        snprintf(buf, sizeof(buf), "%+.2f", display_val);
        lv_label_set_text(st->num_label, buf);
    }

    render_phase(st);
}

// ── Screen lifecycle ──────────────────────────────────────────────────────────

static void on_phase_delete(lv_event_t *e)
{
    PhaseState *st = static_cast<PhaseState *>(lv_event_get_user_data(e));
    if (!st) return;
    if (st->timer) lv_timer_delete(st->timer);
    if (st->buf)   heap_caps_free(st->buf);
    delete st;
}

lv_obj_t *meter_phase_screen_create()
{
    PhaseState *st = new PhaseState{};
    memset(st->history, 0, sizeof(st->history));
    st->corr_smooth = 0.0f;
    st->hist_head   = 0;
    st->last_seq    = 0xFFFFFFFF;

    st->buf = static_cast<lv_color32_t *>(
        heap_caps_malloc(CW * CH * sizeof(lv_color32_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!st->buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        delete st;
        return theme_make_screen();
    }
    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) st->buf[i] = bg;

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_phase_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("PHASE CORR");

    // Canvas
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, CW, CH);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(canvas, st->buf, CW, CH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_invalidate(canvas);
    st->canvas = canvas;

    // Numeric label (LVGL, on top of canvas) — large correlation value
    lv_obj_t *num = lv_label_create(scr);
    lv_obj_remove_style_all(num);
    lv_label_set_text(num, "+0.00");
    lv_obj_set_style_text_color(num, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(num, THEME_FONT_TITLE, 0);
    lv_obj_set_pos(num, 0, NUM_Y);
    lv_obj_set_size(num, CW, NUM_H);
    lv_label_set_long_mode(num, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
    st->num_label = num;

    // Static text labels for the bar scale
    auto make_scale_lbl = [&](const char *text, int x, int y) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_remove_style_all(l);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(l, THEME_FONT_HINT, 0);
        lv_obj_set_pos(l, x, y);
    };

    constexpr int LBL_Y = BAR_Y + BAR_H + 10;
    make_scale_lbl("-1", BAR_X - 6, LBL_Y);
    make_scale_lbl("OUT OF PHASE", BAR_X + 20, LBL_Y);
    make_scale_lbl("MONO", CW / 2 - 18, LBL_Y);
    make_scale_lbl("IN PHASE", BAR_X + BAR_W - 100, LBL_Y);
    make_scale_lbl("+1", BAR_X + BAR_W - 12, LBL_Y);

    // History label
    lv_obj_t *hist_lbl = lv_label_create(scr);
    lv_obj_remove_style_all(hist_lbl);
    lv_label_set_text(hist_lbl, "60s HISTORY");
    lv_obj_set_style_text_color(hist_lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(hist_lbl, THEME_FONT_HINT, 0);
    lv_obj_set_pos(hist_lbl, HIST_X, HIST_Y - 18);

    // Foot bar + Settings
    lv_obj_t *phase_rz = foot_create_hub_back(scr);

    static const SettingOption hist_opts[] = { {"30 s"}, {"60 s"}, {"120 s"} };
    auto *phase_items = new SettingItem[1];
    phase_items[0] = { "History", hist_opts, 3, &st->hist_sel };
    settings_btn_create(phase_rz, scr, phase_items, 1);

    // 2D swipe
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
    }, nullptr);

    st->timer = lv_timer_create(phase_timer_cb, 50, st);

    return scr;
}
