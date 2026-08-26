#include "studio_one.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/foot.h"
#include "screens/statusbar.h"
#include "lvgl.h"

// Transport/session display for Studio One DAW integration (Phase 5: WebSocket).
// Currently shows a static mockup; live data arrives via WebSocket when implemented.

// ── Navigation ────────────────────────────────────────────────────────────────

// IN: dir_h, dir_v from 2D swipe. OUT: delegates to nav_controller.
static void on_swipe(int dir_h, int dir_v, void * /*user_data*/)
{
    nav_swipe(dir_h, dir_v);
}

// ── Screen entry point ────────────────────────────────────────────────────────

// IN: nothing. OUT: new screen lv_obj_t* (not loaded — caller calls lv_screen_load/anim).
lv_obj_t *studio_one_screen_create()
{
    lv_obj_t *scr = theme_make_screen();

    statusbar_set_screen_name("STUDIO ONE");

    // ── Status bar zone: title + connection pill ──────────────────────────────
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "STUDIO ONE");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LABEL, 0);
    lv_obj_set_pos(title, 20, 8);

    lv_obj_t *conn_pill = lv_obj_create(scr);
    lv_obj_remove_style_all(conn_pill);
    lv_obj_set_size(conn_pill, 136, 22);
    lv_obj_set_pos(conn_pill, 800 - 136 - 20, 8);
    lv_obj_set_style_bg_color(conn_pill, lv_color_hex(0x707070), 0);
    lv_obj_set_style_bg_opa(conn_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(conn_pill, 11, 0);
    lv_obj_clear_flag(conn_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *conn_lbl = lv_label_create(conn_pill);
    lv_label_set_text(conn_lbl, "Disconnected");
    lv_obj_set_style_text_color(conn_lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(conn_lbl, THEME_FONT_HINT, 0);
    lv_obj_center(conn_lbl);

    // ── BPM card (centre-left) ────────────────────────────────────────────────
    lv_obj_t *bpm_card = lv_obj_create(scr);
    lv_obj_remove_style_all(bpm_card);
    lv_obj_set_size(bpm_card, 220, 200);
    lv_obj_align(bpm_card, LV_ALIGN_CENTER, -160, -30);
    lv_obj_set_style_bg_color(bpm_card, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(bpm_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bpm_card, THEME_RADIUS, 0);
    lv_obj_clear_flag(bpm_card, LV_OBJ_FLAG_SCROLLABLE);
    theme_apply_glow(bpm_card);

    lv_obj_t *bpm_hint = lv_label_create(bpm_card);
    lv_label_set_text(bpm_hint, "TEMPO");
    lv_obj_set_style_text_color(bpm_hint, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(bpm_hint, THEME_FONT_HINT, 0);
    lv_obj_align(bpm_hint, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *bpm_val = lv_label_create(bpm_card);
    lv_label_set_text(bpm_val, "---");
    lv_obj_set_style_text_color(bpm_val, THEME_TEXT_TITLE, 0);
    lv_obj_set_style_text_font(bpm_val, THEME_FONT_TITLE, 0);
    lv_obj_align(bpm_val, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *bpm_unit = lv_label_create(bpm_card);
    lv_label_set_text(bpm_unit, "BPM");
    lv_obj_set_style_text_color(bpm_unit, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(bpm_unit, THEME_FONT_HINT, 0);
    lv_obj_align(bpm_unit, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *timesig = lv_label_create(bpm_card);
    lv_label_set_text(timesig, "4 / 4");
    lv_obj_set_style_text_color(timesig, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(timesig, THEME_FONT_HINT, 0);
    lv_obj_align(timesig, LV_ALIGN_BOTTOM_MID, 0, -14);

    // ── Position card (centre-right) ──────────────────────────────────────────
    lv_obj_t *pos_card = lv_obj_create(scr);
    lv_obj_remove_style_all(pos_card);
    lv_obj_set_size(pos_card, 280, 200);
    lv_obj_align(pos_card, LV_ALIGN_CENTER, 120, -30);
    lv_obj_set_style_bg_color(pos_card, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(pos_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pos_card, THEME_RADIUS, 0);
    lv_obj_clear_flag(pos_card, LV_OBJ_FLAG_SCROLLABLE);
    theme_apply_glow(pos_card);

    lv_obj_t *pos_hint = lv_label_create(pos_card);
    lv_label_set_text(pos_hint, "POSITION");
    lv_obj_set_style_text_color(pos_hint, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(pos_hint, THEME_FONT_HINT, 0);
    lv_obj_align(pos_hint, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *pos_val = lv_label_create(pos_card);
    lv_label_set_text(pos_val, "001.01.000");
    lv_obj_set_style_text_color(pos_val, THEME_TEXT_TITLE, 0);
    lv_obj_set_style_text_font(pos_val, THEME_FONT_TITLE, 0);
    lv_obj_align(pos_val, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *state_lbl = lv_label_create(pos_card);
    lv_label_set_text(state_lbl, LV_SYMBOL_STOP "  STOPPED");
    lv_obj_set_style_text_color(state_lbl, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(state_lbl, THEME_FONT_HINT, 0);
    lv_obj_align(state_lbl, LV_ALIGN_BOTTOM_MID, 0, -14);

    // ── Transport buttons ─────────────────────────────────────────────────────
    static const char *BTNS[]   = { LV_SYMBOL_PREV, LV_SYMBOL_STOP,
                                     LV_SYMBOL_PLAY, LV_SYMBOL_NEXT };
    constexpr int BTN_W = 72, BTN_H = 44, BTN_GAP = 12;
    const int total_w   = 4 * BTN_W + 3 * BTN_GAP;
    const int start_x   = (800 - total_w) / 2;

    for (int i = 0; i < 4; ++i) {
        lv_obj_t *tb = lv_btn_create(scr);
        lv_obj_set_size(tb, BTN_W, BTN_H);
        lv_obj_set_pos(tb, start_x + i * (BTN_W + BTN_GAP), 380);
        lv_obj_set_style_bg_color(tb, THEME_BG_CARD, 0);
        lv_obj_set_style_bg_color(tb, THEME_BG_CARD_HOVER, LV_STATE_PRESSED);
        lv_obj_t *tb_lbl = lv_label_create(tb);
        lv_label_set_text(tb_lbl, BTNS[i]);
        lv_obj_set_style_text_color(tb_lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_center(tb_lbl);
    }

    foot_create(scr);

    touch_nav_attach_2d(scr, on_swipe, nullptr);

    return scr;
}
