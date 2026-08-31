// visuals_modes.cpp — On-Device Visual Mode renderers (Modes 0-7).
// All modes use an 800×480 LVGL canvas allocated in PSRAM by visuals.cpp.
// Canvas pixel access: lv_canvas_set_px() — writes ARGB8888 into PSRAM buffer.
// Black background constant: lv_color_hex(0x0A0A0A) — NOT 0x000000.

#include "visuals_modes.h"
#include "visuals.h"
#include "theme.h"
#include "demo_signal.h"
#include "audio_data.h"
#include "studio_one_data.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <atomic>

static const char *TAG __attribute__((unused)) = "vis_modes";

// Canvas geometry constants
static constexpr int CW = 800;
static constexpr int CH = 480;
static constexpr int CX = CW / 2;  // 400
static constexpr int CY = CH / 2;  // 240

static constexpr float TWO_PI = 6.28318530f;

// ── Shared helpers ────────────────────────────────────────────────────────────

// IN: hue 0..1 (wraps), saturation 0..1, value 0..1. OUT: lv_color_t RGB.
static lv_color_t hsv_to_color(float h, float s, float v)
{
    h = h - floorf(h);  // wrap 0..1
    float r, g, b;
    int   i = (int)(h * 6.0f);
    float f = h * 6.0f - (float)i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t_v = v * (1.0f - (1.0f - f) * s);
    switch (i % 6) {
        case 0: r = v;   g = t_v;  b = p;  break;
        case 1: r = q;   g = v;    b = p;  break;
        case 2: r = p;   g = v;    b = t_v; break;
        case 3: r = p;   g = q;    b = v;  break;
        case 4: r = t_v; g = p;    b = v;  break;
        default: r = v;  g = p;    b = q;  break;
    }
    return lv_color_make(
        (uint8_t)(r * 255.0f),
        (uint8_t)(g * 255.0f),
        (uint8_t)(b * 255.0f));
}

// IN: canvas, x, y, color. OUT: nothing. Direct PSRAM write — no LVGL draw task.
static inline void put_px(lv_obj_t *canvas, int x, int y, lv_color_t col)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
    if (!db || !db->data) return;
    lv_color32_t *p = reinterpret_cast<lv_color32_t *>(db->data) + y * CW + x;
    p->red = col.red; p->green = col.green; p->blue = col.blue; p->alpha = 0xFF;
}

// Bresenham line — no DRAM draw-task allocation.
static void draw_line_px(lv_obj_t *canvas, int x0, int y0, int x1, int y1, lv_color_t col)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        put_px(canvas, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// IN: canvas, fade opacity 0..255. OUT: palette bg_tint blended into Lissajous region.
// Direct PSRAM buffer write — zero LVGL draw-task allocations (no DRAM risk).
static void fade_canvas(lv_obj_t *canvas, lv_opa_t opa)
{
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
    if (!db || !db->data) return;

    lv_color32_t *buf = reinterpret_cast<lv_color32_t *>(db->data);
    const VisualPalette *pal = visuals_get_palette();
    uint32_t inv = 255u - (uint32_t)opa;
    uint32_t bgr = (uint32_t)pal->bg_tint.red   * opa;
    uint32_t bgg = (uint32_t)pal->bg_tint.green * opa;
    uint32_t bgb = (uint32_t)pal->bg_tint.blue  * opa;

    // Clamp to Lissajous draw region: SCALE=200 → ±205px from canvas centre
    int x0 = CX - 205, y0 = CY - 205, x1 = CX + 205, y1 = CY + 205;
    if (x0 < 0)   x0 = 0;
    if (y0 < 0)   y0 = 0;
    if (x1 >= CW) x1 = CW - 1;
    if (y1 >= CH) y1 = CH - 1;

    for (int y = y0; y <= y1; y++) {
        lv_color32_t *row = buf + y * CW;
        for (int x = x0; x <= x1; x++) {
            row[x].red   = (uint8_t)((row[x].red   * inv + bgr) >> 8);
            row[x].green = (uint8_t)((row[x].green * inv + bgg) >> 8);
            row[x].blue  = (uint8_t)((row[x].blue  * inv + bgb) >> 8);
        }
    }
    lv_obj_invalidate(canvas);
}

// IN: canvas. OUT: fills with active palette bg_tint. Direct PSRAM fill — no draw task.
static void clear_canvas(lv_obj_t *canvas)
{
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
    if (!db || !db->data) return;
    lv_color32_t *buf  = reinterpret_cast<lv_color32_t *>(db->data);
    lv_color_t    bg_c = visuals_get_palette()->bg_tint;
    lv_color32_t  fill;
    fill.red   = bg_c.red;
    fill.green = bg_c.green;
    fill.blue  = bg_c.blue;
    fill.alpha = 0xFF;
    for (int i = 0; i < CW * CH; i++) buf[i] = fill;
    lv_obj_invalidate(canvas);
}

// ── Latest AudioPacket snapshot ───────────────────────────────────────────────

static AudioPacket s_pkt{};

static void snap_packet()
{
    xQueuePeek(g_audio_queue, &s_pkt, 0);
}

// ── demo_signal_is_forced_by_user helper (needed by visuals.cpp) ──────────────
// We track the forced state locally via a flag that mirrors demo_signal_set_forced.
static std::atomic<bool> s_demo_forced{false};

// visuals.cpp calls demo_signal_set_forced then checks this.
// We intercept by providing the is_forced_by_user symbol here.
bool demo_signal_is_forced_by_user()
{
    return s_demo_forced.load(std::memory_order_relaxed);
}

// Override: intercept demo_signal_set_forced to mirror the flag here too.
// NOTE: visuals.cpp calls demo_signal_set_forced (the real one from demo_signal.cpp)
// AND this translation unit exposes demo_signal_is_forced_by_user.
// The forced state atom in demo_signal.cpp is the ground truth; we read it via
// demo_signal_is_active() for the button label, but track the explicit user toggle
// in s_demo_forced so refresh_demo_btn() can distinguish forced vs auto.
// visuals.cpp on_demo_btn calls demo_signal_set_forced(now) and then refresh_demo_btn().
// We also need demo_signal_is_forced_by_user() to reflect that same toggle.
// Solution: on_demo_btn in visuals.cpp sets s_demo_forced here via an alias.
// To avoid circular includes, expose a setter here.
void visuals_modes_set_demo_forced(bool v)
{
    s_demo_forced.store(v, std::memory_order_relaxed);
}

// ── MODE 0: Lissajous XL ──────────────────────────────────────────────────────
// Phosphor trail: dim overlay each frame + plot new goniometer sample.
// Trail length adapts with loudness. Hue shifts with mood.

struct Lissajous0State {
    float last_x = 0.0f;
    float last_y = 0.0f;
    bool  frozen = false;
    uint32_t freeze_end_ms = 0;
};

static Lissajous0State s_lis;

static void mode0_init(lv_obj_t *canvas)
{
    s_lis = {};
    clear_canvas(canvas);
}

static void mode0_tick(lv_obj_t *canvas, uint32_t t_ms)
{
    snap_packet();

    if (s_lis.frozen && t_ms >= s_lis.freeze_end_ms) {
        s_lis.frozen = false;
    }

    // Phosphor trail: dim previous content with semi-transparent black
    // Loudness drives trail persistence: higher energy → faster fade
    float loudness = fmaxf(0.0f, fminf(1.0f, (s_pkt.momentary + 60.0f) / 60.0f));
    lv_opa_t fade_opa = (lv_opa_t)(15 + (int)(loudness * 35));  // 15..50
    fade_canvas(canvas, fade_opa);

    if (s_lis.frozen) return;

    // Map goniometer L/R to screen (L=X, R=Y on polar display)
    float gl = fmaxf(-1.0f, fminf(1.0f, s_pkt.gonio_l));
    float gr = fmaxf(-1.0f, fminf(1.0f, s_pkt.gonio_r));

    // Goniometer → Lissajous (rotate 45°)
    float screen_x = (gl + gr) * 0.5f;  // mid
    float screen_y = (gl - gr) * 0.5f;  // side

    constexpr float SCALE = 200.0f;
    int px = CX + (int)(screen_x * SCALE);
    int py = CY - (int)(screen_y * SCALE);

    // Velocity → brightness
    float dx = screen_x - s_lis.last_x;
    float dy = screen_y - s_lis.last_y;
    float vel = sqrtf(dx * dx + dy * dy);
    float bright = fminf(1.0f, 0.4f + vel * 30.0f);

    // Color: lerp between palette primary and accent driven by mood/velocity
    const VisualPalette *pal = visuals_get_palette();
    lv_color_t col = lv_color_mix(pal->accent, pal->primary,
                                  (uint8_t)(g_visuals_mood * 255.0f));
    // Dim by inverse brightness (bright=1 → full, bright=0 → dim)
    col = lv_color_mix(col, pal->bg_tint, (uint8_t)(bright * 255.0f));

    // Draw a small glow cluster — direct PSRAM write with alpha blend, zero draw tasks.
    {
        lv_draw_buf_t *db0 = lv_canvas_get_draw_buf(canvas);
        lv_color32_t  *b0  = (db0 && db0->data)
                            ? reinterpret_cast<lv_color32_t *>(db0->data) : nullptr;
        if (b0) {
            for (int r = 2; r >= 0; r--) {
                uint8_t opa = (r == 0) ? 255u : (r == 1 ? 128u : 51u);
                for (int dy2 = -r; dy2 <= r; dy2++) {
                    for (int dx2 = -r; dx2 <= r; dx2++) {
                        if (r > 0 && abs(dx2) != r && abs(dy2) != r) continue;
                        int xx = px + dx2, yy = py + dy2;
                        if ((unsigned)xx >= CW || (unsigned)yy >= CH) continue;
                        lv_color32_t *p = b0 + yy * CW + xx;
                        if (opa == 255u) {
                            p->red = col.red; p->green = col.green;
                            p->blue = col.blue; p->alpha = 0xFF;
                        } else {
                            uint32_t inv = 255u - opa;
                            p->red   = (uint8_t)((p->red   * inv + (uint32_t)col.red   * opa) >> 8);
                            p->green = (uint8_t)((p->green * inv + (uint32_t)col.green * opa) >> 8);
                            p->blue  = (uint8_t)((p->blue  * inv + (uint32_t)col.blue  * opa) >> 8);
                        }
                    }
                }
            }
        }
    }

    s_lis.last_x = screen_x;
    s_lis.last_y = screen_y;
}

static void mode0_touch(lv_obj_t *, int, int)
{
    // Tap → freeze trail for 2s
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    s_lis.frozen     = true;
    s_lis.freeze_end_ms = now_ms + 2000;
}

static void mode0_deinit(lv_obj_t *) { s_lis = {}; }

// ── MODE 1: Circular FFT ──────────────────────────────────────────────────────
// 256 FFT bins mapped to polar coords; rotation direction toggleable via tap.

struct CircFFT1State {
    float  angle_offset = 0.0f;
    int    rotation_dir = 1;   // +1=CCW, -1=CW
    float  last_bins[256];
};

static CircFFT1State s_circ;

static void mode1_init(lv_obj_t *canvas)
{
    s_circ = {};
    memset(s_circ.last_bins, 0, sizeof(s_circ.last_bins));
    clear_canvas(canvas);
}

static void mode1_tick(lv_obj_t *canvas, uint32_t)
{
    snap_packet();
    clear_canvas(canvas);

    constexpr float BASE_R  = 80.0f;
    constexpr float SCALE_R = 120.0f;
    constexpr float BPM_DEF = 120.0f;
    float rot_step = (BPM_DEF / 240.0f) * (1.0f / 30.0f) * (float)s_circ.rotation_dir;
    s_circ.angle_offset += rot_step;

    for (int i = 0; i < 256; i++) {
        s_circ.last_bins[i] += 0.3f * (s_pkt.bins[i] - s_circ.last_bins[i]);
    }

    const VisualPalette *pal1 = visuals_get_palette();

    for (int i = 0; i < 256; i++) {
        float angle   = s_circ.angle_offset + (float)i / 256.0f * TWO_PI;
        float r_end   = BASE_R + s_circ.last_bins[i] * SCALE_R;
        float energy_t = fminf(1.0f, s_circ.last_bins[i] * 3.0f);
        lv_color_t base_col = lv_color_mix(pal1->primary, pal1->secondary,
                                           (uint8_t)((1.0f - g_visuals_mood) * 255.0f));
        lv_color_t col = lv_color_mix(pal1->accent, base_col,
                                      (uint8_t)(energy_t * 255.0f));
        float cos_a = cosf(angle), sin_a = sinf(angle);
        draw_line_px(canvas,
                     (int)(CX + cos_a * BASE_R), (int)(CY + sin_a * BASE_R),
                     (int)(CX + cos_a * r_end),  (int)(CY + sin_a * r_end),
                     col);
    }
}

static void mode1_touch(lv_obj_t *, int, int)
{
    // Toggle rotation direction on tap
    s_circ.rotation_dir = -s_circ.rotation_dir;
}

static void mode1_deinit(lv_obj_t *) { s_circ = {}; }

// ── MODE 2: Aurora Waves ──────────────────────────────────────────────────────
// 3 sine wave layers modulated by FFT band energy.

struct AuroraLayer {
    float phase;
    float speed;      // phase increment per tick (tuned at 30Hz)
    float center_y;
};

struct Aurora2State {
    AuroraLayer layers[3];
    float ripple_phase = 0.0f;
    bool  has_ripple   = false;
    int   ripple_x     = 0;
};

static Aurora2State s_aurora;

static float band_energy(const float *bins, int lo, int hi)
{
    float e = 0.0f;
    for (int i = lo; i <= hi; i++) e += bins[i];
    return e / (float)(hi - lo + 1);
}

static void mode2_init(lv_obj_t *canvas)
{
    s_aurora = {};
    // Layer 0: bass — slow
    s_aurora.layers[0] = { 0.0f, 0.015f, (float)CY };
    // Layer 1: mid — medium
    s_aurora.layers[1] = { 1.0f, 0.035f, (float)(CY - 30) };
    // Layer 2: treble — fast
    s_aurora.layers[2] = { 2.0f, 0.07f,  (float)(CY - 70) };
    clear_canvas(canvas);
}

static void mode2_tick(lv_obj_t *canvas, uint32_t)
{
    snap_packet();
    clear_canvas(canvas);

    float bass_e   = band_energy(s_pkt.bins, 0,   20);
    float mid_e    = band_energy(s_pkt.bins, 60,  120);
    float treble_e = band_energy(s_pkt.bins, 180, 255);
    float energies[3] = { bass_e, mid_e, treble_e };

    float amps[3]   = { 80.0f, 40.0f, 20.0f };
    float freqs[3]  = { 0.004f, 0.007f, 0.012f };
    int   widths[3] = { 4, 2, 1 };

    const VisualPalette *pal2 = visuals_get_palette();
    const lv_color_t layer_cols[3] = { pal2->secondary, pal2->primary, pal2->accent };

    for (int li = 0; li < 3; li++) {
        AuroraLayer &l = s_aurora.layers[li];
        float e        = energies[li];
        l.phase       += l.speed * (1.0f + e * 2.0f);

        float      amp = amps[li] * (0.3f + e * 0.7f);
        lv_color_t col = lv_color_mix(layer_cols[li], pal2->secondary,
                                      (uint8_t)((1.0f - e) * 128.0f));

        for (int x = 0; x < CW - 1; x++) {
            float ripple = 0.0f;
            if (s_aurora.has_ripple && li == 0) {
                float rx = (float)(x - s_aurora.ripple_x);
                ripple = 15.0f * sinf(rx * 0.05f + s_aurora.ripple_phase)
                       * expf(-fabsf(rx) * 0.008f);
            }
            float y0f = l.center_y + amp * sinf((float)x       * freqs[li] + l.phase) + ripple;
            float y1f = l.center_y + amp * sinf((float)(x + 1) * freqs[li] + l.phase) + ripple;

            for (int w = 0; w < widths[li]; w++) {
                draw_line_px(canvas, x, (int)y0f + w, x + 1, (int)y1f + w, col);
            }
        }
    }

    if (s_aurora.has_ripple) {
        s_aurora.ripple_phase += 0.12f;
    }
}

static void mode2_touch(lv_obj_t *, int x, int)
{
    s_aurora.has_ripple  = true;
    s_aurora.ripple_x    = x;
    s_aurora.ripple_phase = 0.0f;
}

static void mode2_deinit(lv_obj_t *) { s_aurora = {}; }

// ── MODE 3: Bass Pulse ────────────────────────────────────────────────────────
// Geometric shape pulses on bass transient. Up to 4 expanding rings.

static constexpr int MAX_RINGS = 4;

struct Ring {
    float radius;
    float opacity;  // 0..255
    bool  active;
};

enum class PulseShape { Hexagon = 0, Circle, Diamond, Triangle, COUNT };

struct BassPulse3State {
    Ring       rings[MAX_RINGS];
    float      last_bass   = 0.0f;
    PulseShape shape       = PulseShape::Hexagon;
};

static BassPulse3State s_bass;

// IN: canvas, cx, cy, radius, sides (6=hex,∞=circle,4=diamond,3=tri), color.
static void draw_poly_outline(lv_obj_t *canvas, float cx, float cy,
                              float radius, int sides, lv_color_t col, int /*width*/)
{
    float angle_step  = TWO_PI / (float)sides;
    float start_angle = (sides == 4) ? TWO_PI / 8.0f : 0.0f;

    for (int i = 0; i < sides; i++) {
        float a0 = start_angle + (float)i       * angle_step;
        float a1 = start_angle + (float)(i + 1) * angle_step;
        draw_line_px(canvas,
                     (int)(cx + cosf(a0) * radius), (int)(cy + sinf(a0) * radius),
                     (int)(cx + cosf(a1) * radius), (int)(cy + sinf(a1) * radius),
                     col);
    }
}

static int shape_sides(PulseShape s)
{
    switch (s) {
        case PulseShape::Hexagon:  return 6;
        case PulseShape::Circle:   return 32;
        case PulseShape::Diamond:  return 4;
        case PulseShape::Triangle: return 3;
        default:                   return 6;
    }
}

static void mode3_init(lv_obj_t *canvas)
{
    s_bass = {};
    for (auto &r : s_bass.rings) { r.active = false; r.radius = 0; r.opacity = 0; }
    clear_canvas(canvas);
}

static void mode3_tick(lv_obj_t *canvas, uint32_t)
{
    snap_packet();
    clear_canvas(canvas);

    float bass_e = band_energy(s_pkt.bins, 0, 20);

    if (bass_e > s_bass.last_bass * 1.4f && bass_e > 0.08f) {
        for (auto &r : s_bass.rings) {
            if (!r.active) {
                r.active  = true;
                r.radius  = 0.0f;
                r.opacity = 255.0f;
                break;
            }
        }
    }
    s_bass.last_bass = bass_e;

    const VisualPalette *pal3 = visuals_get_palette();
    lv_color_t ring_col = lv_color_mix(pal3->accent, pal3->primary,
                                       (uint8_t)((1.0f - g_visuals_mood) * 255.0f));

    draw_poly_outline(canvas, CX, CY, 60.0f + bass_e * 40.0f,
                      shape_sides(s_bass.shape), ring_col, 2);

    for (auto &r : s_bass.rings) {
        if (!r.active) continue;
        r.radius  += 8.0f;
        r.opacity -= 12.0f;
        if (r.opacity <= 0.0f) { r.active = false; continue; }
        int sides = shape_sides(s_bass.shape);
        for (int d = 0; d < 3; d++) {
            draw_poly_outline(canvas, CX, CY, r.radius + (float)d, sides, ring_col, 1);
        }
    }
}

static void mode3_touch(lv_obj_t *, int, int)
{
    // Cycle shape on tap
    int next = ((int)s_bass.shape + 1) % (int)PulseShape::COUNT;
    s_bass.shape = (PulseShape)next;
}

static void mode3_deinit(lv_obj_t *) { s_bass = {}; }

// ── MODE 4: Joy Division (Unknown Pleasures) ──────────────────────────────────
// Time-scrolling FFT ridgelines on black. Newest row at bottom, oldest at top.
// Each row's black occlusion fill creates parallax depth — identical to the
// "Unknown Pleasures" album cover effect.

struct Terrain4State {
    static constexpr int ROWS = 10;
    float  history[ROWS][CW];  // ring buffer of FFT snapshots (32 KB DRAM)
    int    head       = 0;     // next row to overwrite
    int    bin_offset = 0;     // touch-controlled frequency offset
    int    tick_skip  = 0;     // throttle scroll rate
};

static Terrain4State *s_terrain = nullptr;

static void mode4_init(lv_obj_t *canvas)
{
    if (!s_terrain) s_terrain = new Terrain4State{};
    memset(s_terrain->history, 0, sizeof(s_terrain->history));
    s_terrain->head       = 0;
    s_terrain->bin_offset = 0;
    s_terrain->tick_skip  = 0;
    clear_canvas(canvas);
}

static void mode4_tick(lv_obj_t *canvas, uint32_t)
{
    snap_packet();
    if (!s_terrain) return;

    lv_draw_buf_t *db4 = lv_canvas_get_draw_buf(canvas);
    if (!db4 || !db4->data) return;
    lv_color32_t *buf = reinterpret_cast<lv_color32_t *>(db4->data);

    // Scroll: push one new FFT snapshot into history every 4 ticks (~5 Hz)
    if (++s_terrain->tick_skip >= 4) {
        s_terrain->tick_skip = 0;
        float *row = s_terrain->history[s_terrain->head];
        for (int x = 0; x < CW; x++) {
            int bin = x * 220 / CW + s_terrain->bin_offset;
            bin = std::max(0, std::min(255, bin));
            row[x] = s_pkt.bins[bin];
        }
        s_terrain->head = (s_terrain->head + 1) % Terrain4State::ROWS;
    }

    // Black background (row-major sequential write — fast PSRAM burst)
    lv_color32_t black = {0x06, 0x06, 0x08, 0xFF};
    for (int i = 0; i < CW * CH; i++) buf[i] = black;

    // Layout
    static constexpr int ROWS = Terrain4State::ROWS;
    const int ct      = THEME_CONTENT_Y + 8;
    const int cb      = CH - THEME_FOOT_H - 8;
    const int row_h   = (cb - ct) / ROWS;         // ≈40 px per row
    const int ridge_h = (int)(row_h * 0.90f);     // max ridge amplitude

    const VisualPalette *pal = visuals_get_palette();

    // Draw oldest→newest so newer rows occlude older ones (Joy Division depth effect)
    for (int ri = 0; ri < ROWS; ri++) {
        int   row_idx = (s_terrain->head + ri) % ROWS;  // ri=0 oldest, ri=ROWS-1 newest
        float *heights = s_terrain->history[row_idx];
        float  age     = (float)ri / (float)(ROWS - 1);  // 0=oldest, 1=newest
        int    base_y  = ct + (ri + 1) * row_h;

        // Ridge colour: dim accent for old rows, bright accent for new rows
        uint8_t br = (uint8_t)(35 + (uint32_t)(age * 220));
        lv_color_t ac = pal->accent;
        lv_color32_t ridge = {
            .blue  = (uint8_t)((uint32_t)ac.blue  * br / 255),
            .green = (uint8_t)((uint32_t)ac.green * br / 255),
            .red   = (uint8_t)((uint32_t)ac.red   * br / 255),
            .alpha = 0xFF
        };

        for (int x = 0; x < CW; x++) {
            int y_peak = base_y - (int)(heights[x] * ridge_h);
            if (y_peak < ct) y_peak = ct;

            // Ridge line — 2 px for readability
            if (y_peak     < CH) buf[y_peak     * CW + x] = ridge;
            if (y_peak + 1 < CH) buf[(y_peak+1) * CW + x] = ridge;

            // Occlusion fill: black from ridge downward hides older rows behind
            for (int y = y_peak + 2; y <= base_y && y < CH; y++)
                buf[y * CW + x] = black;
        }
    }

    lv_obj_invalidate(canvas);
}

static void mode4_touch(lv_obj_t *, int x, int)
{
    // Touch X shifts which FFT region is centred: range ±128 bin offset
    // x=0 → offset=-128, x=400 → offset=0, x=800 → offset=128
    s_terrain->bin_offset = (int)((float)(x - CX) / (float)CX * 128.0f);
    s_terrain->bin_offset = std::max(-128, std::min(128, s_terrain->bin_offset));
}

static void mode4_deinit(lv_obj_t *)
{
    delete s_terrain;
    s_terrain = nullptr;
}

// ── Stub implementations for unimplemented modes (5-6, WP-E) ─────────────────

static void mode_stub_init(lv_obj_t *c)   { clear_canvas(c); }
static void mode_stub_tick(lv_obj_t *, uint32_t) {}
static void mode_stub_touch(lv_obj_t *, int, int) {}
static void mode_stub_deinit(lv_obj_t *) {}

// ── MODE 7: Julia Set / Mandelbrot ────────────────────────────────────────────
// Background task renders escape-time array (200×120) into PSRAM double-buffer.
// tick() (LVGL task): maps escape counts → HSV palette, writes 4×4 px blocks.
//
// Coordinate range (Julia):   zr ∈ [-1.75, 1.75], zi ∈ [-1.0, 1.0]
// c parameter from audio:      c_real ← bass_energy, c_imag ← treble_energy
// Mandelbrot: standard c=pixel, z starts at (0,0). toggled by 1.5s touch hold.

static constexpr int   JULIA_W       = 400;
static constexpr int   JULIA_H       = 240;
static constexpr int   JULIA_MAX_ITER = 128;
static constexpr int   JULIA_SCALE   = 2;    // upscale factor → 800×480
static constexpr float JULIA_RE_SPAN = 3.5f; // real axis full span
static constexpr float JULIA_IM_SPAN = 2.0f; // imaginary axis full span

// Beautiful Julia-set c-parameter waypoints (near Mandelbrot boundary)
static constexpr float JULIA_TRAJ[][2] = {
    {-0.4f,      0.6f  },   // Douady spiral
    {-0.7269f,   0.1889f},  // dendritic / Siegel disk
    {-0.8f,      0.156f},   // Douady rabbit
    {-0.12f,     0.74f },   // thin dendrite
    {-0.70176f, -0.3842f},  // spiral (neg imag)
    { 0.285f,    0.01f },   // connected intricate
    {-0.4f,     -0.6f },    // spiral (mirrored)
    {-0.7269f,  -0.1889f},  // dendritic (mirrored)
};
static constexpr int JULIA_TRAJ_COUNT = 8;

// ── Julia state ───────────────────────────────────────────────────────────────

// Escape-time double buffer (each 24 KB, PSRAM)
static uint8_t *s_julia_buf_front = nullptr;
static uint8_t *s_julia_buf_back  = nullptr;

// Render task
static TaskHandle_t    s_julia_task     = nullptr;
static SemaphoreHandle_t s_frame_ready  = nullptr;

// Render parameters (written by tick, read by render task — atomic where possible)
static std::atomic<float> s_c_real{-0.4f};
static std::atomic<float> s_c_imag{ 0.6f};
static std::atomic<float> s_zoom_atomic{1.0f};
static std::atomic<float> s_zoom_cx_atomic{0.0f};
static std::atomic<float> s_zoom_cy_atomic{0.0f};
static std::atomic<bool>  s_mandelbrot_mode{false};
static std::atomic<bool>  s_julia_running{false};

// Palette animation state (LVGL task only)
static float s_phase_offset   = 0.0f;

// Trajectory position through JULIA_TRAJ waypoints (Julia only, LVGL task only)
static float s_traj_pos       = 0.0f;

// Zoom state (LVGL task only)
static float s_zoom           = 1.0f;
static float s_zoom_cx        = 0.0f;
static float s_zoom_cy        = 0.0f;
static float s_loudness_integral = 0.0f;

// Touch hold detection (LVGL task only)
static uint32_t s_touch_down_ms  = 0;
static int      s_touch_down_x   = -1;
static int      s_touch_down_y   = -1;
static bool     s_touch_held     = false;

// Mandelbrot autonomous zoom state
static uint32_t s_mandel_tick_count   = 0;
static int      s_mandel_center_idx   = 0;
static const float k_interesting[][2] = {
    {-0.75f,  0.10f},
    {-0.12f,  0.74f},
    { 0.28f,  0.01f},
    {-1.40f,  0.00f},
};
static constexpr int k_interesting_count =
    (int)(sizeof(k_interesting) / sizeof(k_interesting[0]));

// ── Render task ───────────────────────────────────────────────────────────────
// Runs on Core 0, Priority 2. Computes escape-time for the back buffer,
// then gives the semaphore so tick() can swap and display.
// MUST NOT call any LVGL API.

static void julia_render_task(void *pvParam)
{
    (void)pvParam;

    while (s_julia_running.load(std::memory_order_relaxed)) {
        // Read current parameters atomically
        float cr   = s_c_real.load(std::memory_order_relaxed);
        float ci   = s_c_imag.load(std::memory_order_relaxed);
        float zoom = s_zoom_atomic.load(std::memory_order_relaxed);
        float cx   = s_zoom_cx_atomic.load(std::memory_order_relaxed);
        float cy   = s_zoom_cy_atomic.load(std::memory_order_relaxed);
        bool  mand = s_mandelbrot_mode.load(std::memory_order_relaxed);

        float half_re = (JULIA_RE_SPAN * 0.5f) / zoom;
        float half_im = (JULIA_IM_SPAN * 0.5f) / zoom;

        uint8_t *buf = s_julia_buf_back;
        if (!buf) { vTaskDelay(pdMS_TO_TICKS(33)); continue; }

        for (int py = 0; py < JULIA_H; py++) {
            float zi_pixel = cy + ((float)py / (float)JULIA_H - 0.5f) * (half_im * 2.0f);

            for (int px = 0; px < JULIA_W; px++) {
                float zr_pixel = cx + ((float)px / (float)JULIA_W - 0.5f) * (half_re * 2.0f);

                float zr, zi, crit_r, crit_i;
                if (mand) {
                    // Mandelbrot: c = pixel, z starts at (0,0)
                    zr     = 0.0f;
                    zi     = 0.0f;
                    crit_r = zr_pixel;
                    crit_i = zi_pixel;
                } else {
                    // Julia: z = pixel, c = audio-driven
                    zr     = zr_pixel;
                    zi     = zi_pixel;
                    crit_r = cr;
                    crit_i = ci;
                }

                int iter = 0;
                while (iter < JULIA_MAX_ITER) {
                    float zr2 = zr * zr;
                    float zi2 = zi * zi;
                    if (zr2 + zi2 > 4.0f) break;
                    float tmp = zr2 - zi2 + crit_r;
                    zi  = 2.0f * zr * zi + crit_i;
                    zr  = tmp;
                    iter++;
                }

                // 0 = in-set (never escaped), 1..63 = escaped at iteration
                buf[py * JULIA_W + px] = (iter >= JULIA_MAX_ITER) ? 0 : (uint8_t)iter;
            }
        }

        // Signal tick() that a new frame is ready
        xSemaphoreGive(s_frame_ready);

        // Yield briefly so tick() can swap before we overwrite again
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    vTaskDelete(nullptr);
}

// ── Julia init / deinit ───────────────────────────────────────────────────────

static void mode7_init(lv_obj_t *canvas)
{
    clear_canvas(canvas);

    // Allocate double buffer in PSRAM
    s_julia_buf_front = static_cast<uint8_t *>(
        heap_caps_malloc(JULIA_W * JULIA_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_julia_buf_back  = static_cast<uint8_t *>(
        heap_caps_malloc(JULIA_W * JULIA_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (s_julia_buf_front) memset(s_julia_buf_front, 0, JULIA_W * JULIA_H);
    if (s_julia_buf_back)  memset(s_julia_buf_back,  0, JULIA_W * JULIA_H);

    // Reset state
    s_phase_offset      = 0.0f;
    s_traj_pos          = 0.0f;  // starts at waypoint 0: c=(-0.4, 0.6)
    s_zoom              = 5.0f;
    s_zoom_cx           = 0.5f;
    s_zoom_cy           = 0.1f;
    s_loudness_integral = 0.0f;
    s_touch_down_ms     = 0;
    s_touch_held        = false;
    s_mandel_tick_count = 0;
    s_mandel_center_idx = 0;

    s_zoom_atomic.store(5.0f,  std::memory_order_relaxed);
    s_zoom_cx_atomic.store(0.5f, std::memory_order_relaxed);
    s_zoom_cy_atomic.store(0.1f, std::memory_order_relaxed);
    s_c_real.store(-0.4f, std::memory_order_relaxed);
    s_c_imag.store( 0.6f, std::memory_order_relaxed);
    s_mandelbrot_mode.store(false, std::memory_order_relaxed);

    // Create semaphore (binary)
    s_frame_ready = xSemaphoreCreateBinary();

    // Launch render task (Core 0, priority 2, 3072 byte stack)
    s_julia_running.store(true, std::memory_order_relaxed);
    xTaskCreatePinnedToCore(julia_render_task, "julia_render",
                            3072, nullptr, 2, &s_julia_task, 1);  // core 1: no LVGL competition
}

static void mode7_deinit(lv_obj_t *)
{
    s_julia_running.store(false, std::memory_order_relaxed);

    // Force-delete the render task — it holds no mutexes, only our PSRAM buffers
    // which we free below. vTaskDelete on another task is non-blocking and safe
    // from LVGL task context. Avoids the 60ms vTaskDelay that froze the UI.
    if (s_julia_task) {
        vTaskDelete(s_julia_task);
        s_julia_task = nullptr;
    }

    if (s_frame_ready) {
        vSemaphoreDelete(s_frame_ready);
        s_frame_ready = nullptr;
    }

    heap_caps_free(s_julia_buf_front);
    heap_caps_free(s_julia_buf_back);
    s_julia_buf_front = nullptr;
    s_julia_buf_back  = nullptr;
}

// ── Julia tick (LVGL task) ────────────────────────────────────────────────────

static void mode7_tick(lv_obj_t *canvas, uint32_t /*t_ms*/)
{
    snap_packet();

    // --- Read current audio packet ---
    float momentary_norm = fmaxf(0.0f, fminf(1.0f,
        (s_pkt.momentary + 60.0f) / 60.0f));

    // Bass and treble energies for c parameter
    float bass_e   = 0.0f;
    float treble_e = 0.0f;
    for (int i = 0;   i <= 20;  i++) bass_e   += s_pkt.bins[i];
    for (int i = 150; i <= 255; i++) treble_e += s_pkt.bins[i];
    bass_e   /= 21.0f;
    treble_e /= 106.0f;

    // Spectral flatness proxy
    float bin_mean = 0.0f, bin_max = 0.0f;
    for (int i = 0; i < 256; i++) {
        bin_mean += s_pkt.bins[i];
        if (s_pkt.bins[i] > bin_max) bin_max = s_pkt.bins[i];
    }
    bin_mean /= 256.0f;
    float flatness_proxy = (bin_max > 0.01f)
        ? fminf(1.0f, bin_mean / bin_max * 4.0f) : 0.5f;

    // BPM from Studio One, fallback 120
    float bpm = 120.0f;
    {
        StudioOneState s1{};
        if (g_studio_one_queue && xQueuePeek(g_studio_one_queue, &s1, 0) == pdTRUE && s1.bpm > 0.0f) {
            bpm = s1.bpm;
        }
    }

    // --- Update c parameter (Julia only) ---
    // Interpolate along a trajectory of known-beautiful Julia-set waypoints
    // (all near the Mandelbrot boundary). Each segment lasts ~30 s at 20 Hz.
    // Audio adds a subtle ±0.05 wobble so live music still has influence.
    if (!s_mandelbrot_mode.load(std::memory_order_relaxed)) {
        s_traj_pos += 1.0f / 600.0f;  // 600 ticks per waypoint ≈ 30 s at 20 Hz
        float pos   = s_traj_pos - floorf(s_traj_pos / JULIA_TRAJ_COUNT) * JULIA_TRAJ_COUNT;
        int   wi    = (int)pos % JULIA_TRAJ_COUNT;
        int   wj    = (wi + 1) % JULIA_TRAJ_COUNT;
        float alpha = pos - (int)pos;
        // Audio modulates c via slow sinusoids — oscillates around the waypoint
        // rather than drifting away. Small amplitude keeps it near the beautiful boundary.
        float cr    = JULIA_TRAJ[wi][0] * (1.0f - alpha) + JULIA_TRAJ[wj][0] * alpha
                      + bass_e   * 0.018f * sinf(s_traj_pos * 5.3f);
        float ci    = JULIA_TRAJ[wi][1] * (1.0f - alpha) + JULIA_TRAJ[wj][1] * alpha
                      + treble_e * 0.014f * cosf(s_traj_pos * 4.1f);
        s_c_real.store(cr, std::memory_order_relaxed);
        s_c_imag.store(ci, std::memory_order_relaxed);
    }

    // --- Zoom system ---
    s_loudness_integral += momentary_norm * 0.0002f;

    if (s_mandelbrot_mode.load(std::memory_order_relaxed)) {
        // Mandelbrot: slow autonomous zoom
        s_zoom *= 1.00005f;

        // Cycle through interesting coordinates every 900 ticks (~30s)
        s_mandel_tick_count++;
        if (s_mandel_tick_count >= 900) {
            s_mandel_tick_count = 0;
            s_mandel_center_idx = (s_mandel_center_idx + 1) % k_interesting_count;
            // Set new center and reset zoom
            s_zoom    = 1.0f;
            s_zoom_cx = k_interesting[s_mandel_center_idx][0];
            s_zoom_cy = k_interesting[s_mandel_center_idx][1];
            s_zoom_cx_atomic.store(s_zoom_cx, std::memory_order_relaxed);
            s_zoom_cy_atomic.store(s_zoom_cy, std::memory_order_relaxed);
        }
    } else {
        // Julia: continuous slow drift + loudness creep + peak transient
        s_zoom *= 1.0003f;  // ~60s from 5.0 to zoom 22
        s_zoom += s_loudness_integral;
        s_loudness_integral = 0.0f;

        if (s_pkt.peak_l > -6.0f || s_pkt.peak_r > -6.0f) {
            s_zoom *= 1.05f;
        }

        // Auto-reset: jump to next c-region when fully zoomed in
        if (s_zoom > 22.0f) {
            s_zoom    = 5.0f;
            s_traj_pos += 0.5f;
            s_zoom_cx = 0.5f;
            s_zoom_cy = 0.1f;
            s_zoom_cx_atomic.store(0.5f, std::memory_order_relaxed);
            s_zoom_cy_atomic.store(0.1f, std::memory_order_relaxed);
        }
    }

    // Clamp zoom
    s_zoom = fminf(s_zoom, 20.0f);
    s_zoom = fmaxf(s_zoom, 0.5f);
    s_zoom_atomic.store(s_zoom, std::memory_order_relaxed);

    // --- Touch hold: check for Mandelbrot toggle (1500ms) ---
    if (s_touch_held) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
        if (now_ms - s_touch_down_ms >= 1500) {
            s_touch_held = false;  // consume the hold
            bool mand = !s_mandelbrot_mode.load(std::memory_order_relaxed);
            s_mandelbrot_mode.store(mand, std::memory_order_relaxed);
            // Reset zoom when toggling
            s_zoom              = 1.0f;
            s_zoom_cx           = 0.0f;
            s_zoom_cy           = 0.0f;
            s_loudness_integral = 0.0f;
            s_mandel_tick_count = 0;
            s_mandel_center_idx = 0;
            s_zoom_atomic.store(1.0f, std::memory_order_relaxed);
            s_zoom_cx_atomic.store(0.0f, std::memory_order_relaxed);
            s_zoom_cy_atomic.store(0.0f, std::memory_order_relaxed);
            if (mand) {
                // Set first interesting center immediately
                s_zoom_cx = k_interesting[0][0];
                s_zoom_cy = k_interesting[0][1];
                s_zoom_cx_atomic.store(s_zoom_cx, std::memory_order_relaxed);
                s_zoom_cy_atomic.store(s_zoom_cy, std::memory_order_relaxed);
            }
        }
    }

    // --- Phase offset (palette animation, BPM-driven) ---
    // At 30Hz (33ms ticks): phase_offset += bpm/60 * 0.033 * 2*PI
    s_phase_offset += (bpm / 60.0f) * 0.033f * TWO_PI * 0.25f;  // 4× slower colour cycle
    if (s_phase_offset > TWO_PI) s_phase_offset -= TWO_PI;

    // --- Check if a new frame is ready from the render task ---
    if (xSemaphoreTake(s_frame_ready, 0) != pdTRUE) {
        // No new frame yet — skip drawing this tick
        return;
    }

    // Swap front/back buffers
    uint8_t *tmp      = s_julia_buf_front;
    s_julia_buf_front = s_julia_buf_back;
    s_julia_buf_back  = tmp;

    // --- Colour mapping: escape_buf → canvas pixels (4×4 block per pixel) ---
    lv_draw_buf_t *db7 = lv_canvas_get_draw_buf(canvas);
    if (!db7 || !db7->data) return;
    lv_color32_t *buf7 = reinterpret_cast<lv_color32_t *>(db7->data);

    const VisualPalette *pal = visuals_get_palette();

    // Pre-compute saturation from spectral flatness
    float sat = 1.0f - flatness_proxy * 0.5f;  // 0.5..1.0

    // Pre-compute value scale from loudness
    float val_scale = 0.4f + momentary_norm * 0.6f;  // 0.4..1.0

    // bg_tint as fallback for in-set pixels (escape count == 0)
    lv_color_t bg = pal->bg_tint;
    lv_color32_t bg32 = { .blue=bg.blue, .green=bg.green, .red=bg.red, .alpha=0xFF };

    for (int jy = 0; jy < JULIA_H; jy++) {
        int canvas_y0 = jy * JULIA_SCALE;

        for (int jx = 0; jx < JULIA_W; jx++) {
            int canvas_x0 = jx * JULIA_SCALE;

            uint8_t esc = s_julia_buf_front[jy * JULIA_W + jx];

            lv_color32_t c32;
            if (esc == 0) {
                lv_color_t col = hsv_to_color(
                    0.70f + s_phase_offset / TWO_PI * 0.12f,
                    0.85f,
                    0.10f + val_scale * 0.10f);
                c32 = { .blue=col.blue, .green=col.green, .red=col.red, .alpha=0xFF };
            } else {
                float t = ((float)esc + s_phase_offset / TWO_PI * JULIA_MAX_ITER)
                          / (float)JULIA_MAX_ITER;
                t = t - floorf(t);
                lv_color_t col = hsv_to_color(t, sat, val_scale);
                c32 = { .blue=col.blue, .green=col.green, .red=col.red, .alpha=0xFF };
            }

            // Write 4×4 block direct to PSRAM — zero LVGL draw task allocation.
            for (int dy = 0; dy < JULIA_SCALE; dy++) {
                int cy2 = canvas_y0 + dy;
                if ((unsigned)cy2 >= CH) continue;
                lv_color32_t *row = buf7 + cy2 * CW;
                for (int dx = 0; dx < JULIA_SCALE; dx++) {
                    int cx2 = canvas_x0 + dx;
                    if ((unsigned)cx2 < CW) row[cx2] = c32;
                }
            }
        }
    }
    lv_obj_invalidate(canvas);
}

// ── Julia touch ───────────────────────────────────────────────────────────────

static void mode7_touch(lv_obj_t *, int x, int y)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);

    // If this is the first touch in this hold sequence, record it
    if (!s_touch_held || s_touch_down_ms == 0) {
        s_touch_down_ms = now_ms;
        s_touch_down_x  = x;
        s_touch_down_y  = y;
        s_touch_held    = true;
    }

    // On a quick tap (time elapsed < 400ms and position not far from down):
    // move zoom center. The hold toggle is handled in tick().
    uint32_t elapsed = now_ms - s_touch_down_ms;
    int dx = x - s_touch_down_x;
    int dy = y - s_touch_down_y;
    bool moved = (dx * dx + dy * dy) > (30 * 30);

    if (elapsed < 400 && !moved) {
        // Map touch to fractal coordinate at current zoom
        float new_cx = (x / (float)CW - 0.5f) * JULIA_RE_SPAN / s_zoom + s_zoom_cx;
        float new_cy = (y / (float)CH - 0.5f) * JULIA_IM_SPAN / s_zoom + s_zoom_cy;
        s_zoom_cx = new_cx;
        s_zoom_cy = new_cy;
        s_zoom_cx_atomic.store(s_zoom_cx, std::memory_order_relaxed);
        s_zoom_cy_atomic.store(s_zoom_cy, std::memory_order_relaxed);
    }

    // If user moved finger, cancel hold detection
    if (moved) {
        s_touch_held    = false;
        s_touch_down_ms = 0;
    }
}

// ── Dispatch table ────────────────────────────────────────────────────────────

struct VisualMode {
    void (*init)(lv_obj_t *);
    void (*tick)(lv_obj_t *, uint32_t);
    void (*touch)(lv_obj_t *, int, int);
    void (*deinit)(lv_obj_t *);
};

static const VisualMode DISPATCH[8] = {
    { mode0_init,  mode0_tick,  mode0_touch,  mode0_deinit  },  // 0: Lissajous
    { mode1_init,  mode1_tick,  mode1_touch,  mode1_deinit  },  // 1: Circular FFT
    { mode2_init,  mode2_tick,  mode2_touch,  mode2_deinit  },  // 2: Aurora
    { mode3_init,  mode3_tick,  mode3_touch,  mode3_deinit  },  // 3: Bass Pulse
    { mode4_init,  mode4_tick,  mode4_touch,  mode4_deinit  },  // 4: Terrain
    { mode_stub_init, mode_stub_tick, mode_stub_touch, mode_stub_deinit },  // 5: Particles (WP-E)
    { mode_stub_init, mode_stub_tick, mode_stub_touch, mode_stub_deinit },  // 6: Game of Life (WP-E)
    { mode7_init,  mode7_tick,  mode7_touch,  mode7_deinit  },  // 7: Julia Set / Mandelbrot
};

void visuals_mode_init(int idx, lv_obj_t *canvas)
{
    if (idx < 0 || idx > 7) return;
    DISPATCH[idx].init(canvas);
}

void visuals_mode_tick(int idx, lv_obj_t *canvas, uint32_t t_ms)
{
    if (idx < 0 || idx > 7) return;
    DISPATCH[idx].tick(canvas, t_ms);
}

void visuals_mode_deinit(int idx, lv_obj_t *canvas)
{
    if (idx < 0 || idx > 7) return;
    DISPATCH[idx].deinit(canvas);
}

void visuals_mode_touch(int idx, lv_obj_t *canvas, int x, int y)
{
    if (idx < 0 || idx > 7) return;
    DISPATCH[idx].touch(canvas, x, y);
}
