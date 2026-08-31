// spectrum.cpp — Unified Spectrum Analyzer
// 6 modes via centered foot pills: Bars · Curve · LED · Waterfall · Octave · Spectro
// Canvas 800×480 PSRAM ARGB8888. History ring buffer 800×64 in PSRAM (~200 KB).
// No BOOT-button navigation — all modes accessible via foot pills.

#include "spectrum.h"
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
#include <cmath>
#include <cstring>
#include <algorithm>

static const char *TAG __attribute__((unused)) = "spectrum";

// ── Layout ────────────────────────────────────────────────────────────────────

static constexpr int CW        = 800;
static constexpr int CH        = 480;
static constexpr int VIS_Y     = THEME_CONTENT_Y;              // 32
static constexpr int LABEL_H   = 52;
static constexpr int VIS_H     = THEME_CONTENT_H - LABEL_H;   // 340
static constexpr int LABEL_Y   = VIS_Y + VIS_H;               // 372

// ── Spectrum parameters ───────────────────────────────────────────────────────

static constexpr int NUM_BARS    = 64;
static constexpr int BINS_PER_BAR = 256 / NUM_BARS;           // 4
static constexpr int NUM_LED_COLS = 32;
static constexpr int NUM_OCT      = 10;
static constexpr int HIST_CAP     = 800;                       // ring-buffer depth

static constexpr float PEAK_HOLD_S = 2.0f;
static constexpr float PEAK_FALL   = 0.5f;
static constexpr float DT          = 50.0f / 1000.0f;         // 50 ms timer

// ── State ─────────────────────────────────────────────────────────────────────

struct SpectrumState {
    lv_color32_t *buf     = nullptr;  // PSRAM canvas buffer
    lv_obj_t     *canvas  = nullptr;
    lv_timer_t   *timer   = nullptr;
    float        *history = nullptr;  // PSRAM [HIST_CAP × NUM_BARS]

    int  mode       = 0;              // 0=Bars 1=Curve 2=LED 3=Waterfall 4=Octave 5=Spectro
    int  hist_head  = 0;
    int  hist_count = 0;

    float bins[NUM_BARS]       = {};
    float peak_hold[NUM_BARS]  = {};
    float peak_timer[NUM_BARS] = {};

    int      peak_sel = 0;            // 0=On 1=Off
    uint32_t last_seq = 0xFFFFFFFF;

    int      guide_sel   = 0;          // 0=Off 1=Rel dB

    lv_obj_t *mode_pills[6]   = {};
    lv_obj_t *lbl_lower[10]   = {};   // bottom label slots (freq or octave)
    lv_obj_t *lufs_badge      = nullptr;
    lv_obj_t *mode_note_lbl   = nullptr;
};

static SpectrumState *s_state = nullptr;

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
    int x1 = x + w, y1 = y + h;
    if (x  < 0)  x  = 0;
    if (x1 > CW) x1 = CW;
    if (y  < 0)  y  = 0;
    if (y1 > CH) y1 = CH;
    for (int row = y; row < y1; row++)
        for (int col = x; col < x1; col++) {
            lv_color32_t *p = buf + row * CW + col;
            p->red = r; p->green = g; p->blue = b; p->alpha = 0xFF;
        }
}

// Bresenham 2-pixel-thick line
static void draw_line(lv_color32_t *buf, int x0, int y0, int x1, int y1,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        put_px(buf, x0, y0,     r, g, b);
        put_px(buf, x0, y0 + 1, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// Guide lines at −3/−6/−12 dB relative to frame peak (bins are 0..1 normalized).
// Only drawn in bar/curve/LED/octave modes when guide_sel == 1.
static void draw_guide_lines(lv_color32_t *buf)
{
    static const float GUIDE_V[3]  = { 0.708f, 0.500f, 0.250f };  // 10^(dB/20)
    static const char *GUIDE_T[3]  = { "-3", "-6", "-12" };

    for (int g = 0; g < 3; g++) {
        int y = VIS_Y + VIS_H - (int)(GUIDE_V[g] * (float)VIS_H);
        if (y < VIS_Y || y >= VIS_Y + VIS_H) continue;
        // Dashed: 6 on, 4 off
        for (int x = 0; x < CW; x++) {
            if ((x % 10) < 6)
                put_px(buf, x, y, 0x55, 0x55, 0x55);
        }
        // Short label drawn as dots (LVGL labels handle text above canvas)
        (void)GUIDE_T;
    }
}

// ── Color helpers ─────────────────────────────────────────────────────────────

// Frequency gradient: bass = blue, mid = green, treble = yellow
static void bin_color(float mag, float ff, uint8_t &r, uint8_t &g, uint8_t &b)
{
    uint8_t br, bg, bb;
    if (ff < 0.5f) {
        float t = ff * 2.0f;
        br = (uint8_t)(0x3B + t * (float)(0x22 - 0x3B));
        bg = (uint8_t)(0x82 + t * (float)(0xC5 - 0x82));
        bb = (uint8_t)(0xF6 + t * (float)(0x5E - 0xF6));
    } else {
        float t = (ff - 0.5f) * 2.0f;
        br = (uint8_t)(0x22 + t * (float)(0xF0 - 0x22));
        bg = (uint8_t)(0xC5 + t * (float)(0xC0 - 0xC5));
        bb = (uint8_t)(0x5E + t * (float)(0x20 - 0x5E));
    }
    float bright = 0.30f + 0.70f * mag;
    r = (uint8_t)(br * bright);
    g = (uint8_t)(bg * bright);
    b = (uint8_t)(bb * bright);
}

// Thermal colormap: black → dark-blue → cyan → green → yellow → white-hot
static void thermal_color(float v, uint8_t &r, uint8_t &g, uint8_t &b)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (v < 0.25f) {
        float t = v * 4.0f;
        r = 0; g = 0; b = (uint8_t)(180.0f * t);
    } else if (v < 0.50f) {
        float t = (v - 0.25f) * 4.0f;
        r = 0; g = (uint8_t)(180.0f * t); b = (uint8_t)(180.0f + 75.0f * t);
    } else if (v < 0.75f) {
        float t = (v - 0.50f) * 4.0f;
        r = (uint8_t)(200.0f * t); g = 180; b = (uint8_t)(255.0f * (1.0f - t));
    } else {
        float t = (v - 0.75f) * 4.0f;
        r = (uint8_t)(200.0f + 55.0f * t);
        g = (uint8_t)(180.0f + 75.0f * t);
        b = 0;
    }
}

// LED column color: green → yellow → red by vertical fraction (0=bottom, 1=top)
static void led_color(float frac, uint8_t &r, uint8_t &g, uint8_t &b)
{
    if (frac < 0.65f) {
        r = 0x00; g = 0xDD; b = 0x44;
    } else if (frac < 0.85f) {
        float t = (frac - 0.65f) / 0.20f;
        r = (uint8_t)(0xFF * t); g = (uint8_t)(0xDD - 0x33 * t); b = 0;
    } else {
        r = 0xFF; g = 0x22; b = 0;
    }
}

// ── Octave-band setup ─────────────────────────────────────────────────────────

static const float OCT_CENTERS[NUM_OCT] = {
    31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
    1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
};
static const char *OCT_LABELS[NUM_OCT] = {
    "31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"
};
static int s_oct_start[NUM_OCT];
static int s_oct_end[NUM_OCT];

static void init_octave_bands()
{
    static bool done = false;
    if (done) return;
    done = true;
    const float F_MIN = 20.0f, F_MAX = 20000.0f;
    for (int i = 0; i < NUM_OCT; i++) {
        float fc  = OCT_CENTERS[i];
        float flo = fc / 1.41421f;
        float fhi = fc * 1.41421f;
        if (flo < F_MIN) flo = F_MIN;
        if (fhi > F_MAX) fhi = F_MAX;
        auto to_bin = [&](float f) -> int {
            return (int)((float)(NUM_BARS - 1) * logf(f / F_MIN) / logf(F_MAX / F_MIN) + 0.5f);
        };
        s_oct_start[i] = std::max(0, to_bin(flo));
        s_oct_end[i]   = std::min(NUM_BARS - 1, to_bin(fhi));
        if (s_oct_end[i] < s_oct_start[i]) s_oct_end[i] = s_oct_start[i];
    }
}

// ── Render: BARS ──────────────────────────────────────────────────────────────

static void render_bars(SpectrumState *st)
{
    for (int i = 0; i < NUM_BARS; i++) {
        int x0 = (int)((float)i       * (float)CW / (float)NUM_BARS);
        int x1 = (int)((float)(i + 1) * (float)CW / (float)NUM_BARS) - 1;
        if (x1 < x0) x1 = x0;
        float mag = st->bins[i];
        float ff  = (float)i / (float)(NUM_BARS - 1);
        int   bh  = std::max(1, (int)(mag * (float)VIS_H));
        uint8_t r, g, b;
        bin_color(mag, ff, r, g, b);
        fill_rect(st->buf, x0, VIS_Y + VIS_H - bh, x1 - x0 + 1, bh, r, g, b);
        // Peak-hold
        if (st->peak_sel == 0 && st->peak_hold[i] > 0.01f) {
            int py = VIS_Y + VIS_H - (int)(st->peak_hold[i] * (float)VIS_H);
            fill_rect(st->buf, x0, py - 1, x1 - x0 + 1, 2, 0xF0, 0xF0, 0xF0);
        }
    }
}

// ── Render: CURVE ─────────────────────────────────────────────────────────────

static void render_curve(SpectrumState *st)
{
    for (int i = 0; i < NUM_BARS - 1; i++) {
        int x0 = (int)(((float)i       + 0.5f) * (float)CW / (float)NUM_BARS);
        int x1 = (int)(((float)(i + 1) + 0.5f) * (float)CW / (float)NUM_BARS);
        int y0 = VIS_Y + VIS_H - (int)(st->bins[i]     * (float)VIS_H);
        int y1 = VIS_Y + VIS_H - (int)(st->bins[i + 1] * (float)VIS_H);
        draw_line(st->buf, x0, y0, x1, y1, 0x22, 0xC5, 0x5E);
    }
}

// ── Render: LED ───────────────────────────────────────────────────────────────

static constexpr int LED_SZ       = 9;
static constexpr int LED_GAP      = 2;
static constexpr int LED_COL_GAP  = 4;
static constexpr int LED_MARGIN   = 6;
static constexpr int LED_STRIDE   = (CW - 2 * LED_MARGIN + LED_COL_GAP) / NUM_LED_COLS;
static constexpr int LED_COL_W    = LED_STRIDE - LED_COL_GAP;
static constexpr int LED_LEDS     = VIS_H / (LED_SZ + LED_GAP);

static void render_led(SpectrumState *st)
{
    for (int col = 0; col < NUM_LED_COLS; col++) {
        int b0 = col * NUM_BARS / NUM_LED_COLS;
        int b1 = (col + 1) * NUM_BARS / NUM_LED_COLS - 1;
        if (b1 >= NUM_BARS) b1 = NUM_BARS - 1;

        float sum = 0.0f; int cnt = b1 - b0 + 1;
        for (int k = b0; k <= b1; k++) sum += st->bins[k];
        float level = sum / (float)cnt;
        int lit = std::min(LED_LEDS, (int)(level * (float)LED_LEDS));

        int cx = LED_MARGIN + col * LED_STRIDE;

        for (int led = 0; led < LED_LEDS; led++) {
            float frac = (float)led / (float)(LED_LEDS - 1);
            int ly = VIS_Y + VIS_H - (led + 1) * (LED_SZ + LED_GAP) + LED_GAP;
            if (ly < VIS_Y || ly + LED_SZ > CH) continue;
            uint8_t r, g, b;
            led_color(frac, r, g, b);
            if (led < lit) {
                fill_rect(st->buf, cx, ly, LED_COL_W, LED_SZ, r, g, b);
                uint8_t hr = (uint8_t)std::min(255, (int)r + 50);
                uint8_t hg = (uint8_t)std::min(255, (int)g + 50);
                uint8_t hb = (uint8_t)std::min(255, (int)b + 50);
                for (int px = cx; px < cx + 3 && px < cx + LED_COL_W; px++)
                    put_px(st->buf, px, ly, hr, hg, hb);
            } else {
                fill_rect(st->buf, cx, ly, LED_COL_W, LED_SZ,
                          (uint8_t)(r * 5 / 255),
                          (uint8_t)(g * 5 / 255),
                          (uint8_t)(b * 5 / 255));
            }
        }

        // Peak-hold dot
        if (st->peak_sel == 0) {
            float pk_sum = 0.0f;
            for (int k = b0; k <= b1; k++) pk_sum += st->peak_hold[k];
            float pk = pk_sum / (float)cnt;
            int pk_led = (int)(pk * (float)(LED_LEDS - 1));
            if (pk_led > 0 && pk_led < LED_LEDS) {
                int py = VIS_Y + VIS_H - (pk_led + 1) * (LED_SZ + LED_GAP) + LED_GAP;
                if (py >= VIS_Y) fill_rect(st->buf, cx, py, LED_COL_W, 2, 0xFF, 0xFF, 0xFF);
            }
        }
    }
}

// ── Render: WATERFALL ─────────────────────────────────────────────────────────

static void render_waterfall(SpectrumState *st)
{
    int n = std::min(st->hist_count, VIS_H);
    for (int row = 0; row < n; row++) {
        int y     = VIS_Y + row;                                    // newest at top
        int frame = (st->hist_head - 1 - row + HIST_CAP) % HIST_CAP;
        const float *fd = st->history + frame * NUM_BARS;
        for (int px = 0; px < CW; px++) {
            int bin = px * NUM_BARS / CW;
            uint8_t r, g, b;
            thermal_color(fd[bin], r, g, b);
            put_px(st->buf, px, y, r, g, b);
        }
    }
}

// ── Render: OCTAVE ────────────────────────────────────────────────────────────

static constexpr int OCT_BAR_W  = CW / NUM_OCT;
static constexpr int OCT_BAR_PAD = 5;

static void render_octave(SpectrumState *st)
{
    for (int i = 0; i < NUM_OCT; i++) {
        int x0 = i * OCT_BAR_W + OCT_BAR_PAD;
        int x1 = (i + 1) * OCT_BAR_W - OCT_BAR_PAD - 1;
        if (x1 <= x0) continue;
        int bw = x1 - x0 + 1;

        int cnt = s_oct_end[i] - s_oct_start[i] + 1;
        float sum = 0.0f;
        for (int k = s_oct_start[i]; k <= s_oct_end[i]; k++) sum += st->bins[k];
        float mag = sum / (float)cnt;
        int bh = std::max(1, (int)(mag * (float)VIS_H));

        float ff = (float)i / (float)(NUM_OCT - 1);
        uint8_t r, g, b;
        bin_color(mag, ff, r, g, b);
        fill_rect(st->buf, x0, VIS_Y + VIS_H - bh, bw, bh, r, g, b);

        // Top highlight stripe
        fill_rect(st->buf, x0, VIS_Y + VIS_H - bh - 2, bw, 2, 0xC0, 0xC0, 0xC0);

        // Peak-hold
        if (st->peak_sel == 0) {
            float pk_sum = 0.0f;
            for (int k = s_oct_start[i]; k <= s_oct_end[i]; k++) pk_sum += st->peak_hold[k];
            float pk = pk_sum / (float)cnt;
            if (pk > 0.01f) {
                int py = VIS_Y + VIS_H - (int)(pk * (float)VIS_H);
                fill_rect(st->buf, x0, py - 1, bw, 2, 0xF0, 0xF0, 0xF0);
            }
        }
    }
}

// ── Render: SPECTROGRAM ───────────────────────────────────────────────────────

static void render_spectrogram(SpectrumState *st)
{
    int n = std::min(st->hist_count, CW);
    for (int col = 0; col < n; col++) {
        int x     = CW - n + col;                                  // oldest left, newest right
        int frame = (st->hist_head - n + col + HIST_CAP) % HIST_CAP;
        const float *fd = st->history + frame * NUM_BARS;
        for (int py = 0; py < VIS_H; py++) {
            // py=0 top = treble, py=VIS_H-1 bottom = bass
            int bin = (VIS_H - 1 - py) * (NUM_BARS - 1) / (VIS_H - 1);
            uint8_t r, g, b;
            thermal_color(fd[bin], r, g, b);
            put_px(st->buf, x, VIS_Y + py, r, g, b);
        }
    }
}

// ── Frame render dispatch ─────────────────────────────────────────────────────

static void render_frame(SpectrumState *st)
{
    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) st->buf[i] = bg;

    switch (st->mode) {
        case 0: render_bars(st);        break;
        case 1: render_curve(st);       break;
        case 2: render_led(st);         break;
        case 3: render_waterfall(st);   break;
        case 4: render_octave(st);      break;
        case 5: render_spectrogram(st); break;
        default: break;
    }

    if (st->guide_sel == 1 && st->mode != 3 && st->mode != 5)
        draw_guide_lines(st->buf);

    lv_obj_invalidate(st->canvas);
}

// ── Bottom label management ───────────────────────────────────────────────────

static const struct { const char *text; float freq; } FREQ_LBL[] = {
    {"20Hz", 20.0f}, {"100", 100.0f}, {"1k", 1000.0f},
    {"5k", 5000.0f}, {"10k", 10000.0f}, {"20k", 20000.0f},
};
static const float F_MIN = 20.0f, F_MAX = 20000.0f;

static void update_bottom_labels(SpectrumState *st)
{
    if (st->mode == 4) {
        // Octave: 10 evenly-spaced labels
        for (int i = 0; i < 10; i++) {
            if (!st->lbl_lower[i]) continue;
            int cx = i * OCT_BAR_W + OCT_BAR_W / 2;
            lv_obj_set_pos(st->lbl_lower[i], cx - 10, LABEL_Y + 6);
            lv_label_set_text(st->lbl_lower[i], OCT_LABELS[i]);
            lv_obj_clear_flag(st->lbl_lower[i], LV_OBJ_FLAG_HIDDEN);
        }
    } else if (st->mode == 5) {
        // Spectrogram: Y=frequency — hide bottom labels
        for (int i = 0; i < 10; i++)
            if (st->lbl_lower[i]) lv_obj_add_flag(st->lbl_lower[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        // Bars / Curve / LED / Waterfall: log-spaced frequency labels
        for (int i = 0; i < 10; i++) {
            if (!st->lbl_lower[i]) continue;
            if (i < 6) {
                float lr = logf(FREQ_LBL[i].freq / F_MIN) / logf(F_MAX / F_MIN);
                int lx = (int)(lr * (float)CW);
                lx = std::max(2, std::min(lx, CW - 40));
                lv_obj_set_pos(st->lbl_lower[i], lx, LABEL_Y + 6);
                lv_label_set_text(st->lbl_lower[i], FREQ_LBL[i].text);
                lv_obj_clear_flag(st->lbl_lower[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(st->lbl_lower[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// ── Pill style refresh ────────────────────────────────────────────────────────

static void refresh_pills(SpectrumState *st)
{
    for (int i = 0; i < 6; i++) {
        if (!st->mode_pills[i]) continue;
        bool active = (st->mode == i);
        lv_obj_set_style_bg_color(st->mode_pills[i],
                                  active ? THEME_ACCENT : THEME_BG_CARD, 0);
        lv_obj_t *lbl = lv_obj_get_child(st->mode_pills[i], 0);
        if (lbl)
            lv_obj_set_style_text_color(lbl,
                                        active ? lv_color_hex(0x080808) : THEME_TEXT_HINT, 0);
        lv_obj_invalidate(st->mode_pills[i]);
    }
    update_bottom_labels(st);

    if (st->mode_note_lbl) {
        if (st->mode == 2)
            lv_obj_clear_flag(st->mode_note_lbl, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(st->mode_note_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── Timer callback ────────────────────────────────────────────────────────────

static void spectrum_timer_cb(lv_timer_t *timer)
{
    auto *st = static_cast<SpectrumState *>(lv_timer_get_user_data(timer));
    if (!st) return;

    AudioPacket pkt{};
    bool new_pkt = (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE && pkt.seq != st->last_seq);
    if (st->lufs_badge) {
        if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE && pkt.magic == 0xAB) {
            char buf[20];
            snprintf(buf, sizeof(buf), "M  %.1f LUFS", pkt.momentary);
            lv_label_set_text(st->lufs_badge, buf);
        } else {
            lv_label_set_text(st->lufs_badge, "M  -- LUFS");
        }
    }

    if (new_pkt) {
        st->last_seq = pkt.seq;

        for (int i = 0; i < NUM_BARS; i++) {
            float avg = 0.0f;
            for (int k = 0; k < BINS_PER_BAR; k++) avg += pkt.bins[i * BINS_PER_BAR + k];
            avg /= (float)BINS_PER_BAR;

            float alpha = (avg > st->bins[i]) ? 0.5f : 0.12f;
            st->bins[i] += alpha * (avg - st->bins[i]);

            if (st->peak_sel == 0) {
                if (st->bins[i] >= st->peak_hold[i]) {
                    st->peak_hold[i]  = st->bins[i];
                    st->peak_timer[i] = PEAK_HOLD_S;
                } else if (st->peak_timer[i] > 0.0f) {
                    st->peak_timer[i] -= DT;
                } else {
                    st->peak_hold[i] = std::max(st->peak_hold[i] - PEAK_FALL * DT, 0.0f);
                }
            } else {
                st->peak_hold[i] = std::max(st->peak_hold[i] - PEAK_FALL * 5.0f * DT, 0.0f);
            }
        }

        // Push frame to history ring buffer
        float *frame_ptr = st->history + st->hist_head * NUM_BARS;
        for (int i = 0; i < NUM_BARS; i++) frame_ptr[i] = st->bins[i];
        st->hist_head = (st->hist_head + 1) % HIST_CAP;
        if (st->hist_count < HIST_CAP) st->hist_count++;
    }

    render_frame(st);
}

// ── Screen lifecycle ──────────────────────────────────────────────────────────

static void on_spectrum_delete(lv_event_t *e)
{
    auto *st = static_cast<SpectrumState *>(lv_event_get_user_data(e));
    if (!st) return;
    if (st->timer)   lv_timer_delete(st->timer);
    if (st->buf)     heap_caps_free(st->buf);
    if (st->history) heap_caps_free(st->history);
    if (s_state == st) s_state = nullptr;
    delete st;
}

// ── Screen creation ───────────────────────────────────────────────────────────

lv_obj_t *spectrum_screen_create()
{
    if (s_state) return nullptr;  // guard: hub deletes old screen before creating new

    init_octave_bands();

    auto *st = new SpectrumState{};

    st->buf = static_cast<lv_color32_t *>(
        heap_caps_malloc(CW * CH * sizeof(lv_color32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    st->history = static_cast<float *>(
        heap_caps_malloc(HIST_CAP * NUM_BARS * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (!st->buf || !st->history) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        if (st->buf)     heap_caps_free(st->buf);
        if (st->history) heap_caps_free(st->history);
        delete st;
        return theme_make_screen();
    }

    memset(st->history, 0, HIST_CAP * NUM_BARS * sizeof(float));
    lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
    for (int i = 0; i < CW * CH; i++) st->buf[i] = bg;

    // Screen
    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_spectrum_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("SPECTRUM");

    // Canvas
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, CW, CH);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(canvas, st->buf, CW, CH, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_invalidate(canvas);
    st->canvas = canvas;

    // Bottom label slots (over canvas — created after canvas so they render on top)
    for (int i = 0; i < 10; i++) {
        lv_obj_t *lbl = lv_label_create(scr);
        lv_obj_remove_style_all(lbl);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_color(lbl, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_set_pos(lbl, 0, LABEL_Y + 6);
        st->lbl_lower[i] = lbl;
    }
    update_bottom_labels(st);

    // LUFS momentary badge — top-right overlay over canvas
    {
        lv_obj_t *badge = lv_label_create(scr);
        lv_obj_remove_style_all(badge);
        lv_label_set_text(badge, "M  -- LUFS");
        lv_obj_set_style_text_color(badge, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(badge, THEME_FONT_HINT, 0);
        lv_obj_set_pos(badge, CW - 150, VIS_Y + 8);
        st->lufs_badge = badge;
    }

    // LED mode disclaimer — shown only in LED mode
    {
        lv_obj_t *note = lv_label_create(scr);
        lv_obj_remove_style_all(note);
        lv_label_set_text(note, "Relative display — not calibrated to dBFS");
        lv_obj_set_style_text_color(note, lv_color_hex(0xF59E0B), 0);  // amber
        lv_obj_set_style_text_font(note, THEME_FONT_HINT, 0);
        lv_obj_set_pos(note, 0, LABEL_Y + 30);
        lv_obj_set_width(note, CW);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(note, LV_OBJ_FLAG_HIDDEN);
        st->mode_note_lbl = note;
    }

    // Foot bar — use foot_create_hub_back, grab the foot via parent of right_zone
    lv_obj_t *right_zone = foot_create_hub_back(scr);
    lv_obj_t *foot = lv_obj_get_parent(right_zone);

    // Settings gear in right_zone (2 items: Peak Hold + Guide Lines)
    static const SettingOption peak_opts[]  = { {"On"}, {"Off"} };
    static const SettingOption guide_opts[] = { {"Off"}, {"Rel dB"} };
    auto *items = new SettingItem[2];
    items[0] = { "Peak Hold",   peak_opts,  2, &st->peak_sel  };
    items[1] = { "Guide Lines", guide_opts, 2, &st->guide_sel };
    settings_btn_create(right_zone, scr, items, 2);

    // Centered mode pills in the foot
    static const char *PILL_NAMES[6] = { "Bars", "Curve", "LED", "Waterfall", "Octave", "Spectro" };
    constexpr int PILL_W     = 80;
    constexpr int PILL_H     = 32;
    constexpr int PILL_GAP   = 6;
    constexpr int PILLS_TOT  = 6 * PILL_W + 5 * PILL_GAP;   // 510
    constexpr int PILLS_X0   = (CW - PILLS_TOT) / 2;         // 145

    for (int i = 0; i < 6; i++) {
        lv_obj_t *pill = lv_btn_create(foot);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, PILL_W, PILL_H);
        lv_obj_set_pos(pill, PILLS_X0 + i * (PILL_W + PILL_GAP),
                       (THEME_FOOT_H - PILL_H) / 2);
        bool active = (i == 0);
        lv_obj_set_style_bg_color(pill, active ? THEME_ACCENT : THEME_BG_CARD, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, THEME_RADIUS, 0);

        lv_obj_t *lbl = lv_label_create(pill);
        lv_obj_remove_style_all(lbl);
        lv_label_set_text(lbl, PILL_NAMES[i]);
        lv_obj_set_style_text_color(lbl, active ? lv_color_hex(0x080808) : THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_center(lbl);

        st->mode_pills[i] = pill;
        lv_obj_add_event_cb(pill, [](lv_event_t *e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            if (!s_state) return;
            s_state->mode = idx;
            refresh_pills(s_state);
            lv_obj_invalidate(s_state->canvas);
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // 2D swipe — hub-aware (same as other hub screens)
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
    }, nullptr);

    st->timer = lv_timer_create(spectrum_timer_cb, 50, st);
    s_state = st;
    return scr;
}
