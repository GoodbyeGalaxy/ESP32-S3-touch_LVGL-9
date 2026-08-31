// metering_hub.cpp — Metering tile-picker hub + fullscreen dispatcher.
// Models visuals.cpp pattern: tile grid → fullscreen per meter.
// Swipe navigation: down from hub → home; left/right in fullscreen → cycle meters;
// down in fullscreen → back to hub.

#include "metering_hub.h"
#include "metering.h"
#include "meter_osc.h"
#include "meter_phase.h"
#include "meter_ms.h"
#include "meter_tuner.h"
#include "meter_gonio.h"
#include "meter_vu.h"
#include "meter_stereo.h"
#include "spectrum.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG __attribute__((unused)) = "meter_hub";

// ── Meter table ───────────────────────────────────────────────────────────────

using MeterFactory = lv_obj_t *(*)();

struct MeterDef {
    const char    *name;
    const char    *symbol;
    const char    *hint;
    uint32_t       tile_color;   // dark accent background for tile
    MeterFactory   factory;
};

static const MeterDef METERS[] = {
    { "LEVELS",    LV_SYMBOL_AUDIO,       "Peak / RMS / LUFS",       0x0D1A2A, metering_screen_create   },
    { "OSC",       LV_SYMBOL_REFRESH,     "Time-domain waveform",    0x0A1A0A, meter_osc_screen_create  },
    { "PHASE",     LV_SYMBOL_SHUFFLE,     "Correlation -1..+1",      0x1A0D00, meter_phase_screen_create},
    { "M/S",       LV_SYMBOL_VOLUME_MAX,  "Mid / Side balance",      0x1A001A, meter_ms_screen_create   },
    { "TUNER",     LV_SYMBOL_AUDIO,       "Chromatic bass tuner",    0x0A1A14, meter_tuner_screen_create},
    { "GONIOMETER",LV_SYMBOL_COPY,        "Lissajous L vs R",        0x001214, meter_gonio_screen_create},
    { "SPECTRUM",  LV_SYMBOL_SETTINGS,    "Bars/Curve/LED/Water/Oct",0x14140A, spectrum_screen_create   },
    { "VU",        LV_SYMBOL_VOLUME_MAX,  "Analog VU needle meter",  0x1A1200, meter_vu_screen_create   },
    { "STEREO",    LV_SYMBOL_SHUFFLE,     "Gonio + M/S + Phase",     0x001A14, meter_stereo_screen_create},
};

static constexpr int METER_COUNT = 9;

// ── State ─────────────────────────────────────────────────────────────────────

static int s_active_meter = 0;  // index of currently active fullscreen, or -1

// ── Stub fullscreen builder ───────────────────────────────────────────────────
// For Tier-2 meters that have no factory yet.

static lv_obj_t *build_stub_screen(const char *name)
{
    lv_obj_t *scr = theme_make_screen();
    statusbar_set_screen_name(name);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, "Coming Soon");
    lv_obj_set_style_text_color(lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_TITLE, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *sub = lv_label_create(scr);
    lv_obj_remove_style_all(sub);
    lv_label_set_text(sub, name);
    lv_obj_set_style_text_color(sub, THEME_ACCENT_DIM, 0);
    lv_obj_set_style_text_font(sub, THEME_FONT_HINT, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 10);

    foot_create(scr);

    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
        if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
        if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
    }, nullptr);

    return scr;
}

// ── Public API ────────────────────────────────────────────────────────────────

// Build a fullscreen for the given meter index.
// For index 0 (LEVELS / metering_screen_create), we detach that screen's built-in
// swipe handler and reattach a hub-aware one so left/right cycle meters and
// down returns to hub (instead of routing through nav_controller incorrectly).
static lv_obj_t *build_fullscreen_for_hub(int meter_idx)
{
    lv_obj_t *scr;
    if (METERS[meter_idx].factory) {
        scr = METERS[meter_idx].factory();
        // LEVELS uses nav_swipe instead of hub-aware navigation — detach and reattach.
        if (METERS[meter_idx].factory == metering_screen_create) {
            touch_nav_detach(scr);
            touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
                if (dir_h == -1) { lv_async_call([](void *){ metering_hub_next(); }, nullptr); return; }
                if (dir_h ==  1) { lv_async_call([](void *){ metering_hub_prev(); }, nullptr); return; }
                if (dir_v ==  1) { lv_async_call([](void *){ metering_hub_exit(); }, nullptr); return; }
            }, nullptr);
        }
    } else {
        scr = build_stub_screen(METERS[meter_idx].name);
    }
    return scr;
}

void metering_hub_enter(int meter_idx)
{
    if (meter_idx < 0 || meter_idx >= METER_COUNT) meter_idx = 0;
    s_active_meter = meter_idx;

    lv_obj_t *scr = build_fullscreen_for_hub(meter_idx);
    lv_indev_reset(NULL, NULL);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_TOP, 200, 0, true);
}

void metering_hub_exit()
{
    lv_obj_t *hub = metering_hub_screen_create();
    lv_indev_reset(NULL, NULL);
    lv_screen_load_anim(hub, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 200, 0, true);
}

void metering_hub_next()
{
    int next = (s_active_meter + 1) % METER_COUNT;
    s_active_meter = next;
    lv_obj_t *scr = build_fullscreen_for_hub(next);
    lv_indev_reset(NULL, NULL);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, true);
}

void metering_hub_prev()
{
    int prev = (s_active_meter - 1 + METER_COUNT) % METER_COUNT;
    s_active_meter = prev;
    lv_obj_t *scr = build_fullscreen_for_hub(prev);
    lv_indev_reset(NULL, NULL);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
}

// ── Tile-picker hub screen ────────────────────────────────────────────────────

lv_obj_t *metering_hub_screen_create()
{
    lv_obj_t *scr = theme_make_screen();
    statusbar_set_screen_name("METERS");

    // Grid: 3 cols × 4 rows = 12 slots, we use 10.
    // Content area: Y=32..424 (H=392). Foot at Y=424, H=56.
    // 4 rows of tiles with 8px margins and 8px vertical gaps.
    // TILE_H = (392 - 2*8 - 3*8) / 4 = (392 - 40) / 4 = 88
    constexpr int COLS    = 3;
    constexpr int ROWS    = 4;
    constexpr int GAP_H   = 8;
    constexpr int GAP_V   = 8;
    constexpr int MARGIN  = 8;
    constexpr int TILE_W  = (800 - 2 * MARGIN - (COLS - 1) * GAP_H) / COLS;  // ~258
    constexpr int TILE_H  = (THEME_CONTENT_H - 2 * MARGIN - (ROWS - 1) * GAP_V) / ROWS; // 120
    (void)ROWS;

    for (int i = 0; i < METER_COUNT; i++) {
        int col = i % COLS;
        int row = i / COLS;

        int x = MARGIN + col * (TILE_W + GAP_H);
        int y = THEME_CONTENT_Y + MARGIN + row * (TILE_H + GAP_V);

        lv_obj_t *tile = lv_obj_create(scr);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_pos(tile, x, y);
        lv_obj_set_style_bg_color(tile, lv_color_hex(METERS[i].tile_color), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, THEME_RADIUS, 0);

        bool is_stub = (METERS[i].factory == nullptr);
        lv_obj_set_style_border_color(tile, is_stub ? THEME_SEPARATOR : THEME_ACCENT, 0);
        lv_obj_set_style_border_width(tile, is_stub ? 1 : 1, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(tile, THEME_BG_CARD_HOVER, LV_STATE_PRESSED);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

        // Symbol (top-center) — small so it fits in 120px tile height
        lv_obj_t *sym = lv_label_create(tile);
        lv_obj_remove_style_all(sym);
        lv_label_set_text(sym, METERS[i].symbol);
        lv_obj_set_style_text_color(sym, is_stub ? THEME_TEXT_HINT : THEME_ACCENT_DIM, 0);
        lv_obj_set_style_text_font(sym, THEME_FONT_HINT, 0);
        lv_obj_align(sym, LV_ALIGN_TOP_MID, 0, 8);

        // Name — vertical center
        lv_obj_t *lbl = lv_label_create(tile);
        lv_obj_remove_style_all(lbl);
        lv_obj_set_size(lbl, TILE_W - 8, 22);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(lbl, METERS[i].name);
        lv_obj_set_style_text_color(lbl, is_stub ? THEME_TEXT_HINT : THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_LABEL, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 4);

        // Hint (bottom)
        lv_obj_t *hint = lv_label_create(tile);
        lv_obj_remove_style_all(hint);
        lv_obj_set_size(hint, TILE_W - 8, 16);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
        lv_label_set_text(hint, METERS[i].hint);
        lv_obj_set_style_text_color(hint, THEME_TEXT_HINT, 0);
        lv_obj_set_style_text_font(hint, THEME_FONT_HINT, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

        // Click handler — index stored in user_data
        lv_obj_add_event_cb(tile, [](lv_event_t *ev) {
            int idx = (int)(intptr_t)lv_event_get_user_data(ev);
            metering_hub_enter(idx);
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        if (!is_stub) {
            theme_apply_glow(tile);
        }
    }

    // Foot bar
    foot_create(scr);

    // 2D swipe: up → home, down → home (hub sits at row 0 in the grid,
    // swipe up from hub goes to home). Left/right handled by nav_controller.
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        nav_swipe(dir_h, dir_v);
    }, nullptr);

    return scr;
}
