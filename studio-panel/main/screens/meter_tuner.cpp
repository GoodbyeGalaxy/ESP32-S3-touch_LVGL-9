// meter_tuner.cpp — Chromatic Bass Tuner
// HPS pitch detection (log-space sum) from AudioPacket.bins[256] (20Hz–20kHz, log-spaced).
// Canvas: PSRAM ARGB8888 — glow ring, cents bar, needle. LVGL labels: note, octave, Hz.
// Updates at 10 Hz (100ms timer) — sufficient for stable pitch display.

#include <initializer_list>
#include <cmath>
#include <cstring>
#include <algorithm>
#include "meter_tuner.h"
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

static const char *TAG __attribute__((unused)) = "meter_tuner";

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int CW = 800;
static constexpr int CH = 480;

// HPS harmonic bin shifts for log-spaced bins (20Hz..20kHz, 256 bins):
//   Δi = 255 * log10(N) / 3
static constexpr int H2 = 26;  // 2nd harmonic (×2 freq)
static constexpr int H3 = 41;  // 3rd harmonic (×3 freq)
static constexpr int H4 = 51;  // 4th harmonic (×4 freq)

// Search range: E0 (~21Hz, bin=0) to ~500Hz (bin=130), covers 5-string bass
static constexpr int BIN_LO = 2;
static constexpr int BIN_HI = 130;

// Silence gate: below this peak level → show no-signal state
static constexpr float SIGNAL_THRESHOLD_DB = -42.0f;

// Glow ring (behind note labels)
static constexpr int RING_CX = 400;
static constexpr int RING_CY = 148;
static constexpr int RING_R  = 92;

// Cents bar geometry (centered horizontally)
static constexpr int BAR_W  = 560;
static constexpr int BAR_H  = 20;
static constexpr int BAR_X  = (CW - BAR_W) / 2;   // 120
static constexpr int BAR_CX = CW / 2;              // 400
static constexpr int BAR_Y  = 282;

// Needle extends above + below the bar
static constexpr int NEEDLE_OVER = 14;

// Note names (sharps notation)
static const char *NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

// A4 reference presets — index matches concert_pitch_sel in TunerState
static constexpr float A4_PRESETS[] = { 440.0f, 432.0f, 442.0f, 443.0f };
static constexpr int   A4_MIDI      = 69;

// ── State ─────────────────────────────────────────────────────────────────────

struct TunerState {
    lv_timer_t   *timer    = nullptr;
    lv_obj_t     *canvas   = nullptr;
    lv_color32_t *buf      = nullptr;

    // LVGL labels (over canvas)
    lv_obj_t     *lbl_note = nullptr;   // "E", "A", "C#" …
    lv_obj_t     *lbl_oct  = nullptr;   // "2"
    lv_obj_t     *lbl_freq = nullptr;   // "41.2 Hz"

    // Needle animation state
    float         needle_x   = (float)BAR_CX;  // pixel, smoothed
    float         smooth_hz  = 0.0f;
    int           note_midi  = -1;     // last stable MIDI note (-1 = none)
    bool          has_signal = false;

    uint32_t      last_seq   = 0xFFFFFFFF;

    // Settings
    int concert_pitch_sel = 0;  // index into A4_PRESETS
};

// ── Pixel helpers ─────────────────────────────────────────────────────────────

static inline void put_px(lv_color32_t *buf, int x, int y,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    lv_color32_t *p = buf + y * CW + x;
    p->red = r; p->green = g; p->blue = b; p->alpha = 0xFF;
}

static void fill_rect(lv_color32_t *buf, int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b)
{
    for (int ry = y; ry < y + h; ry++)
        for (int cx_ = x; cx_ < x + w; cx_++)
            put_px(buf, cx_, ry, r, g, b);
}

// ── HPS pitch detection ───────────────────────────────────────────────────────
// bins[]: log-scaled magnitudes 0..1, 256 bins, 20Hz..20kHz log-spaced.
// Log-space HPS: sum bins at i, i+H2, i+H3, i+H4 — peak = fundamental.
// Returns detected frequency in Hz.

static float hps_detect_hz(const float *bins)
{
    float best_score = -1.0f;
    int   best_i     = BIN_LO;

    for (int i = BIN_LO; i < BIN_HI; i++) {
        float s = bins[i];
        if (i + H2 < 256) s += bins[i + H2];
        if (i + H3 < 256) s += bins[i + H3];
        if (i + H4 < 256) s += bins[i + H4];
        if (s > best_score) { best_score = s; best_i = i; }
    }

    // Parabolic interpolation for sub-bin accuracy
    float f0 = best_i > 0   ? bins[best_i - 1] : 0.0f;
    float f1 = bins[best_i];
    float f2 = best_i < 255 ? bins[best_i + 1] : 0.0f;
    float delta = 0.0f;
    float denom = 2.0f * f1 - f0 - f2;
    if (fabsf(denom) > 1e-6f) {
        delta = 0.5f * (f2 - f0) / denom;
        if (delta < -0.5f) delta = -0.5f;
        if (delta >  0.5f) delta =  0.5f;
    }

    // bin → Hz: f = 20 × 1000^(bin/255)
    return 20.0f * powf(1000.0f, ((float)best_i + delta) / 255.0f);
}

// ── Glow ring ─────────────────────────────────────────────────────────────────
// Soft radial glow behind the note display.
// Color shifts green (in-tune <5c), amber (~20c), purple (far off).

static void draw_glow(lv_color32_t *buf, bool signal, float cents_abs)
{
    uint8_t gr, gg, gb;
    if (!signal) {
        gr = 0x20; gg = 0x20; gb = 0x20;
    } else if (cents_abs < 5.0f) {
        gr = 0x00; gg = 0xDD; gb = 0x44;   // green
    } else if (cents_abs < 20.0f) {
        // green → orange
        float t = (cents_abs - 5.0f) / 15.0f;
        gr = (uint8_t)(0xFF * t);
        gg = (uint8_t)(0xDD * (1.0f - t) + 0x66 * t);
        gb = (uint8_t)(0x44 * (1.0f - t));
    } else {
        // orange → red
        float t = std::min(1.0f, (cents_abs - 20.0f) / 30.0f);
        gr = 0xFF;
        gg = (uint8_t)(0x66 * (1.0f - t) + 0x18 * t);
        gb = (uint8_t)(0x18 * t);
    }

    // Blend glow ON TOP of the canvas background (0x0A0A0A).
    // At ring edge (d→1), bright→0 and the pixel seamlessly matches the background.
    // Prevents the dark halo that forms when brightness is multiplied from zero.
    float r_inv = 1.0f / (float)RING_R;
    for (int py = RING_CY - RING_R; py <= RING_CY + RING_R; py++) {
        for (int px = RING_CX - RING_R; px <= RING_CX + RING_R; px++) {
            float dx = (float)(px - RING_CX);
            float dy = (float)(py - RING_CY);
            float d  = sqrtf(dx*dx + dy*dy) * r_inv;
            if (d > 1.0f) continue;
            float bright = (1.0f - d) * (1.0f - d) * 0.45f;
            uint8_t r = (uint8_t)std::min(255, (int)(0x0A + gr * bright));
            uint8_t g = (uint8_t)std::min(255, (int)(0x0A + gg * bright));
            uint8_t b = (uint8_t)std::min(255, (int)(0x0A + gb * bright));
            put_px(buf, px, py, r, g, b);
        }
    }
}

// ── Cents bar ─────────────────────────────────────────────────────────────────

static void draw_cents_bar(lv_color32_t *buf, float needle_x, bool signal)
{
    // Color zones (distance from center in pixels):
    //   ±30px  = ±5 cents  → green (in tune)
    //   ±120px = ±20 cents → amber
    //   beyond            → red
    for (int y = BAR_Y; y < BAR_Y + BAR_H; y++) {
        for (int x = BAR_X; x < BAR_X + BAR_W; x++) {
            int dist = abs(x - BAR_CX);
            uint8_t r, g, b;
            if (dist < 30) {
                r = 0x00; g = 0x55; b = 0x18;
            } else if (dist < 120) {
                float t = (float)(dist - 30) / 90.0f;
                r = (uint8_t)(0x66 * t);
                g = (uint8_t)(0x55 * (1.0f - t) + 0x44 * t);
                b = (uint8_t)(0x18 * (1.0f - t));
            } else {
                float t = std::min(1.0f, (float)(dist - 120) / 90.0f);
                r = (uint8_t)(0x44 + 0x33 * t);
                g = 0x08;
                b = 0x00;
            }
            // Dim when no signal
            if (!signal) { r >>= 2; g >>= 2; b >>= 2; }
            put_px(buf, x, y, r, g, b);
        }
    }

    // Border frame
    for (int x = BAR_X; x < BAR_X + BAR_W; x++) {
        put_px(buf, x, BAR_Y,            0x40, 0x40, 0x40);
        put_px(buf, x, BAR_Y + BAR_H - 1, 0x40, 0x40, 0x40);
    }
    for (int y = BAR_Y; y < BAR_Y + BAR_H; y++) {
        put_px(buf, BAR_X,            y, 0x40, 0x40, 0x40);
        put_px(buf, BAR_X + BAR_W - 1, y, 0x40, 0x40, 0x40);
    }

    // Tick marks at 0, ±25, ±50 cents (above + below bar)
    for (float tc : {-50.0f, -25.0f, 0.0f, 25.0f, 50.0f}) {
        int tx = BAR_CX + (int)(tc / 50.0f * (BAR_W / 2));
        uint8_t tc_br = (fabsf(tc) < 1.0f) ? 0x88 : 0x44;
        for (int y = BAR_Y - NEEDLE_OVER; y < BAR_Y; y++)
            put_px(buf, tx, y, tc_br, tc_br, tc_br);
        for (int y = BAR_Y + BAR_H; y < BAR_Y + BAR_H + NEEDLE_OVER; y++)
            put_px(buf, tx, y, tc_br, tc_br, tc_br);
    }

    // Needle — bright white vertical line
    if (signal) {
        int nx = (int)(needle_x + 0.5f);
        for (int dx = -1; dx <= 1; dx++) {
            uint8_t nb = (dx == 0) ? 0xFF : 0xBB;
            for (int y = BAR_Y - NEEDLE_OVER; y < BAR_Y + BAR_H + NEEDLE_OVER; y++)
                put_px(buf, nx + dx, y, nb, nb, nb);
        }
        // Diamond tip at top of needle
        for (int dx = -2; dx <= 2; dx++) {
            int abs_dx = abs(dx);
            put_px(buf, nx + dx, BAR_Y - NEEDLE_OVER - abs_dx, 0xFF, 0xFF, 0xFF);
        }
    }
}

// ── Full render ───────────────────────────────────────────────────────────────

static void render(TunerState *st, float cents_abs)
{
    lv_color32_t *buf = st->buf;

    // Background
    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) buf[i] = bg;

    draw_glow(buf, st->has_signal, cents_abs);

    // Thin horizontal separator between note zone and cents zone
    fill_rect(buf, 80, 238, 640, 1, 0x28, 0x28, 0x28);

    draw_cents_bar(buf, st->needle_x, st->has_signal);

    lv_obj_invalidate(st->canvas);
}

// ── Timer callback ────────────────────────────────────────────────────────────

static void tuner_timer_cb(lv_timer_t *t)
{
    TunerState *st = static_cast<TunerState *>(lv_timer_get_user_data(t));
    if (!st) return;

    AudioPacket pkt{};
    bool got_pkt = (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE
                    && pkt.seq != st->last_seq);

    if (got_pkt) {
        st->last_seq = pkt.seq;

        bool signal = (pkt.peak_l > SIGNAL_THRESHOLD_DB ||
                       pkt.peak_r > SIGNAL_THRESHOLD_DB);
        st->has_signal = signal;

        float cents_abs = 0.0f;

        if (signal) {
            float hz = hps_detect_hz(pkt.bins);

            // Low-pass Hz for stable display (avoids label flickering)
            if (st->smooth_hz < 5.0f) st->smooth_hz = hz;
            st->smooth_hz += (hz - st->smooth_hz) * 0.25f;

            // Hz → MIDI note + cents
            float a4_hz  = A4_PRESETS[st->concert_pitch_sel];
            float midi_f  = A4_MIDI + 12.0f * log2f(st->smooth_hz / a4_hz);
            int   midi    = (int)roundf(midi_f);
            float cents   = (midi_f - (float)midi) * 100.0f;
            cents_abs = fabsf(cents);

            // Update note label only on semitone change (hysteresis)
            if (midi != st->note_midi) {
                st->note_midi = midi;
                int   semitone = ((midi % 12) + 12) % 12;
                int   octave   = midi / 12 - 1;

                lv_label_set_text(st->lbl_note, NOTE_NAMES[semitone]);

                char oct_buf[12];
                snprintf(oct_buf, sizeof(oct_buf), "%d", octave);
                lv_label_set_text(st->lbl_oct, oct_buf);
            }

            // Frequency readout
            char freq_buf[20];
            snprintf(freq_buf, sizeof(freq_buf), "%.1f Hz", st->smooth_hz);
            lv_label_set_text(st->lbl_freq, freq_buf);

            // Smooth needle
            float target_x = (float)BAR_CX + (cents / 50.0f) * (BAR_W / 2.0f);
            target_x = std::max((float)BAR_X, std::min((float)(BAR_X + BAR_W), target_x));
            st->needle_x += (target_x - st->needle_x) * 0.3f;

        } else {
            st->note_midi  = -1;
            st->smooth_hz  = 0.0f;
            st->needle_x   = (float)BAR_CX;
            lv_label_set_text(st->lbl_note, "--");
            lv_label_set_text(st->lbl_oct,  "");
            lv_label_set_text(st->lbl_freq, "-- Hz");
        }

        render(st, cents_abs);
    }
}

// ── Screen lifecycle ──────────────────────────────────────────────────────────

static void on_tuner_delete(lv_event_t *e)
{
    TunerState *st = static_cast<TunerState *>(lv_event_get_user_data(e));
    if (!st) return;
    if (st->timer) lv_timer_delete(st->timer);
    if (st->buf)   heap_caps_free(st->buf);
    delete st;
}

lv_obj_t *meter_tuner_screen_create()
{
    TunerState *st = new TunerState{};

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
    lv_obj_add_event_cb(scr, on_tuner_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("TUNER");

    // Canvas (full screen, behind labels)
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, CW, CH);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(canvas, st->buf, CW, CH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_invalidate(canvas);
    st->canvas = canvas;

    // ── Note name label ───────────────────────────────────────────────────────
    // Centered horizontally in the glow ring, slightly above ring center.
    lv_obj_t *lbl_note = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_note);
    lv_label_set_text(lbl_note, "--");
    lv_obj_set_style_text_color(lbl_note, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(lbl_note, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_letter_space(lbl_note, 2, 0);
    lv_obj_set_size(lbl_note, 120, 44);
    lv_label_set_long_mode(lbl_note, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lbl_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_note, LV_ALIGN_TOP_MID, 0, 126);
    st->lbl_note = lbl_note;

    // ── Octave label ──────────────────────────────────────────────────────────
    lv_obj_t *lbl_oct = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_oct);
    lv_label_set_text(lbl_oct, "");
    lv_obj_set_style_text_color(lbl_oct, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl_oct, THEME_FONT_LABEL, 0);
    lv_obj_set_size(lbl_oct, 40, 20);
    lv_label_set_long_mode(lbl_oct, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lbl_oct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_oct, LV_ALIGN_TOP_MID, 40, 160);
    st->lbl_oct = lbl_oct;

    // ── "CENTS" unit label ────────────────────────────────────────────────────
    lv_obj_t *lbl_cents_unit = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_cents_unit);
    lv_label_set_text(lbl_cents_unit, "cents");
    lv_obj_set_style_text_color(lbl_cents_unit, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl_cents_unit, THEME_FONT_LABEL, 0);
    lv_obj_align(lbl_cents_unit, LV_ALIGN_TOP_MID, 0, 254);

    // ── Tick labels: -50  -25  0  +25  +50 ───────────────────────────────────
    constexpr int TICK_Y = BAR_Y + BAR_H + 8;
    struct { float cents; const char *text; } ticks[] = {
        {-50.0f, "-50"}, {-25.0f, "-25"}, {0.0f, "0"},
        {+25.0f, "+25"}, {+50.0f, "+50"}
    };
    for (auto &tk : ticks) {
        int tx = BAR_CX + (int)(tk.cents / 50.0f * (BAR_W / 2));
        lv_obj_t *tl = lv_label_create(scr);
        lv_obj_remove_style_all(tl);
        lv_label_set_text(tl, tk.text);
        lv_obj_set_style_text_color(tl, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(tl, THEME_FONT_LABEL, 0);
        lv_obj_set_size(tl, 40, 22);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(tl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(tl, tx - 18, TICK_Y);
    }

    // ── Frequency readout label ───────────────────────────────────────────────
    lv_obj_t *lbl_freq = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_freq);
    lv_label_set_text(lbl_freq, "-- Hz");
    lv_obj_set_style_text_color(lbl_freq, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl_freq, THEME_FONT_LABEL, 0);
    lv_obj_set_size(lbl_freq, 160, 24);
    lv_label_set_long_mode(lbl_freq, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lbl_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_freq, LV_ALIGN_TOP_MID, 0, BAR_Y + BAR_H + 32);
    st->lbl_freq = lbl_freq;

    // ── Foot bar + Settings ───────────────────────────────────────────────────
    lv_obj_t *right_zone = foot_create_hub_back(scr);

    static const SettingOption pitch_opts[] = {
        {"440 Hz"}, {"432 Hz"}, {"442 Hz"}, {"443 Hz"}
    };
    auto *tuner_items = new SettingItem[1];
    tuner_items[0] = { "Concert Pitch", pitch_opts, 4, &st->concert_pitch_sel };
    settings_btn_create(right_zone, scr, tuner_items, 1);

    // ── 2D swipe ──────────────────────────────────────────────────────────────
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
    }, nullptr);

    // ── Render timer (10 Hz) ──────────────────────────────────────────────────
    st->timer = lv_timer_create(tuner_timer_cb, 100, st);

    // Initial render (dark, no signal)
    render(st, 0.0f);

    return scr;
}
