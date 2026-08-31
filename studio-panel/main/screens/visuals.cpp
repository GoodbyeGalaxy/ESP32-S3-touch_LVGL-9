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

// ── Palette definitions ───────────────────────────────────────────────────────

static const VisualPalette k_palettes[] = {
    {"PHOSPHOR", lv_color_hex(0x00E5FF), lv_color_hex(0x00838F), lv_color_hex(0x69FF47), lv_color_hex(0x000A08)},
    {"NOIR",     lv_color_hex(0xE0E0E0), lv_color_hex(0x505050), lv_color_hex(0xFFFFFF), lv_color_hex(0x080808)},
    {"SYNTHWAVE",lv_color_hex(0xFF0080), lv_color_hex(0x6A0080), lv_color_hex(0x00FFFF), lv_color_hex(0x0A0010)},
    {"SOLAR",    lv_color_hex(0xFFB300), lv_color_hex(0x5D3200), lv_color_hex(0xFF6D00), lv_color_hex(0x0A0600)},
    {"ARCTIC",   lv_color_hex(0x80D8FF), lv_color_hex(0x003D5C), lv_color_hex(0xE0F7FF), lv_color_hex(0x00080A)},
};
static constexpr int k_palette_count = 5;
static int s_palette_index = 0;

// IN: nothing. OUT: pointer to currently active VisualPalette (never null).
const VisualPalette *visuals_get_palette()
{
    return &k_palettes[s_palette_index];
}

// ── NVS ───────────────────────────────────────────────────────────────────────

static constexpr char NVS_NS[]        = "visuals";
static constexpr char NVS_KEY[]       = "mode";
static constexpr char NVS_KEY_PAL[]   = "palette";

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

static lv_obj_t *s_palette_dot = nullptr;   // color dot on PALETTE pill (set in build_fullscreen)

// IN: palette_index 0..4. OUT: nothing. Persists to NVS.
static void save_palette(int idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_PAL, (uint8_t)idx);
        nvs_commit(h);
        nvs_close(h);
    }
}

// IN: nothing. OUT: saved palette index, or 0 if not found.
static int load_palette()
{
    nvs_handle_t h;
    uint8_t val = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_PAL, &val);
        nvs_close(h);
    }
    return val < (uint8_t)k_palette_count ? (int)val : 0;
}

// IN: index 0..4. OUT: nothing. Persists to NVS, updates palette dot if visible.
void visuals_set_palette(int index)
{
    if (index < 0 || index >= k_palette_count) return;
    s_palette_index = index;
    save_palette(index);
    // Update the dot on the PALETTE pill if the fullscreen is currently shown
    if (s_palette_dot) {
        lv_obj_set_style_bg_color(s_palette_dot,
            k_palettes[s_palette_index].primary, 0);
    }
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
static lv_obj_t   *s_demo_btn_lbl   = nullptr;  // label on DEMO pill button

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

// ── Palette overlay ───────────────────────────────────────────────────────────
// Semi-transparent panel, direct child of fullscreen screen.
// Tap on a palette pill → visuals_set_palette() + destroy overlay.
// Tap on overlay background → destroy overlay (no change).

// User-data struct carried by each palette pill button.
struct PillUD { lv_obj_t *overlay; int index; };

static void destroy_overlay(lv_event_t *e)
{
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    if (!overlay || !lv_obj_is_valid(overlay)) return;

    // Free PillUD structs on pill children before LVGL destroys them
    uint32_t child_cnt = lv_obj_get_child_count(overlay);
    for (uint32_t ci = 0; ci < child_cnt; ci++) {
        lv_obj_t *child = lv_obj_get_child(overlay, ci);
        void *cud = lv_obj_get_user_data(child);
        if (cud) {
            delete static_cast<PillUD *>(cud);
            lv_obj_set_user_data(child, nullptr);
        }
    }
    lv_obj_delete(overlay);
    if (s_render_timer) lv_timer_resume(s_render_timer);
}

static void on_palette_pill_click(lv_event_t *e)
{
    PillUD *ud = static_cast<PillUD *>(lv_event_get_user_data(e));
    if (!ud) return;

    int       idx     = ud->index;
    lv_obj_t *overlay = ud->overlay;

    // Set palette (updates dot, saves to NVS)
    visuals_set_palette(idx);

    if (s_render_timer) lv_timer_resume(s_render_timer);

    // Destroy overlay — also frees all children (and their user_data structs
    // are heap-allocated: clean up before delete)
    if (overlay && lv_obj_is_valid(overlay)) {
        // Free all pill user_data structs before destroying the parent
        uint32_t child_cnt = lv_obj_get_child_count(overlay);
        for (uint32_t ci = 0; ci < child_cnt; ci++) {
            lv_obj_t *child = lv_obj_get_child(overlay, ci);
            void *cud = lv_obj_get_user_data(child);
            if (cud) {
                delete static_cast<PillUD *>(cud);
                lv_obj_set_user_data(child, nullptr);
            }
        }
        lv_obj_delete(overlay);
    }
}

static void on_palette_btn(lv_event_t *e)
{
    lv_obj_t *screen = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
    if (!screen) return;

    // Pause render timer while overlay is open — prevents PSRAM canvas
    // invalidation from fighting LVGL compositing (causes visible stall).
    if (s_render_timer) lv_timer_pause(s_render_timer);

    // Overlay container — 800×90px, vertically above foot bar by 8px
    constexpr int OVL_H  = 90;
    constexpr int OVL_Y  = 480 - THEME_FOOT_H - OVL_H - 8;

    lv_obj_t *overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 800, OVL_H);
    lv_obj_set_pos(overlay, 0, OVL_Y);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x111111u), 0);
    lv_obj_set_style_bg_opa(overlay, (lv_opa_t)230, 0);
    lv_obj_set_style_radius(overlay, 12, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    // Tap on overlay background → close without change
    lv_obj_add_event_cb(overlay, destroy_overlay, LV_EVENT_CLICKED, overlay);

    // 5 palette pills: 120×52px, 10px gap, centered inside overlay
    constexpr int  PPILL_W   = 120;
    constexpr int  PPILL_H   = 52;
    constexpr int  PPILL_GAP = 10;
    constexpr int  TOTAL_W   = k_palette_count * PPILL_W + (k_palette_count - 1) * PPILL_GAP;
    int            start_x   = (800 - TOTAL_W) / 2;
    int            start_y   = (OVL_H - PPILL_H) / 2;

    for (int i = 0; i < k_palette_count; i++) {
        const VisualPalette &pal = k_palettes[i];
        bool active = (i == s_palette_index);

        int pill_x = start_x + i * (PPILL_W + PPILL_GAP);

        lv_obj_t *pill = lv_obj_create(overlay);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, PPILL_W, PPILL_H);
        lv_obj_set_pos(pill, pill_x, start_y);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

        // BG: dimmed primary (mix primary 25% with black)
        lv_color_t dim_bg = lv_color_mix(pal.primary, lv_color_hex(0x000000u), 64);
        lv_obj_set_style_bg_color(pill, dim_bg, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pill, 8, 0);

        // Border: active = 3px white, inactive = 2px primary
        lv_obj_set_style_border_width(pill, active ? 3 : 2, 0);
        lv_obj_set_style_border_color(pill,
            active ? lv_color_hex(0xFFFFFFu) : pal.primary, 0);
        lv_obj_set_style_border_opa(pill, LV_OPA_COVER, 0);

        // Name label (upper area)
        lv_obj_t *name_lbl = lv_label_create(pill);
        lv_obj_remove_style_all(name_lbl);
        lv_label_set_text(name_lbl, pal.name);
        lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xFFFFFFu), 0);
        lv_obj_set_style_text_font(name_lbl, THEME_FONT_HINT, 0);
        lv_obj_set_size(name_lbl, PPILL_W - 4, 18);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(name_lbl, LV_ALIGN_TOP_MID, 0, 6);

        // Accent band (8px high, bottom of pill) in accent color
        lv_obj_t *band = lv_obj_create(pill);
        lv_obj_remove_style_all(band);
        lv_obj_set_size(band, PPILL_W, 8);
        lv_obj_align(band, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(band, pal.accent, 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(band, 0, 0);

        // Click on pill — stop propagation to overlay background
        lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);

        PillUD *ud = new PillUD{overlay, i};
        lv_obj_set_user_data(pill, ud);
        lv_obj_add_event_cb(pill, [](lv_event_t *ev) {
            // Stop propagation so overlay-background click handler doesn't fire
            lv_event_stop_bubbling(ev);
        }, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(pill, on_palette_pill_click, LV_EVENT_CLICKED, ud);
    }
}

// ── Render timer (fullscreen) ─────────────────────────────────────────────────

// Per-screen data stored in screen user_data and timer user_data.
// Keeps canvas + mode_idx bound to the screen that created them so that
// on_fs_delete and visuals_render_tick remain correct during mode transitions.
struct FsData {
    lv_timer_t *timer;
    lv_obj_t   *canvas;
    int         mode_idx;
};

// IN: lv_timer_t* user_data = FsData*. OUT: nothing.
void visuals_render_tick(lv_timer_t *t)
{
    FsData *fd = static_cast<FsData *>(lv_timer_get_user_data(t));
    if (!fd) return;

    AudioPacket pkt{};
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) {
        update_mood(pkt);
    }

    uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
    visuals_mode_tick(fd->mode_idx, fd->canvas, t_ms);
}

// ── Fullscreen screen builder ─────────────────────────────────────────────────

static lv_obj_t *build_fullscreen(int mode_idx)
{
    // Load palette from NVS once (first fullscreen build only)
    static bool s_palette_loaded = false;
    if (!s_palette_loaded) {
        s_palette_index  = load_palette();
        s_palette_loaded = true;
    }

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
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    if (s_canvas_buf) {
        lv_canvas_set_buffer(canvas, s_canvas_buf,
                             CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_ARGB8888);
        // Direct PSRAM fill — avoids LVGL draw task allocation at init
        lv_color32_t bg = { .blue=0x0A, .green=0x0A, .red=0x0A, .alpha=0xFF };
        for (int i = 0; i < CANVAS_W * CANVAS_H; i++) s_canvas_buf[i] = bg;
        lv_obj_invalidate(canvas);
    }
    s_canvas = canvas;

    // Foot bar — capture right_zone for DEMO + PALETTE buttons
    lv_obj_t *right_zone = foot_create(scr);

    // PALETTE button (96×40) — rightmost in foot bar
    lv_obj_t *pal_btn = lv_btn_create(right_zone);
    lv_obj_remove_style_all(pal_btn);
    lv_obj_set_size(pal_btn, 96, 40);
    lv_obj_align(pal_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(pal_btn, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(pal_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pal_btn, 14, 0);
    lv_obj_set_style_border_width(pal_btn, 0, 0);
    lv_obj_add_flag(pal_btn, LV_OBJ_FLAG_CLICKABLE);

    // Color dot (8px circle) inside PALETTE — shows active palette's primary color
    lv_obj_t *dot = lv_obj_create(pal_btn);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_bg_color(dot, k_palettes[s_palette_index].primary, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 10, 0);
    s_palette_dot = dot;

    lv_obj_t *pal_lbl = lv_label_create(pal_btn);
    lv_obj_remove_style_all(pal_lbl);
    lv_label_set_text(pal_lbl, "PALETTE");
    lv_obj_set_style_text_color(pal_lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(pal_lbl, THEME_FONT_HINT, 0);
    lv_obj_align(pal_lbl, LV_ALIGN_RIGHT_MID, -8, 0);

    lv_obj_add_event_cb(pal_btn, on_palette_btn, LV_EVENT_CLICKED, scr);

    // DEMO button (76×40) — left of PALETTE (-8 margin -96 palette -8 gap = -112)
    lv_obj_t *demo_btn = lv_btn_create(right_zone);
    lv_obj_remove_style_all(demo_btn);
    lv_obj_set_size(demo_btn, 76, 40);
    lv_obj_align(demo_btn, LV_ALIGN_RIGHT_MID, -112, 0);
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

    // 2D swipe navigation — deferred via lv_async_call so the screen transition
    // runs after the swipe callback returns. Without deferral, auto_del=true in
    // lv_screen_load_anim deletes the current screen synchronously, freeing
    // Swipe2DState while the event callback still holds a pointer to it (UAF).
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void*){ visuals_mode_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void*){ visuals_mode_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void*){ visuals_fullscreen_exit(); }, nullptr); return; }
    }, nullptr);

    // Initialise mode renderer
    visuals_mode_init(mode_idx, canvas);

    // Render timer at ~30 Hz — FsData binds this timer to its canvas+mode so
    // on_fs_delete and visuals_render_tick stay correct during mode transitions.
    FsData *fsd     = new FsData{ nullptr, canvas, mode_idx };
    s_render_timer  = lv_timer_create(visuals_render_tick, 50, fsd);  // 20Hz
    fsd->timer      = s_render_timer;
    lv_obj_set_user_data(scr, fsd);

    return scr;
}

static void on_fs_delete(lv_event_t *e)
{
    lv_obj_t *scr = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    FsData   *fsd = static_cast<FsData *>(lv_obj_get_user_data(scr));
    if (fsd) {
        if (fsd->timer) lv_timer_delete(fsd->timer);
        visuals_mode_deinit(fsd->mode_idx, fsd->canvas);
        delete fsd;
        lv_obj_set_user_data(scr, nullptr);
    }
    // Only clear global pointers if this is still the current screen
    // (during a mode switch, a new screen has already updated s_fs_scr).
    if (scr == s_fs_scr) {
        s_fs_scr       = nullptr;
        s_canvas       = nullptr;
        s_render_timer = nullptr;
        s_demo_btn_lbl = nullptr;
        s_palette_dot  = nullptr;
        ESP_LOGI(TAG, "fullscreen deleted (mode %d)", s_active_mode);
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void visuals_fullscreen_enter(int mode_idx)
{
    // Pause old render timer immediately — prevents it from firing again and
    // overwriting s_canvas_buf after build_fullscreen initialises it for the new mode.
    // on_fs_delete will lv_timer_delete it safely once the old screen is freed.
    if (s_render_timer) lv_timer_pause(s_render_timer);

    if (mode_idx < 0 || mode_idx >= MODE_COUNT) mode_idx = 0;
    s_active_mode = mode_idx;
    save_mode(mode_idx);

    lv_obj_t *scr = build_fullscreen(mode_idx);
    lv_obj_add_event_cb(scr, on_fs_delete, LV_EVENT_DELETE, nullptr);
    s_fs_scr = scr;
    lv_indev_reset(NULL, NULL);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

void visuals_fullscreen_exit()
{
    lv_obj_t *picker = visuals_screen_create();
    lv_indev_reset(NULL, NULL);
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
