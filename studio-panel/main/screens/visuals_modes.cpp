// visuals_modes.cpp — On-Device Visual Mode renderers (Modes 0-4).
// All modes use an 800×480 LVGL canvas allocated in PSRAM by visuals.cpp.
// Canvas pixel access: lv_canvas_set_px() — writes ARGB8888 into PSRAM buffer.
// Black background constant: lv_color_hex(0x0A0A0A) — NOT 0x000000.

#include "visuals_modes.h"
#include "visuals.h"
#include "theme.h"
#include "demo_signal.h"
#include "audio_data.h"
#include "esp_log.h"
#include "esp_timer.h"
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

// IN: canvas, x, y, color. OUT: nothing. Bounds-checked pixel write.
static inline void put_px(lv_obj_t *canvas, int x, int y, lv_color_t col)
{
    if ((unsigned)x < CW && (unsigned)y < CH) {
        lv_canvas_set_px(canvas, x, y, col, LV_OPA_COVER);
    }
}

// IN: canvas, fade opacity 0..255. OUT: dark overlay drawn over entire canvas.
// Used for trail effects — semi-transparent black dims previous content.
static void fade_canvas(lv_obj_t *canvas, lv_opa_t opa)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color   = visuals_get_palette()->bg_tint;
    dsc.bg_opa     = opa;
    dsc.radius     = 0;
    lv_area_t area = { 0, 0, CW - 1, CH - 1 };
    lv_draw_rect(&layer, &dsc, &area);

    lv_canvas_finish_layer(canvas, &layer);
}

// IN: canvas. OUT: fills with active palette bg_tint (very dark, safe for IPS panel).
static void clear_canvas(lv_obj_t *canvas)
{
    lv_canvas_fill_bg(canvas, visuals_get_palette()->bg_tint, LV_OPA_COVER);
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

    // Draw a small glow cluster around the point
    for (int r = 2; r >= 0; r--) {
        lv_opa_t opa = (r == 0) ? LV_OPA_COVER : (r == 1 ? LV_OPA_50 : LV_OPA_20);
        for (int dy2 = -r; dy2 <= r; dy2++) {
            for (int dx2 = -r; dx2 <= r; dx2++) {
                if (r > 0 && abs(dx2) != r && abs(dy2) != r) continue; // outline only
                int xx = px + dx2, yy = py + dy2;
                if ((unsigned)xx < CW && (unsigned)yy < CH) {
                    lv_canvas_set_px(canvas, xx, yy, col, opa);
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

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    constexpr float BASE_R   = 80.0f;
    constexpr float SCALE_R  = 120.0f;
    constexpr float BPM_DEF  = 120.0f;
    // Rotation speed: BPM/240 radians per tick at 30Hz → 120 BPM = 0.5 rad/s
    float rot_step = (BPM_DEF / 240.0f) * (1.0f / 30.0f) * (float)s_circ.rotation_dir;
    s_circ.angle_offset += rot_step;

    // EMA smoothing on bins
    for (int i = 0; i < 256; i++) {
        s_circ.last_bins[i] += 0.3f * (s_pkt.bins[i] - s_circ.last_bins[i]);
    }

    // Color: lerp from palette secondary (inner ring) to primary (peaks),
    // accent marks the highest energy bins.
    const VisualPalette *pal1 = visuals_get_palette();

    for (int i = 0; i < 256; i++) {
        float angle = s_circ.angle_offset + (float)i / 256.0f * TWO_PI;
        float r_end = BASE_R + s_circ.last_bins[i] * SCALE_R;

        // Bins with high energy → accent, others lerp primary↔secondary by mood
        float energy_t = fminf(1.0f, s_circ.last_bins[i] * 3.0f);
        lv_color_t base_col = lv_color_mix(pal1->primary, pal1->secondary,
                                           (uint8_t)((1.0f - g_visuals_mood) * 255.0f));
        lv_color_t col = lv_color_mix(pal1->accent, base_col,
                                      (uint8_t)(energy_t * 255.0f));

        float cos_a = cosf(angle), sin_a = sinf(angle);

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = col;
        dsc.width = 1;
        dsc.p1.x  = (lv_value_precise_t)(CX + cos_a * BASE_R);
        dsc.p1.y  = (lv_value_precise_t)(CY + sin_a * BASE_R);
        dsc.p2.x  = (lv_value_precise_t)(CX + cos_a * r_end);
        dsc.p2.y  = (lv_value_precise_t)(CY + sin_a * r_end);
        lv_draw_line(&layer, &dsc);
    }

    lv_canvas_finish_layer(canvas, &layer);
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

    // Amplitudes
    float amps[3]   = { 80.0f, 40.0f, 20.0f };
    // Frequency of spatial wave (cycles per 800px)
    float freqs[3]  = { 0.004f, 0.007f, 0.012f };
    // Thickness
    int   widths[3] = { 4, 2, 1 };

    // Layer colours from active palette:
    // bass (0) → secondary, mid (1) → primary, treble (2) → accent
    const VisualPalette *pal2 = visuals_get_palette();
    const lv_color_t layer_cols[3] = { pal2->secondary, pal2->primary, pal2->accent };

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    for (int li = 0; li < 3; li++) {
        AuroraLayer &l  = s_aurora.layers[li];
        float e         = energies[li];
        l.phase        += l.speed * (1.0f + e * 2.0f);

        float amp = amps[li] * (0.3f + e * 0.7f);
        // Brighten/dim by energy; mood modulates mix toward secondary
        lv_color_t bright_col = lv_color_mix(layer_cols[li], pal2->secondary,
                                             (uint8_t)((1.0f - e) * 128.0f));
        lv_color_t col = bright_col;

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = col;
        dsc.width = (int32_t)widths[li];

        for (int x = 0; x < CW - 1; x++) {
            float ripple = 0.0f;
            if (s_aurora.has_ripple && li == 0) {
                float rx = (float)(x - s_aurora.ripple_x);
                s_aurora.ripple_phase += 0.0f;  // updated once per tick below
                ripple = 15.0f * sinf(rx * 0.05f + s_aurora.ripple_phase)
                       * expf(-fabsf(rx) * 0.008f);
            }
            float y0 = l.center_y + amp * sinf((float)x * freqs[li] + l.phase) + ripple;
            float y1 = l.center_y + amp * sinf((float)(x + 1) * freqs[li] + l.phase) + ripple;

            dsc.p1 = { (lv_value_precise_t)x,       (lv_value_precise_t)y0 };
            dsc.p2 = { (lv_value_precise_t)(x + 1), (lv_value_precise_t)y1 };
            lv_draw_line(&layer, &dsc);
        }
    }

    if (s_aurora.has_ripple) {
        s_aurora.ripple_phase += 0.12f;
    }

    lv_canvas_finish_layer(canvas, &layer);
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

// IN: cx, cy, radius, sides (6=hex,∞=circle,4=diamond,3=tri), color, width.
static void draw_poly_outline(lv_layer_t *layer, float cx, float cy,
                              float radius, int sides,
                              lv_color_t col, int width)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = col;
    dsc.width = (int32_t)width;

    float angle_step = TWO_PI / (float)sides;
    float start_angle = (sides == 4) ? TWO_PI / 8.0f : 0.0f;  // rotate diamond 45°

    for (int i = 0; i < sides; i++) {
        float a0 = start_angle + (float)i       * angle_step;
        float a1 = start_angle + (float)(i + 1) * angle_step;
        dsc.p1 = { (lv_value_precise_t)(cx + cosf(a0) * radius),
                   (lv_value_precise_t)(cy + sinf(a0) * radius) };
        dsc.p2 = { (lv_value_precise_t)(cx + cosf(a1) * radius),
                   (lv_value_precise_t)(cy + sinf(a1) * radius) };
        lv_draw_line(layer, &dsc);
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

    // Transient detection
    if (bass_e > s_bass.last_bass * 1.4f && bass_e > 0.08f) {
        // Find an inactive ring slot
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

    // Color: mood lerps between palette primary (low mood) and accent (high mood)
    const VisualPalette *pal3 = visuals_get_palette();
    lv_color_t ring_col = lv_color_mix(pal3->accent, pal3->primary,
                                       (uint8_t)((1.0f - g_visuals_mood) * 255.0f));

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Centre geometric shape — breathes with bass energy
    {
        float base_r = 60.0f + bass_e * 40.0f;
        int sides    = shape_sides(s_bass.shape);
        draw_poly_outline(&layer, CX, CY, base_r, sides, ring_col, 2);
    }

    // Expanding rings
    for (auto &r : s_bass.rings) {
        if (!r.active) continue;

        r.radius  += 8.0f;
        r.opacity -= 12.0f;
        if (r.opacity <= 0.0f) { r.active = false; continue; }

        lv_opa_t opa = (lv_opa_t)r.opacity;
        int sides    = shape_sides(s_bass.shape);
        // 3px wide ring = 3 concentric outlines at r, r+1, r+2
        for (int d = 0; d < 3; d++) {
            lv_color_t rc = ring_col;
            draw_poly_outline(&layer, CX, CY, r.radius + d, sides, rc, 1);
            (void)opa;  // opacity via alpha blend not available in draw_line; use dim colour instead
        }
    }

    lv_canvas_finish_layer(canvas, &layer);
}

static void mode3_touch(lv_obj_t *, int, int)
{
    // Cycle shape on tap
    int next = ((int)s_bass.shape + 1) % (int)PulseShape::COUNT;
    s_bass.shape = (PulseShape)next;
}

static void mode3_deinit(lv_obj_t *) { s_bass = {}; }

// ── MODE 4: Terrain Wave ──────────────────────────────────────────────────────
// Scrolling heightmap from FFT bins, 3 depth layers.

struct Terrain4State {
    // Circular buffer of columns: terrain[col_idx][x_within_800]
    // Storing last 800 column heights, updated per tick
    float  heights[CW];        // current column to display
    int    bin_offset = 0;     // touch-controlled frequency offset ±128
    float  scroll_acc = 0.0f;  // sub-pixel scroll accumulator
};

static Terrain4State *s_terrain = nullptr;

static void mode4_init(lv_obj_t *canvas)
{
    if (!s_terrain) {
        s_terrain = new Terrain4State{};
    }
    memset(s_terrain->heights, 0, sizeof(s_terrain->heights));
    s_terrain->bin_offset = 0;
    s_terrain->scroll_acc = 0.0f;
    clear_canvas(canvas);
}

static void mode4_tick(lv_obj_t *canvas, uint32_t)
{
    snap_packet();
    if (!s_terrain) return;

    constexpr float BPM_DEFAULT  = 120.0f;
    float           scroll_speed = BPM_DEFAULT / 120.0f;  // px per tick

    s_terrain->scroll_acc += scroll_speed;
    int scroll_px = (int)s_terrain->scroll_acc;
    s_terrain->scroll_acc -= (float)scroll_px;
    if (scroll_px < 1) scroll_px = 1;

    // Build new column(s) from current FFT
    // Map screen X → FFT bin with optional offset
    for (int x = 0; x < CW; x++) {
        int bin_idx = x * 256 / CW + s_terrain->bin_offset;
        bin_idx = std::max(0, std::min(255, bin_idx));
        s_terrain->heights[x] = s_pkt.bins[bin_idx] * 160.0f;  // 0..160px
    }

    clear_canvas(canvas);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    // Draw 3 depth layers (back to front) using palette colors
    // Layer 2 (back): secondary dimmed, heights*0.35, y_offset=-80
    // Layer 1 (mid): lerp secondary↔primary, heights*0.6, y_offset=-40
    // Layer 0 (front): primary/accent, heights*1.0, y_offset=0
    const VisualPalette *pal4 = visuals_get_palette();

    struct LayerSpec { float h_scale; float bright; int y_off; };
    static const LayerSpec layers[3] = {
        { 0.35f, 0.30f, -80 },  // back
        { 0.60f, 0.60f, -40 },  // mid
        { 1.00f, 1.00f,   0 },  // front
    };

    // Per-layer colors derived from palette
    lv_color_t layer_colors[3] = {
        lv_color_mix(pal4->secondary, lv_color_hex(0x000000u), 80),   // back: very dim secondary
        lv_color_mix(pal4->primary,   pal4->secondary,          128),  // mid: blend
        lv_color_mix(pal4->accent,    pal4->primary,
                     (uint8_t)(g_visuals_mood * 255.0f)),              // front: accent↔primary by mood
    };

    int base_y = CH - THEME_FOOT_H - 10;  // bottom of terrain area

    for (int li = 2; li >= 0; li--) {
        const LayerSpec &L = layers[li];
        // Apply brightness scaling to the layer color
        lv_color_t col = lv_color_mix(layer_colors[li], lv_color_hex(0x000000u),
                                      (uint8_t)(L.bright * 255.0f));

        for (int x = 0; x < CW; x++) {
            float h = s_terrain->heights[x] * L.h_scale;
            int   y_top = base_y + L.y_off - (int)h;
            int   y_bot = base_y + L.y_off;
            if (y_top > y_bot) y_top = y_bot;

            lv_area_t col_area = {
                (lv_coord_t)x,
                (lv_coord_t)y_top,
                (lv_coord_t)x,
                (lv_coord_t)y_bot
            };
            lv_draw_rect_dsc_t dsc;
            lv_draw_rect_dsc_init(&dsc);
            dsc.bg_color = col;
            dsc.radius   = 0;
            lv_draw_rect(&layer, &dsc, &col_area);
        }
    }

    lv_canvas_finish_layer(canvas, &layer);
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

// ── Stub implementations for unimplemented modes (5-7, WP-C / WP-E) ──────────

static void mode_stub_init(lv_obj_t *c)   { clear_canvas(c); }
static void mode_stub_tick(lv_obj_t *, uint32_t) {}
static void mode_stub_touch(lv_obj_t *, int, int) {}
static void mode_stub_deinit(lv_obj_t *) {}

// ── Dispatch table ────────────────────────────────────────────────────────────

struct VisualMode {
    void (*init)(lv_obj_t *);
    void (*tick)(lv_obj_t *, uint32_t);
    void (*touch)(lv_obj_t *, int, int);
    void (*deinit)(lv_obj_t *);
};

static const VisualMode DISPATCH[8] = {
    { mode0_init,      mode0_tick,      mode0_touch,      mode0_deinit      },  // 0: Lissajous
    { mode1_init,      mode1_tick,      mode1_touch,      mode1_deinit      },  // 1: Circular FFT
    { mode2_init,      mode2_tick,      mode2_touch,      mode2_deinit      },  // 2: Aurora
    { mode3_init,      mode3_tick,      mode3_touch,      mode3_deinit      },  // 3: Bass Pulse
    { mode4_init,      mode4_tick,      mode4_touch,      mode4_deinit      },  // 4: Terrain
    { mode_stub_init,  mode_stub_tick,  mode_stub_touch,  mode_stub_deinit  },  // 5: Particles (WP-E)
    { mode_stub_init,  mode_stub_tick,  mode_stub_touch,  mode_stub_deinit  },  // 6: Game of Life (WP-E)
    { mode_stub_init,  mode_stub_tick,  mode_stub_touch,  mode_stub_deinit  },  // 7: Julia Set (WP-C)
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
