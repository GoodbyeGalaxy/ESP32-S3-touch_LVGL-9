// on_air.cpp — On Air indicator with pulsating red circle.
// State (s_on_air) persists across screen navigations.
// Toggle via foot button. Circle pulses when active; dims when standby.

#include "on_air.h"
#include "theme.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/statusbar.h"
#include "screens/foot.h"
#include "lvgl.h"

// ── Persistent state ───────────────────────────────────────────────────────────

static bool s_on_air = false;

// ── Per-screen state ───────────────────────────────────────────────────────────

struct OnAirState {
    lv_obj_t *circle   = nullptr;
    lv_obj_t *lbl_main = nullptr;  // "ON AIR" / "STANDBY"
    lv_obj_t *lbl_hint = nullptr;
};

// ── Animation ──────────────────────────────────────────────────────────────────

static void anim_opa_cb(void *var, int32_t val)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)val, 0);
}

static void start_pulse(lv_obj_t *circle)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, circle);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_50);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_playback_duration(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void stop_pulse(lv_obj_t *circle)
{
    lv_anim_delete(circle, anim_opa_cb);
    lv_obj_set_style_opa(circle, LV_OPA_COVER, 0);
}

// ── Display update ─────────────────────────────────────────────────────────────

static void update_display(OnAirState *st)
{
    if (s_on_air) {
        lv_obj_set_style_bg_color(st->circle, lv_color_hex(0xDC2626), 0);  // red-600
        lv_label_set_text(st->lbl_main, "ON AIR");
        lv_obj_set_style_text_color(st->lbl_main, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(st->lbl_hint, "tap to go off air");
    } else {
        lv_obj_set_style_bg_color(st->circle, lv_color_hex(0x1C1C1C), 0);  // dark
        lv_label_set_text(st->lbl_main, "STANDBY");
        lv_obj_set_style_text_color(st->lbl_main, THEME_TEXT_HINT, 0);
        lv_label_set_text(st->lbl_hint, "tap to go on air");
    }
}

// ── Event handlers ─────────────────────────────────────────────────────────────

static void on_toggle(lv_event_t *e)
{
    auto *st = static_cast<OnAirState *>(lv_event_get_user_data(e));
    s_on_air = !s_on_air;
    update_display(st);
    if (s_on_air) start_pulse(st->circle);
    else          stop_pulse(st->circle);
}

static void on_delete(lv_event_t *e)
{
    // LVGL auto-deletes animations when circle is deleted — no manual cleanup needed.
    auto *st = static_cast<OnAirState *>(lv_event_get_user_data(e));
    delete st;
}

// ── Screen creation ────────────────────────────────────────────────────────────

lv_obj_t *on_air_screen_create()
{
    auto *st = new OnAirState{};

    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, st);
    statusbar_set_screen_name("ON AIR");

    // ── Circle indicator ──────────────────────────────────────────────────────
    // Diameter 220px, centered horizontally, below statusbar
    static constexpr int DIAM   = 220;
    static constexpr int CIR_X  = (800 - DIAM) / 2;   // 290
    static constexpr int CIR_Y  = THEME_CONTENT_Y + 30; // 62

    lv_obj_t *circle = lv_obj_create(scr);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, DIAM, DIAM);
    lv_obj_set_pos(circle, CIR_X, CIR_Y);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(circle, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(circle, LV_OBJ_FLAG_CLICKABLE);
    st->circle = circle;

    // ── Status text: centered below circle ───────────────────────────────────
    static constexpr int TXT_Y = CIR_Y + DIAM + 20;  // 312

    lv_obj_t *lbl_main = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_main);
    lv_label_set_text(lbl_main, "STANDBY");
    lv_obj_set_style_text_color(lbl_main, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl_main, &lv_font_montserrat_36, 0);
    lv_obj_set_pos(lbl_main, 0, TXT_Y);
    lv_obj_set_width(lbl_main, 800);
    lv_obj_set_style_text_align(lbl_main, LV_TEXT_ALIGN_CENTER, 0);
    st->lbl_main = lbl_main;

    lv_obj_t *lbl_hint = lv_label_create(scr);
    lv_obj_remove_style_all(lbl_hint);
    lv_label_set_text(lbl_hint, "tap to go on air");
    lv_obj_set_style_text_color(lbl_hint, lv_color_hex(0x505050), 0);
    lv_obj_set_style_text_font(lbl_hint, THEME_FONT_HINT, 0);
    lv_obj_set_pos(lbl_hint, 0, TXT_Y + 44);
    lv_obj_set_width(lbl_hint, 800);
    lv_obj_set_style_text_align(lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
    st->lbl_hint = lbl_hint;

    // ── Foot bar with toggle button ───────────────────────────────────────────
    lv_obj_t *right_zone = foot_create(scr);

    lv_obj_t *tog = lv_btn_create(right_zone);
    lv_obj_remove_style_all(tog);
    lv_obj_set_size(tog, 148, 36);
    lv_obj_align(tog, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(tog, lv_color_hex(0xDC2626), 0);
    lv_obj_set_style_bg_color(tog, lv_color_hex(0xB91C1C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(tog, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tog, THEME_RADIUS, 0);
    lv_obj_add_event_cb(tog, on_toggle, LV_EVENT_CLICKED, st);
    {
        lv_obj_t *tl = lv_label_create(tog);
        lv_obj_remove_style_all(tl);
        lv_label_set_text(tl, "TOGGLE ON AIR");
        lv_obj_set_style_text_color(tl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(tl, THEME_FONT_HINT, 0);
        lv_obj_center(tl);
    }

    // ── Nav swipe ─────────────────────────────────────────────────────────────
    touch_nav_attach_2d(scr, [](int dir_h, int dir_v, void *) {
        nav_swipe(dir_h, dir_v);
    }, nullptr);

    // Restore state from previous visit
    update_display(st);
    if (s_on_air) start_pulse(st->circle);

    return scr;
}
