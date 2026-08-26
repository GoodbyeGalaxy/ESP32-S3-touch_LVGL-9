#include "routing.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/foot.h"
#include "screens/statusbar.h"
#include "lvgl.h"
#include <cstdio>

// WING mixer integration mockup (Phase 6: OSC/UDP).
// Channel strip layout mirrors a typical 8-bus mixer view.
// Live fader data will arrive via OSC when Phase 6 is implemented.

// ── Layout ────────────────────────────────────────────────────────────────────

static constexpr int STRIP_COUNT  = 8;
static constexpr int STRIP_W      = 80;
static constexpr int STRIP_H      = 320;
static constexpr int STRIP_PAD_X  = 20;
static constexpr int STRIP_TOP_Y  = 52;
static constexpr int STRIP_GAP    = (800 - 2 * STRIP_PAD_X - STRIP_COUNT * STRIP_W)
                                    / (STRIP_COUNT - 1);  // ≈ 11px

static constexpr int FADER_W      = 12;
static constexpr int FADER_H      = 200;
static constexpr int FADER_OFF_X  = (STRIP_W - FADER_W) / 2;
static constexpr int FADER_OFF_Y  = 36;

// ── Channel definitions ───────────────────────────────────────────────────────

struct ChannelDef { const char *name; uint8_t init_val; uint32_t color_hex; };
static const ChannelDef CHANNELS[STRIP_COUNT] = {
    { "IN 1",  100, 0x3B82F6 },
    { "IN 2",  100, 0x3B82F6 },
    { "IN 3",   80, 0x3B82F6 },
    { "IN 4",   80, 0x3B82F6 },
    { "FX 1",   60, 0x8B5CF6 },
    { "FX 2",   60, 0x8B5CF6 },
    { "BUS L",  95, 0x22C55E },
    { "BUS R",  95, 0x22C55E },
};

// ── Navigation ────────────────────────────────────────────────────────────────

// IN: dir_h, dir_v from 2D swipe. OUT: delegates to nav_controller.
static void on_swipe(int dir_h, int dir_v, void * /*user_data*/)
{
    nav_swipe(dir_h, dir_v);
}

// ── Channel strip builder ─────────────────────────────────────────────────────

// IN: parent, channel def, strip x position. OUT: creates strip with fader + label on parent.
// All colors use theme or ≥38% luminance (IPS rule).
static void create_channel_strip(lv_obj_t *parent, const ChannelDef &ch, int x)
{
    lv_obj_t *strip = lv_obj_create(parent);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, STRIP_W, STRIP_H);
    lv_obj_set_pos(strip, x, STRIP_TOP_Y);
    lv_obj_set_style_bg_color(strip, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strip, 6, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    theme_apply_glow(strip);

    // Level bar (vertical)
    lv_obj_t *bar = lv_bar_create(strip);
    lv_obj_set_size(bar, FADER_W, FADER_H);
    lv_obj_set_pos(bar, FADER_OFF_X, FADER_OFF_Y);
    lv_bar_set_mode(bar, LV_BAR_MODE_NORMAL);
    lv_bar_set_range(bar, 0, 127);
    lv_bar_set_value(bar, ch.init_val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x606060), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(ch.color_hex), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    // Channel name
    lv_obj_t *lbl = lv_label_create(strip);
    lv_label_set_text(lbl, ch.name);
    lv_obj_set_style_text_color(lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ── Screen entry point ────────────────────────────────────────────────────────

// IN: nothing. OUT: new screen lv_obj_t* (not loaded — caller calls lv_screen_load/anim).
lv_obj_t *routing_screen_create()
{
    lv_obj_t *scr = theme_make_screen();

    statusbar_set_screen_name("ROUTING");

    // Title + connection status
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ROUTING");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LABEL, 0);
    lv_obj_set_pos(title, STRIP_PAD_X, 8);

    lv_obj_t *conn = lv_label_create(scr);
    lv_label_set_text(conn, "WING  Disconnected");
    lv_obj_set_style_text_color(conn, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(conn, THEME_FONT_HINT, 0);
    lv_obj_set_pos(conn, 800 - 200, 11);

    // 8 channel strips
    for (int i = 0; i < STRIP_COUNT; ++i) {
        int x = STRIP_PAD_X + i * (STRIP_W + STRIP_GAP);
        create_channel_strip(scr, CHANNELS[i], x);
    }

    foot_create(scr);

    touch_nav_attach_2d(scr, on_swipe, nullptr);

    return scr;
}
