#include "settings.h"
#include "theme.h"
#include "screens/home.h"
#include "screens/touch_nav.h"
#include "screens/nav_controller.h"
#include "screens/gradient_test.h"
#include "screens/foot.h"
#include "screens/statusbar.h"
#include "wifi.h"
#include "audio_data.h"
#include "lvgl.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <cstdio>

// ── Layout constants ──────────────────────────────────────────────────────────

static constexpr int PAD      = 20;
static constexpr int CARD_W   = (800 - 3 * PAD) / 2;   // 370
static constexpr int CARD_H   = 160;
static constexpr int ROW1_Y   = 52;                       // below statusbar
static constexpr int ROW2_Y   = ROW1_Y + CARD_H + PAD;   // 232
static constexpr int COL2_X   = PAD + CARD_W + PAD;      // 410

// ── State ─────────────────────────────────────────────────────────────────────

struct SettingsData {
    lv_obj_t   *wifi_dot    = nullptr;
    lv_obj_t   *wifi_status = nullptr;
    lv_obj_t   *wifi_ip     = nullptr;
    lv_obj_t   *audio_dot   = nullptr;
    lv_obj_t   *audio_mode  = nullptr;
    lv_obj_t   *audio_seq   = nullptr;
    lv_obj_t   *sys_heap    = nullptr;
    lv_timer_t *timer       = nullptr;
};

// ── Timer: refresh live data at 1Hz ──────────────────────────────────────────

// Called at 1Hz from LVGL timer (already locked). Reads wifi_is_connected() and g_audio_queue.
static void settings_timer_cb(lv_timer_t *t)
{
    auto *d = static_cast<SettingsData*>(lv_timer_get_user_data(t));

    // WiFi
    bool connected = wifi_is_connected();
    lv_obj_set_style_bg_color(d->wifi_dot,
        lv_color_hex(connected ? 0x3B82F6u : 0x707070u), 0);
    if (connected) {
        lv_label_set_text(d->wifi_status, "Connected");
    } else {
        int reason = wifi_get_disconnect_reason();
        char reason_buf[32];
        if (reason) snprintf(reason_buf, sizeof(reason_buf), "DC reason=%d", reason);
        else        snprintf(reason_buf, sizeof(reason_buf), "Disconnected");
        lv_label_set_text(d->wifi_status, reason_buf);
    }
    char ip_str[18];
    wifi_get_ip(ip_str, sizeof(ip_str));
    lv_label_set_text(d->wifi_ip, ip_str);

    // Audio queue: peek latest packet (non-destructive)
    AudioPacket pkt{};
    bool live = (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) && (pkt.magic == 0xAB);
    lv_obj_set_style_bg_color(d->audio_dot,
        lv_color_hex(live ? 0x22C55Eu : 0x707070u), 0);
    lv_label_set_text(d->audio_mode, live ? "Live  UDP 4210" : "Demo  (no signal)");

    if (live) {
        char buf[24];
        snprintf(buf, sizeof(buf), "seq %lu", (unsigned long)pkt.seq);
        lv_label_set_text(d->audio_seq, buf);
    } else {
        lv_label_set_text(d->audio_seq, "");
    }

    // Free heap
    char heap_buf[24];
    snprintf(heap_buf, sizeof(heap_buf), "%lu kB free",
             (unsigned long)(esp_get_free_heap_size() / 1024));
    lv_label_set_text(d->sys_heap, heap_buf);
}

static void on_delete(lv_event_t *e)
{
    auto *d = static_cast<SettingsData*>(lv_event_get_user_data(e));
    if (d->timer) lv_timer_delete(d->timer);
    delete d;
}

// ── Navigation ────────────────────────────────────────────────────────────────

// IN: dir_h, dir_v from 2D swipe. OUT: delegates to nav_controller.
static void on_swipe(int dir_h, int dir_v, void * /*user_data*/)
{
    nav_swipe(dir_h, dir_v);
}

// ── Widget helpers ────────────────────────────────────────────────────────────

// IN: parent, pos (x,y), size (w,h), title string. OUT: styled card container.
static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, THEME_BG_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(card);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, THEME_TEXT_HINT, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
    lv_obj_set_pos(lbl, 12, 8);
    return card;
}

// IN: parent card, relative pos (rx, ry). OUT: small status-dot lv_obj_t (initially gray).
static lv_obj_t *make_dot(lv_obj_t *card, int rx, int ry)
{
    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_pos(dot, rx, ry);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x707070), 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    return dot;
}

// IN: parent, text, font, color, pos (rx, ry). OUT: label (LVGL-owned).
static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                             const lv_font_t *font, lv_color_t color, int rx, int ry)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_pos(lbl, rx, ry);
    return lbl;
}

// ── Screen entry point ────────────────────────────────────────────────────────

// IN: nothing. OUT: new screen lv_obj_t* (not loaded — caller calls lv_screen_load/anim).
lv_obj_t *settings_screen_create()
{
    auto *d = new SettingsData{};
    lv_obj_t *scr = theme_make_screen();
    lv_obj_add_event_cb(scr, on_delete, LV_EVENT_DELETE, d);

    statusbar_set_screen_name("SETTINGS");

    // Title
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, THEME_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LABEL, 0);
    lv_obj_set_pos(title, PAD, 8);

    // ── WiFi card ─────────────────────────────────────────────────────────────
    lv_obj_t *c_wifi = make_card(scr, PAD, ROW1_Y, CARD_W, CARD_H, "NETWORK");
    theme_apply_glow(c_wifi);
    d->wifi_dot    = make_dot(c_wifi, 12, 36);
    d->wifi_status = make_label(c_wifi, "...", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 30, 30);
    d->wifi_ip     = make_label(c_wifi, "--", THEME_FONT_HINT, THEME_TEXT_PRIMARY, 30, 62);
    make_label(c_wifi, "WiFi Station", THEME_FONT_HINT, THEME_TEXT_HINT, 12, 88);

    // Reconnect button — bottom-right of card
    {
        lv_obj_t *btn = lv_btn_create(c_wifi);
        lv_obj_set_size(btn, 110, 32);
        lv_obj_set_pos(btn, CARD_W - 118, 118);
        lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
        lv_obj_set_style_bg_color(btn, THEME_ACCENT_DIM, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, THEME_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 2, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, THEME_RADIUS, 0);
        lv_obj_add_event_cb(btn, [](lv_event_t *) { wifi_reconnect(); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, LV_SYMBOL_REFRESH " RECONNECT");
        lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
        lv_obj_center(lbl);
    }

    // ── Audio card ────────────────────────────────────────────────────────────
    lv_obj_t *c_audio = make_card(scr, COL2_X, ROW1_Y, CARD_W, CARD_H, "AUDIO IN");
    theme_apply_glow(c_audio);
    d->audio_dot  = make_dot(c_audio, 12, 36);
    d->audio_mode = make_label(c_audio, "...", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 30, 30);
    d->audio_seq  = make_label(c_audio, "", THEME_FONT_HINT, THEME_TEXT_HINT, 12, 72);

    // ── System card ───────────────────────────────────────────────────────────
    lv_obj_t *c_sys = make_card(scr, PAD, ROW2_Y, CARD_W, CARD_H, "SYSTEM");
    theme_apply_glow(c_sys);
    d->sys_heap = make_label(c_sys, "--- kB", THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 32);
    make_label(c_sys, IDF_VER, THEME_FONT_HINT, THEME_TEXT_HINT, 12, 72);

    // ── Build card ────────────────────────────────────────────────────────────
    lv_obj_t *c_build = make_card(scr, COL2_X, ROW2_Y, CARD_W, CARD_H, "BUILD");
    theme_apply_glow(c_build);
    make_label(c_build, __DATE__, THEME_FONT_LABEL, THEME_TEXT_PRIMARY, 12, 32);
    make_label(c_build, __TIME__, THEME_FONT_HINT, THEME_TEXT_HINT, 12, 72);

    // Foot bar with Home button; add Gradient → button in right_zone
    lv_obj_t *right_zone = foot_create(scr);

    lv_obj_t *grad_btn = lv_btn_create(right_zone);
    lv_obj_set_size(grad_btn, 120, 40);
    lv_obj_align(grad_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(grad_btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(grad_btn, [](lv_event_t *) {
        lv_screen_load_anim(gradient_test_screen_create(), LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, true);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *grad_lbl = lv_label_create(grad_btn);
    lv_label_set_text(grad_lbl, "Gradient " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(grad_lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(grad_lbl);

    touch_nav_attach_2d(scr, on_swipe, nullptr);

    d->timer = lv_timer_create(settings_timer_cb, 1000, d);
    settings_timer_cb(d->timer);  // populate immediately on screen open

    return scr;
}
