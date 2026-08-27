#include "studio_one.h"
#include "studio_one_data.h"
#include "ws_client.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/foot.h"
#include "screens/statusbar.h"
#include "lvgl.h"

// Transport/session display for Studio One DAW integration (Phase 6: WebSocket).
// Live data arrives from g_studio_one_queue written by ws_client.cpp.

// ── Widget refs shared with the timer callback ────────────────────────────────

struct ScreenWidgets {
    lv_obj_t *bpm_val;
    lv_obj_t *timesig;
    lv_obj_t *pos_val;
    lv_obj_t *state_lbl;
    lv_obj_t *conn_pill;
    lv_obj_t *conn_lbl;
};

// ── Timer callback — runs in LVGL task, safe to update widgets directly ───────

// IN: lv_timer_t* with user_data pointing to ScreenWidgets. OUT: nothing.
// Polls g_studio_one_queue (non-destructive) and refreshes all transport labels.
static void transport_timer_cb(lv_timer_t *t)
{
    auto *w = static_cast<ScreenWidgets*>(lv_timer_get_user_data(t));

    // Connection pill
    bool connected = ws_client_connected();
    lv_obj_set_style_bg_color(w->conn_pill,
        lv_color_hex(connected ? 0x2E7D32 : 0x707070), 0);
    lv_label_set_text(w->conn_lbl, connected ? "Connected" : "Disconnected");
    lv_obj_set_style_text_color(w->conn_lbl,
        connected ? lv_color_hex(0xA5D6A7) : THEME_TEXT_HINT, 0);

    // Transport data
    StudioOneState state = {};
    if (!g_studio_one_queue || xQueuePeek(g_studio_one_queue, &state, 0) != pdTRUE) {
        lv_label_set_text(w->bpm_val, "---");
        lv_label_set_text(w->timesig, "- / -");
        lv_label_set_text(w->pos_val, "---");
        lv_label_set_text(w->state_lbl, LV_SYMBOL_STOP "  STOPPED");
        lv_obj_set_style_text_color(w->state_lbl, THEME_TEXT_SECONDARY, 0);
        return;
    }

    // BPM
    if (state.bpm > 0.0f) {
        char bpm_buf[16];
        snprintf(bpm_buf, sizeof(bpm_buf), "%.1f", state.bpm);
        lv_label_set_text(w->bpm_val, bpm_buf);
    } else {
        lv_label_set_text(w->bpm_val, "---");
    }

    lv_label_set_text(w->timesig, state.timesig[0] ? state.timesig : "4 / 4");
    lv_label_set_text(w->pos_val, state.pos[0] ? state.pos : "---");

    switch (state.state) {
    case TransportState::Playing:
        lv_label_set_text(w->state_lbl, LV_SYMBOL_PLAY "  PLAYING");
        lv_obj_set_style_text_color(w->state_lbl, lv_color_hex(0x4CAF50), 0);
        break;
    case TransportState::Recording:
        lv_label_set_text(w->state_lbl, LV_SYMBOL_AUDIO "  REC");
        lv_obj_set_style_text_color(w->state_lbl, lv_color_hex(0xFF4444), 0);
        break;
    case TransportState::Paused:
        lv_label_set_text(w->state_lbl, LV_SYMBOL_PAUSE "  PAUSED");
        lv_obj_set_style_text_color(w->state_lbl, THEME_TEXT_HINT, 0);
        break;
    default:
        lv_label_set_text(w->state_lbl, LV_SYMBOL_STOP "  STOPPED");
        lv_obj_set_style_text_color(w->state_lbl, THEME_TEXT_SECONDARY, 0);
        break;
    }
}

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

    // Persist widget refs for the timer; lives as long as the screen exists.
    static ScreenWidgets s_widgets = {};

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

    s_widgets.conn_pill = conn_pill;
    s_widgets.conn_lbl  = conn_lbl;

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

    s_widgets.bpm_val = bpm_val;
    s_widgets.timesig = timesig;

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
    lv_label_set_text(pos_val, "---");
    lv_obj_set_style_text_color(pos_val, THEME_TEXT_TITLE, 0);
    lv_obj_set_style_text_font(pos_val, THEME_FONT_TITLE, 0);
    lv_obj_align(pos_val, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t *state_lbl = lv_label_create(pos_card);
    lv_label_set_text(state_lbl, LV_SYMBOL_STOP "  STOPPED");
    lv_obj_set_style_text_color(state_lbl, THEME_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(state_lbl, THEME_FONT_HINT, 0);
    lv_obj_align(state_lbl, LV_ALIGN_BOTTOM_MID, 0, -14);

    s_widgets.pos_val   = pos_val;
    s_widgets.state_lbl = state_lbl;

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

    // 33 ms ≈ 30 Hz — matches UDP data rate
    lv_timer_create(transport_timer_cb, 33, &s_widgets);

    return scr;
}
