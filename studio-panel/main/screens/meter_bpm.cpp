// meter_bpm.cpp — BPM Detector (Option B: on-device spectral onset autocorrelation)
//
// Onset function: half-wave-rectified spectral flux from low-mid FFT bins (0–63)
// sampled at 30 fps (UDP packet rate). Ring buffer holds ~8.5 s of onset data.
// Autocorrelation across lag range 9–60 frames → 200–30 BPM, recomputed every
// 15 new packets (~0.5 s). BPM is smoothed with a slow EMA to reduce jitter.
//
// Option A fallback stub: when AudioPacket gains a host-computed bpm field,
// check it first and skip recompute_bpm() when the host provides a valid value.

#include "meter_bpm.h"
#include "audio_data.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <cmath>
#include <cstring>
#include <algorithm>

// ── Constants ──────────────────────────────────────────────────────────────────

static constexpr int   ONSET_CAP   = 256;   // ring buffer: ~8.5 s at 30 fps
static constexpr int   FLUX_BINS   = 64;    // low-mid bins used for onset
static constexpr int   LAG_MIN     = 9;     // 60*30/9  ≈ 200 BPM
static constexpr int   LAG_MAX     = 60;    // 60*30/60 =  30 BPM
static constexpr int   RECOMPUTE_N = 15;    // rerun autocorr every 15 packets
static constexpr float FPS         = 30.0f;

// Waveform canvas strip (onset history bar chart)
static constexpr int WF_W = 800;
static constexpr int WF_H = 190;
static constexpr int WF_Y = 210;

// ── State ──────────────────────────────────────────────────────────────────────

struct BpmState {
    float    onset_buf[ONSET_CAP] = {};
    float    prev_bins[FLUX_BINS] = {};
    int      head       = 0;
    int      count      = 0;
    int      since_recp = 0;
    float    bpm        = 0.0f;     // 0 = not enough data yet
    float    confidence = 0.0f;
    uint32_t last_seq   = 0xFFFFFFFF;

    lv_color32_t *wf_buf  = nullptr;  // PSRAM canvas buffer
    lv_obj_t     *canvas  = nullptr;
    lv_obj_t     *lbl_bpm = nullptr;
    lv_obj_t     *lbl_sub = nullptr;  // "BPM" unit + confidence + method
    lv_timer_t   *timer   = nullptr;
};

// ── Waveform canvas ────────────────────────────────────────────────────────────

static inline void wf_put(lv_color32_t *buf, int x, int y,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x >= WF_W || (unsigned)y >= WF_H) return;
    lv_color32_t *p = buf + y * WF_W + x;
    p->red = r; p->green = g; p->blue = b; p->alpha = 0xFF;
}

static void render_waveform(BpmState *st)
{
    // Background
    lv_color32_t bg = { .blue=0x14, .green=0x14, .red=0x14, .alpha=0xFF };
    for (int i = 0; i < WF_W * WF_H; i++) st->wf_buf[i] = bg;

    // Onset bars: right = newest, left = oldest
    int n = std::min(st->count, WF_W);
    float peak = 0.0f;
    for (int k = 0; k < n; k++) {
        float v = st->onset_buf[(st->head - 1 - k + ONSET_CAP) % ONSET_CAP];
        if (v > peak) peak = v;
    }
    if (peak < 1e-6f) { lv_obj_invalidate(st->canvas); return; }

    for (int col = 0; col < n; col++) {
        int   frame = (st->head - 1 - (n - 1 - col) + ONSET_CAP) % ONSET_CAP;
        float v     = st->onset_buf[frame] / peak;
        int   bh    = std::max(1, (int)(v * (float)(WF_H - 2)));
        int   x     = WF_W - n + col;

        // Bar
        for (int row = WF_H - 1 - bh; row < WF_H - 1; row++)
            wf_put(st->wf_buf, x, row, 0x3B, 0x82, 0xF6);
        // Bright cap
        wf_put(st->wf_buf, x, WF_H - 1 - bh, 0x93, 0xC5, 0xFD);
    }

    lv_obj_invalidate(st->canvas);
}

// ── Autocorrelation ────────────────────────────────────────────────────────────

static void recompute_bpm(BpmState *st)
{
    int n = std::min(st->count, ONSET_CAP);
    if (n < LAG_MAX + 8) return;

    // Linearise ring buffer newest-first
    static float buf[ONSET_CAP];
    for (int i = 0; i < n; i++)
        buf[i] = st->onset_buf[(st->head - 1 - i + ONSET_CAP) % ONSET_CAP];

    // Autocorrelation; track best lag and mean across all lags
    float best_val = -1.0f;
    int   best_lag = 15;
    float sum_all  = 0.0f;
    int   cnt_all  = 0;

    for (int lag = LAG_MIN; lag <= LAG_MAX && lag < n; lag++) {
        float sum = 0.0f;
        int   cnt = n - lag;
        for (int i = 0; i < cnt; i++) sum += buf[i] * buf[i + lag];
        float acf = sum / (float)cnt;
        sum_all += acf;
        cnt_all++;
        if (acf > best_val) { best_val = acf; best_lag = lag; }
    }

    // Confidence: how much the peak stands above the mean (contrast ratio)
    float mean_acf = (cnt_all > 0) ? sum_all / (float)cnt_all : 0.0f;
    float new_conf = (best_val + mean_acf > 1e-6f)
                   ? (best_val - mean_acf) / (best_val + mean_acf)
                   : 0.0f;
    if (new_conf < 0.0f) new_conf = 0.0f;

    float new_bpm = FPS * 60.0f / (float)best_lag;

    if (new_conf > 0.05f) {
        st->bpm        = (st->bpm < 1.0f) ? new_bpm : st->bpm * 0.7f + new_bpm * 0.3f;
        st->confidence = st->confidence * 0.7f + new_conf * 0.3f;
    } else {
        st->confidence *= 0.92f;
    }
}

// ── Timer callback ──────────────────────────────────────────────────────────────

static void bpm_timer_cb(lv_timer_t *t)
{
    auto *st = static_cast<BpmState *>(lv_timer_get_user_data(t));
    if (!st) return;

    AudioPacket pkt{};
    if (xQueuePeek(g_audio_queue, &pkt, 0) != pdTRUE) return;
    if (pkt.magic != 0xAB || pkt.seq == st->last_seq) return;
    st->last_seq = pkt.seq;

    // Spectral flux: sum positive changes in low-mid bins (half-wave rectified)
    float flux = 0.0f;
    for (int i = 0; i < FLUX_BINS; i++) {
        float d = pkt.bins[i] - st->prev_bins[i];
        if (d > 0.0f) flux += d;
        st->prev_bins[i] = pkt.bins[i];
    }
    flux /= (float)FLUX_BINS;

    st->onset_buf[st->head] = flux;
    st->head = (st->head + 1) % ONSET_CAP;
    if (st->count < ONSET_CAP) st->count++;

    // Recompute every RECOMPUTE_N packets
    if (++st->since_recp >= RECOMPUTE_N) {
        st->since_recp = 0;

        // Option A stub: if pkt gains a host-computed bpm field (> 0), use it here
        // and skip recompute_bpm() for better accuracy.
        recompute_bpm(st);

        // Update BPM label
        if (st->bpm > 1.0f) {
            char buf[16];
            snprintf(buf, sizeof(buf), "BPM  %d", (int)(st->bpm + 0.5f));
            lv_label_set_text(st->lbl_bpm, buf);
        } else {
            lv_label_set_text(st->lbl_bpm, "BPM  ---");
        }

        // Update subtitle: BPM unit + confidence + method tag
        char sub[64];
        int  lag = (st->bpm > 1.0f)
                 ? (int)(FPS * 60.0f / st->bpm + 0.5f)
                 : 0;
        int  pct = (int)(st->confidence * 100.0f + 0.5f);
        if (pct > 100) pct = 100;
        if (st->bpm > 1.0f) {
            snprintf(sub, sizeof(sub), "conf %d%%   lag %d fr   onset autocorr",
                     pct, lag);
        } else {
            snprintf(sub, sizeof(sub), "collecting onset data...  %d / %d frames",
                     st->count, LAG_MAX + 8);
        }
        lv_label_set_text(st->lbl_sub, sub);
    }

    render_waveform(st);
}

// ── Lifecycle ──────────────────────────────────────────────────────────────────

static void on_bpm_delete(lv_event_t *e)
{
    auto *st = static_cast<BpmState *>(lv_event_get_user_data(e));
    if (st->timer)  lv_timer_delete(st->timer);
    if (st->wf_buf) heap_caps_free(st->wf_buf);
    delete st;
}

// ── Screen creation ────────────────────────────────────────────────────────────

lv_obj_t *meter_bpm_screen_create()
{
    auto *st = new BpmState{};

    st->wf_buf = static_cast<lv_color32_t *>(
        heap_caps_malloc(WF_W * WF_H * sizeof(lv_color32_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!st->wf_buf) { delete st; return theme_make_screen(); }

    lv_color32_t bg = { .blue=0x14, .green=0x14, .red=0x14, .alpha=0xFF };
    for (int i = 0; i < WF_W * WF_H; i++) st->wf_buf[i] = bg;

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_bpm_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("BPM");

    // ── Large BPM number ──────────────────────────────────────────────────────
    lv_obj_t *lbl_bpm = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_bpm);
    lv_label_set_text(lbl_bpm, "BPM  ---");
    lv_obj_set_style_text_color(lbl_bpm, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(lbl_bpm, &lv_font_montserrat_36, 0);
    lv_obj_set_pos(lbl_bpm, 0, THEME_CONTENT_Y + 28);
    lv_obj_set_width(lbl_bpm, 800);
    lv_obj_set_style_text_align(lbl_bpm, LV_TEXT_ALIGN_CENTER, 0);
    st->lbl_bpm = lbl_bpm;

    // ── Subtitle: unit + confidence + method ──────────────────────────────────
    lv_obj_t *lbl_sub = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_sub);
    lv_label_set_text(lbl_sub, "collecting onset data...");
    lv_obj_set_style_text_color(lbl_sub, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl_sub, THEME_FONT_HINT, 0);
    lv_obj_set_pos(lbl_sub, 0, THEME_CONTENT_Y + 82);
    lv_obj_set_width(lbl_sub, 800);
    lv_obj_set_style_text_align(lbl_sub, LV_TEXT_ALIGN_CENTER, 0);
    st->lbl_sub = lbl_sub;

    // ── Accuracy note ─────────────────────────────────────────────────────────
    lv_obj_t *note = lv_label_create(scr);
    lv_obj_remove_style_all(note);
    lv_label_set_text(note, "+/-2-4 BPM accuracy at 30 fps, best for steady 4/4, 8 s warmup");
    lv_obj_set_style_text_color(note, lv_color_hex(0x505050), 0);
    lv_obj_set_style_text_font(note, THEME_FONT_HINT, 0);
    lv_obj_set_pos(note, 0, THEME_CONTENT_Y + 104);
    lv_obj_set_width(note, 800);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);

    // ── Onset waveform label ──────────────────────────────────────────────────
    lv_obj_t *wf_lbl = lv_label_create(scr);
    lv_obj_remove_style_all(wf_lbl);
    lv_label_set_text(wf_lbl, "spectral flux (onset)  last 8 s  <");
    lv_obj_set_style_text_color(wf_lbl, lv_color_hex(0x404040), 0);
    lv_obj_set_style_text_font(wf_lbl, THEME_FONT_HINT, 0);
    lv_obj_set_pos(wf_lbl, 8, WF_Y - 18);

    // ── Onset waveform canvas ─────────────────────────────────────────────────
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, WF_Y);
    lv_obj_set_size(canvas, WF_W, WF_H);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(canvas, st->wf_buf, WF_W, WF_H, LV_COLOR_FORMAT_ARGB8888);
    st->canvas = canvas;

    // ── Foot bar ──────────────────────────────────────────────────────────────
    foot_create(scr);

    // ── Nav swipe ─────────────────────────────────────────────────────────────
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        nav_swipe(dir_h, dir_v);
    }, nullptr);

    st->timer = lv_timer_create(bpm_timer_cb, 50, st);
    return scr;
}
