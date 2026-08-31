// meter_ms.cpp — M/S Meter (Mid/Side).
// Mid  = (L + R) / 2  — checks mono compatibility
// Side = (L - R) / 2  — stereo width
// Two VU-style vertical bars with ballistics, numeric dBFS readout below each.
// Canvas 800×480, PSRAM, ARGB8888. Updates at 20 Hz.

#include "meter_ms.h"
#include "metering_hub.h"
#include "audio_data.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <cstring>
#include <cmath>
#include <algorithm>

static const char *TAG __attribute__((unused)) = "meter_ms";

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int CW = 800;
static constexpr int CH = 480;

// dBFS range displayed
static constexpr float DB_MIN = -60.0f;
static constexpr float DB_MAX =   0.0f;
static constexpr float DB_RANGE = DB_MAX - DB_MIN;

// Bar layout — two wide bars centered horizontally
// Content area: Y=32..424 (H=392)
static constexpr int BAR_W       = 280;
static constexpr int BAR_H       = 280;
static constexpr int BAR_GAP     = 40;
static constexpr int BAR_Y       = THEME_CONTENT_Y + 30;
static constexpr int BAR_M_X     = (CW - BAR_W * 2 - BAR_GAP) / 2;       // Mid bar X
static constexpr int BAR_S_X     = BAR_M_X + BAR_W + BAR_GAP;             // Side bar X

// Peak hold time in timer ticks (20Hz → 2s = 40 ticks)
static constexpr int PEAK_HOLD_TICKS = 40;

// ── State ─────────────────────────────────────────────────────────────────────

struct MsState {
    lv_timer_t   *timer;
    lv_obj_t     *canvas;
    lv_obj_t     *lbl_m_num;   // Mid numeric dBFS label
    lv_obj_t     *lbl_s_num;   // Side numeric dBFS label
    lv_color32_t *buf;

    float         mid_rms;      // dBFS, smoothed
    float         side_rms;     // dBFS, smoothed
    float         mid_peak;     // dBFS, peak hold
    float         side_peak;
    int           mid_peak_hold;
    int           side_peak_hold;

    uint32_t      last_seq;
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

// ── Bar color: green at low, yellow at -6dB, red at 0dB ──────────────────────

static void bar_color(float db_frac, uint8_t &r, uint8_t &g, uint8_t &b)
{
    // db_frac: 0=bottom(-60dB), 1=top(0dB)
    if (db_frac < 0.8f) {
        // Green (0x00DD44) → Yellow (0xFFCC00)
        float t = db_frac / 0.8f;
        r = (uint8_t)(0xFF * t);
        g = (uint8_t)(0xDD + (0xCC - 0xDD) * t);
        b = (uint8_t)(0x44 * (1.0f - t));
    } else {
        // Yellow → Red
        float t = (db_frac - 0.8f) / 0.2f;
        r = 0xFF;
        g = (uint8_t)(0xCC * (1.0f - t));
        b = 0x00;
    }
}

// ── Draw a single bar ─────────────────────────────────────────────────────────

static void draw_bar(lv_color32_t *buf, int bar_x, float db, float peak_db)
{
    // Background
    fill_rect(buf, bar_x, BAR_Y, BAR_W, BAR_H, 0x14, 0x14, 0x14);

    // dBFS → fraction (0=bottom, 1=top)
    float frac = (db - DB_MIN) / DB_RANGE;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;

    int fill_h = (int)(frac * (float)(BAR_H - 2));

    // Draw filled bar (bottom up), column-wise with color gradient per row
    for (int row = 0; row < fill_h; row++) {
        // row 0 = bottom, row fill_h-1 = near top
        float row_frac = (float)row / (float)(BAR_H - 2);
        uint8_t cr, cg, cb;
        bar_color(row_frac, cr, cg, cb);
        // Dim top/bottom edges for 3D
        if (row == 0 || row == fill_h - 1) {
            cr = (uint8_t)((int)cr * 60 / 100);
            cg = (uint8_t)((int)cg * 60 / 100);
            cb = (uint8_t)((int)cb * 60 / 100);
        }
        int canvas_y = BAR_Y + BAR_H - 1 - row;
        for (int col = bar_x + 2; col < bar_x + BAR_W - 2; col++)
            put_px(buf, col, canvas_y, cr, cg, cb);
    }

    // LED segment lines (every 8 pixels)
    for (int seg = 0; seg < BAR_H; seg += 8) {
        int seg_y = BAR_Y + seg;
        float seg_frac = (float)(BAR_H - seg) / (float)(BAR_H - 2);
        bool lit = (seg_frac <= frac);
        if (!lit) {
            for (int col = bar_x; col < bar_x + BAR_W; col++)
                put_px(buf, col, seg_y, 0x0A, 0x0A, 0x0A);
        }
    }

    // Tick marks at -60, -48, -36, -24, -18, -12, -6, -3, 0 dBFS
    for (float mark : { -60.0f, -48.0f, -36.0f, -24.0f, -18.0f, -12.0f, -6.0f, -3.0f, 0.0f }) {
        float mark_frac = (mark - DB_MIN) / DB_RANGE;
        int tick_y = BAR_Y + BAR_H - 1 - (int)(mark_frac * (float)(BAR_H - 2));
        for (int col = bar_x + BAR_W; col < bar_x + BAR_W + 8; col++)
            put_px(buf, col, tick_y, 0x44, 0x44, 0x44);
    }

    // Peak hold indicator (white horizontal line)
    float pk_frac = (peak_db - DB_MIN) / DB_RANGE;
    if (pk_frac > 0.0f && pk_frac <= 1.0f) {
        int peak_y = BAR_Y + BAR_H - 1 - (int)(pk_frac * (float)(BAR_H - 2));
        for (int col = bar_x + 1; col < bar_x + BAR_W - 1; col++)
            put_px(buf, col, peak_y, 0xFF, 0xFF, 0xFF);
    }

    // 0 dBFS danger zone — top 4 pixels always red-tinted
    for (int row = BAR_Y; row < BAR_Y + 4; row++)
        for (int col = bar_x + 1; col < bar_x + BAR_W - 1; col++)
            put_px(buf, col, row, 0x44, 0x00, 0x00);

    // Frame
    for (int col = bar_x; col < bar_x + BAR_W; col++) {
        put_px(buf, col, BAR_Y, 0x40, 0x40, 0x40);
        put_px(buf, col, BAR_Y + BAR_H - 1, 0x40, 0x40, 0x40);
    }
    for (int row = BAR_Y; row < BAR_Y + BAR_H; row++) {
        put_px(buf, bar_x, row, 0x40, 0x40, 0x40);
        put_px(buf, bar_x + BAR_W - 1, row, 0x40, 0x40, 0x40);
    }
}

// ── Render ────────────────────────────────────────────────────────────────────

static void render_ms(MsState *st)
{
    lv_color32_t *buf = st->buf;

    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) buf[i] = bg;

    draw_bar(buf, BAR_M_X, st->mid_rms,  st->mid_peak);
    draw_bar(buf, BAR_S_X, st->side_rms, st->side_peak);

    lv_obj_invalidate(st->canvas);
}

// ── Timer callback ────────────────────────────────────────────────────────────

static void ms_timer_cb(lv_timer_t *t)
{
    MsState *st = static_cast<MsState *>(lv_timer_get_user_data(t));
    if (!st) return;

    AudioPacket pkt{};
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE && pkt.seq != st->last_seq) {
        st->last_seq = pkt.seq;

        // True M/S RMS from sample domain — phase-correct, sent by sender.
        float target_m = pkt.rms_mono;
        float target_s = pkt.rms_side;

        if (target_m < DB_MIN) target_m = DB_MIN;
        if (target_s < DB_MIN) target_s = DB_MIN;

        // Ballistics: fast attack, slow decay
        float alpha_attack = 0.4f;
        float alpha_decay  = 0.05f;

        auto apply_ballistics = [&](float &current, float target) {
            float alpha = (target > current) ? alpha_attack : alpha_decay;
            current += (target - current) * alpha;
        };

        apply_ballistics(st->mid_rms,  target_m);
        apply_ballistics(st->side_rms, target_s);

        // Peak hold
        if (target_m >= st->mid_peak) {
            st->mid_peak = target_m;
            st->mid_peak_hold = PEAK_HOLD_TICKS;
        } else {
            if (st->mid_peak_hold > 0) st->mid_peak_hold--;
            else st->mid_peak -= 0.5f;  // fall slowly
            if (st->mid_peak < DB_MIN) st->mid_peak = DB_MIN;
        }

        if (target_s >= st->side_peak) {
            st->side_peak = target_s;
            st->side_peak_hold = PEAK_HOLD_TICKS;
        } else {
            if (st->side_peak_hold > 0) st->side_peak_hold--;
            else st->side_peak -= 0.5f;
            if (st->side_peak < DB_MIN) st->side_peak = DB_MIN;
        }

        // Update numeric labels
        char buf_m[16], buf_s[16];
        float disp_m = st->mid_rms;
        float disp_s = st->side_rms;
        if (disp_m <= DB_MIN) snprintf(buf_m, sizeof(buf_m), "-inf");
        else                  snprintf(buf_m, sizeof(buf_m), "%.1f", disp_m);
        if (disp_s <= DB_MIN) snprintf(buf_s, sizeof(buf_s), "-inf");
        else                  snprintf(buf_s, sizeof(buf_s), "%.1f", disp_s);
        lv_label_set_text(st->lbl_m_num, buf_m);
        lv_label_set_text(st->lbl_s_num, buf_s);
    }

    // Always tick peak hold decay timer
    render_ms(st);
}

// ── Screen lifecycle ──────────────────────────────────────────────────────────

static void on_ms_delete(lv_event_t *e)
{
    MsState *st = static_cast<MsState *>(lv_event_get_user_data(e));
    if (!st) return;
    if (st->timer) lv_timer_delete(st->timer);
    if (st->buf)   heap_caps_free(st->buf);
    delete st;
}

lv_obj_t *meter_ms_screen_create()
{
    MsState *st = new MsState{};
    st->mid_rms  = DB_MIN;
    st->side_rms = DB_MIN;
    st->mid_peak  = DB_MIN;
    st->side_peak = DB_MIN;
    st->mid_peak_hold  = 0;
    st->side_peak_hold = 0;
    st->last_seq = 0xFFFFFFFF;

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
    lv_obj_add_event_cb(scr, on_ms_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("M/S METER");

    // Canvas
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, CW, CH);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(canvas, st->buf, CW, CH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_invalidate(canvas);
    st->canvas = canvas;

    // Bar title labels
    constexpr int LBL_TITLE_Y = BAR_Y - 22;
    constexpr int NUM_Y       = BAR_Y + BAR_H + 8;

    auto make_title = [&](const char *text, int x, int w) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_remove_style_all(l);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(l, THEME_FONT_LABEL, 0);
        lv_obj_set_pos(l, x, LBL_TITLE_Y);
        lv_obj_set_size(l, w, 22);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    };
    make_title("MID", BAR_M_X, BAR_W);
    make_title("SIDE", BAR_S_X, BAR_W);

    // Numeric readout labels
    auto make_num_lbl = [&](lv_obj_t **out, const char *init, int x) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_remove_style_all(l);
        lv_label_set_text(l, init);
        lv_obj_set_style_text_color(l, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(l, THEME_FONT_LABEL, 0);
        lv_obj_set_pos(l, x, NUM_Y);
        lv_obj_set_size(l, BAR_W, 24);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        *out = l;
    };
    make_num_lbl(&st->lbl_m_num, "-inf", BAR_M_X);
    make_num_lbl(&st->lbl_s_num, "-inf", BAR_S_X);

    // dBFS unit label
    auto make_unit = [&](int x) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_remove_style_all(l);
        lv_label_set_text(l, "dBFS");
        lv_obj_set_style_text_color(l, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(l, THEME_FONT_LABEL, 0);
        lv_obj_set_pos(l, x, NUM_Y + 24);
        lv_obj_set_size(l, BAR_W, 22);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    };
    make_unit(BAR_M_X);
    make_unit(BAR_S_X);

    // Foot bar
    foot_create_hub_back(scr);

    // 2D swipe
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
    }, nullptr);

    st->timer = lv_timer_create(ms_timer_cb, 50, st);

    return scr;
}
