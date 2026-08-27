// visuals.cpp — Visuals Tile-Picker + Fullscreen infrastructure.
// Layout: Statusbar (32px) | Content 4×2 tile grid | Foot (56px).
// Fullscreen: 800×480 canvas, DEMO pill button bottom-right, 2D swipe navigation.

#include "visuals.h"
#include "visuals_modes.h"
#include "audio_data.h"
#include "demo_signal.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <cmath>
#include <cstring>

static const char *TAG = "visuals";

// ── NVS ───────────────────────────────────────────────────────────────────────

static constexpr char NVS_NS[]  = "visuals";
static constexpr char NVS_KEY[] = "mode";

// IN: mode_index 0..7. OUT: nothing. Persists to NVS.
static void save_mode(int idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, (uint8_t)idx);
        nvs_commit(h);
        nvs_close(h);
    }
}

// IN: nothing. OUT: saved mode index, or 0 if not found.
static int load_mode()
{
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &val);
        nvs_close(h);
    }
    return val < 8 ? (int)val : 0;
}

// ── Mode table ────────────────────────────────────────────────────────────────

static constexpr int MODE_COUNT = 8;

struct ModeDef {
    const char *name;
    uint32_t    tile_color;   // hex, distinct dark tone per mode
    const char *symbol;
};

static const ModeDef MODES[MODE_COUNT] = {
    { "LISSAJOUS",    0x1A1A2E, LV_SYMBOL_AUDIO       },
    { "CIRC FFT",     0x16213E, LV_SYMBOL_SHUFFLE      },
    { "AURORA",       0x0F3460, LV_SYMBOL_IMAGE        },
    { "BASS PULSE",   0x1B1B2F, LV_SYMBOL_VOLUME_MAX   },
    { "TERRAIN",      0x162032, LV_SYMBOL_EYE_OPEN     },
    { "PARTICLES",    0x1A1529, LV_SYMBOL_DRIVE        },
    { "GAME OF LIFE", 0x141E2B, LV_SYMBOL_REFRESH      },
    { "JULIA SET",    0x0D1B2A, LV_SYMBOL_LOOP         },
};

// ── Fullscreen state ──────────────────────────────────────────────────────────

static int         s_active_mode = 0;
static lv_obj_t   *s_fs_scr     = nullptr;  // current fullscreen screen
static lv_obj_t   *s_canvas     = nullptr;
static lv_timer_t *s_render_timer = nullptr;
static lv_obj_t   *s_demo_btn_lbl = nullptr;  // label on DEMO pill button

// Per-mode PSRAM canvas buffer (800×480×4 bytes = 1.5 MB) — allocated once,
// reused across mode switches to avoid repeated heap churn.
static lv_color32_t *s_canvas_buf = nullptr;
static constexpr int CANVAS_W = 800;
static constexpr int CANVAS_H = 480;

// ── Mood score ────────────────────────────────────────────────────────────────

// Exposed to visuals_modes.cpp via visuals_modes.h
float g_visuals_mood = 0.5f;

// IN: latest AudioPacket. OUT: updates g_visuals_mood.
// Mood 0=dark/bass-heavy, 1=bright/treble-rich. Lerps slowly.
static void update_mood(const AudioPacket &pkt)
{
    float weighted = 0.0f, total = 0.0f;
    for (int i = 0; i < 256; ++i) {
        weighted += (float)i * pkt.bins[i];
        total    += pkt.bins[i];
    }
    float centroid = (total > 0.01f) ? (weighted / total / 255.0f) : 0.5f;
    float loudness = fmaxf(0.0f, fminf(1.0f, (pkt.momentary + 60.0f) / 60.0f));
    float target   = centroid * 0.7f + loudness * 0.3f;
    float alpha    = 0.01f + loudness * 0.05f;
    g_visuals_mood += (target - g_visuals_mood) * alpha;
}

// ── DEMO pill button ──────────────────────────────────────────────────────────

static void refresh_demo_btn()
{
    if (!s_demo_btn_lbl) return;
    bool active = demo_signal_is_active() && demo_signal_is_forced_by_user();
    lv_label_set_text(s_demo_btn_lbl, active ? "DEMO " LV_SYMBOL_PLAY : "DEMO");
    lv_obj_t *btn = lv_obj_get_parent(s_demo_btn_lbl);
    if (!btn) return;
    lv_obj_set_style_bg_color(btn,
        active ? lv_color_hex(0xE65100u) : THEME_BG_CARD, 0);
    lv_obj_set_style_text_color(s_demo_btn_lbl,
        active ? lv_color_hex(0xFFFFFFu) : THEME_TEXT_HINT, 0);
}

static void on_demo_btn(lv_event_t *e)
{
    (void)e;
    bool now = !demo_signal_is_forced_by_user();
    demo_signal_set_forced(now);
    visuals_modes_set_demo_forced(now);
    refresh_demo_btn();
}

// ── Render timer (fullscreen) ─────────────────────────────────────────────────

// IN: lv_timer_t* user_data = canvas lv_obj_t*. OUT: nothing.
void visuals_render_tick(lv_timer_t *t)
{
    lv_obj_t *canvas = static_cast<lv_obj_t *>(lv_timer_get_user_data(t));
    if (!canvas) return;

    AudioPacket pkt{};
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) {
        update_mood(pkt);
    }

    uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    visuals_mode_tick(s_active_mode, canvas, t_ms);
}

// ── Fullscreen screen builder ─────────────────────────────────────────────────

static lv_obj_t *build_fullscreen(int mode_idx)
{
    lv_obj_t *scr = theme_make_screen();

    statusbar_set_screen_name(MODES[mode_idx].name);

    // Allocate PSRAM canvas buffer once
    if (!s_canvas_buf) {
        s_canvas_buf = static_cast<lv_color32_t *>(
            heap_caps_malloc(CANVAS_W * CANVAS_H * sizeof(lv_color32_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_canvas_buf) {
            ESP_LOGE(TAG, "Failed to allocate canvas buffer in PSRAM");
        }
    }

    // Canvas — covers the full 800×480 screen (behind statusbar, which is on lv_layer_top)
    lv_obj_t *canvas = lv_canvas_create(scr);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, CANVAS_W, CANVAS_H);
    if (s_canvas_buf) {
        lv_canvas_set_buffer(canvas, s_canvas_buf,
                             CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_ARGB8888);
        lv_canvas_fill_bg(canvas, lv_color_hex(0x0A0A0Au), LV_OPA_COVER);
    }
    s_canvas = canvas;

    // Foot bar
    foot_create(scr);

    // DEMO pill button — 80×28px, bottom-right, 12px margin, radius 14
    // Placed on scr directly so it floats above canvas
    lv_obj_t *demo_btn = lv_btn_create(scr);
    lv_obj_remove_style_all(demo_btn);
    lv_obj_set_size(demo_btn, 80, 28);
    lv_obj_set_pos(demo_btn,
                   800 - 80 - 12,
                   480 - THEME_FOOT_H - 28 - 12);
    lv_obj_set_style_bg_color(demo_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(demo_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(demo_btn, 14, 0);
    lv_obj_set_style_border_width(demo_btn, 0, 0);
    lv_obj_add_flag(demo_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(demo_btn, on_demo_btn, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *demo_lbl = lv_label_create(demo_btn);
    lv_obj_remove_style_all(demo_lbl);
    lv_label_set_text(demo_lbl, "DEMO");
    lv_obj_set_style_text_color(demo_lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(demo_lbl, THEME_FONT_HINT, 0);
    lv_obj_center(demo_lbl);
    s_demo_btn_lbl = demo_lbl;
    refresh_demo_btn();

    // 2D swipe navigation in fullscreen
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { visuals_mode_next(); return; }
        if (dir_h ==  1) { visuals_mode_prev(); return; }
        if (dir_v ==  1) { visuals_fullscreen_exit(); return; }
    }, nullptr);

    // Initialise mode renderer
    visuals_mode_init(mode_idx, canvas);

    // Render timer at ~30 Hz
    s_render_timer = lv_timer_create(visuals_render_tick, 33, canvas);

    return scr;
}

static void on_fs_delete(lv_event_t *e)
{
    (void)e;
    if (s_render_timer) {
        lv_timer_delete(s_render_timer);
        s_render_timer = nullptr;
    }
    // Deinit active mode renderer
    if (s_canvas) {
        visuals_mode_deinit(s_active_mode, s_canvas);
    }
    s_fs_scr       = nullptr;
    s_canvas       = nullptr;
    s_demo_btn_lbl = nullptr;
    ESP_LOGI(TAG, "fullscreen deleted (mode %d)", s_active_mode);
}

// ── Public API ────────────────────────────────────────────────────────────────

void visuals_fullscreen_enter(int mode_idx)
{
    if (mode_idx < 0 || mode_idx >= MODE_COUNT) mode_idx = 0;
    s_active_mode = mode_idx;
    save_mode(mode_idx);

    lv_obj_t *scr = build_fullscreen(mode_idx);
    lv_obj_add_event_cb(scr, on_fs_delete, LV_EVENT_DELETE, nullptr);
    s_fs_scr = scr;
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, true);
}

void visuals_fullscreen_exit()
{
    lv_obj_t *picker = visuals_screen_create();
    lv_screen_load_anim(picker, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 200, 0, true);
}

void visuals_mode_next()
{
    int next = (s_active_mode + 1) % MODE_COUNT;
    visuals_fullscreen_enter(next);
}

void visuals_mode_prev()
{
    int prev = (s_active_mode - 1 + MODE_COUNT) % MODE_COUNT;
    visuals_fullscreen_enter(prev);
}

// ── Tile-picker screen ────────────────────────────────────────────────────────

lv_obj_t *visuals_screen_create()
{
    lv_obj_t *scr = theme_make_screen();
    statusbar_set_screen_name("VISUALS");

    // 4 cols × 2 rows grid filling content area (Y=32..424, H=392)
    // Content area: 800×392, minus 4px top/bottom margin.
    // Tile size: (800 - 5*gap) / 4 cols × (392 - 3*gap) / 2 rows
    constexpr int COLS    = 4;
    constexpr int ROWS    = 2;
    constexpr int GAP_H   = 6;   // horizontal gap
    constexpr int GAP_V   = 6;   // vertical gap
    constexpr int MARGIN  = 6;   // outer margin left/right/top
    constexpr int TILE_W  = (800 - 2 * MARGIN - (COLS - 1) * GAP_H) / COLS;   // 188
    constexpr int TILE_H  = (THEME_CONTENT_H - 2 * MARGIN - (ROWS - 1) * GAP_V) / ROWS; // 190

    int last_mode = load_mode();

    for (int i = 0; i < MODE_COUNT; i++) {
        int col = i % COLS;
        int row = i / COLS;

        int x = MARGIN + col * (TILE_W + GAP_H);
        int y = THEME_CONTENT_Y + MARGIN + row * (TILE_H + GAP_V);

        lv_obj_t *tile = lv_obj_create(scr);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_style_bg_color(tile, lv_color_hex(MODES[i].tile_color), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, THEME_RADIUS, 0);
        lv_obj_set_style_border_color(tile,
            (i == last_mode) ? THEME_ACCENT : THEME_SEPARATOR, 0);
        lv_obj_set_style_border_width(tile, (i == last_mode) ? 2 : 1, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(tile, THEME_BG_CARD_HOVER, LV_STATE_PRESSED);

        // Symbol — upper-center
        lv_obj_t *sym = lv_label_create(tile);
        lv_obj_remove_style_all(sym);
        lv_label_set_text(sym, MODES[i].symbol);
        lv_obj_set_style_text_color(sym, THEME_ACCENT_DIM, 0);
        lv_obj_set_style_text_font(sym, THEME_FONT_HINT, 0);
        lv_obj_align(sym, LV_ALIGN_TOP_MID, 0, 10);

        // Mode name — centered
        lv_obj_t *lbl = lv_label_create(tile);
        lv_obj_remove_style_all(lbl);
        lv_obj_set_size(lbl, TILE_W - 8, 20);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(lbl, MODES[i].name);
        lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 8);

        // Click handler — store index as integer in user_data
        lv_obj_add_event_cb(tile, [](lv_event_t *ev) {
            int idx = (int)(intptr_t)lv_event_get_user_data(ev);
            visuals_fullscreen_enter(idx);
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    // Foot bar
    foot_create(scr);

    // 2D swipe — routes through nav_controller for screen-to-screen navigation
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        nav_swipe(dir_h, dir_v);
    }, nullptr);

    return scr;
}
